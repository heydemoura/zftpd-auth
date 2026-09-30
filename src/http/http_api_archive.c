#include "http_api_internal.h"
#include "archive_path.h"
#include "archive/zip_writer.h"
#include "builtin_unzip.h"
#include "ftp_config.h"
#include "http_config.h"
#include "pal_limits.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

typedef struct {
  _Atomic int active;
  _Atomic int done;
  _Atomic int cancelled;
  _Atomic int error;
  _Atomic uint64_t bytes_extracted;
  _Atomic uint64_t total_bytes;
  char archive_path[PAL_PATH_MAX];
  char dest_path[PAL_PATH_MAX];
  char error_msg[256];
} extract_state_t;

static extract_state_t g_extract;

static void extract_mark_error(const char *message) {
  if (message == NULL) message = "Extraction failed";
  (void)snprintf(g_extract.error_msg, sizeof(g_extract.error_msg), "%s", message);
  atomic_store_explicit(&g_extract.error, 1, memory_order_release);
}

static void extract_finish(void) {
  if (atomic_load_explicit(&g_extract.cancelled, memory_order_relaxed) != 0 &&
      atomic_load_explicit(&g_extract.error, memory_order_acquire) == 0)
    (void)snprintf(g_extract.error_msg, sizeof(g_extract.error_msg), "Cancelled");
  atomic_store_explicit(&g_extract.done, 1, memory_order_release);
  atomic_store_explicit(&g_extract.active, 0, memory_order_release);
}

static int spawn_extract_thread(void *(*entry)(void *)) {
  pthread_t tid;
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) == 0) {
    (void)pthread_attr_setstacksize(&attr, (size_t)HTTP_THREAD_STACK_SIZE);
    (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, entry, NULL);
    (void)pthread_attr_destroy(&attr);
    return rc;
  }
  int rc = pthread_create(&tid, NULL, entry, NULL);
  if (rc == 0) (void)pthread_detach(tid);
  return rc;
}

static int path_has_extension(const char *path, const char *ext) {
  if (path == NULL || ext == NULL) return 0;
  const char *dot = strrchr(path, '.');
  return dot != NULL && strcasecmp(dot, ext) == 0;
}

static void *extract_thread_builtin(void *arg) {
  (void)arg;
  int rc = builtin_unzip(g_extract.archive_path, g_extract.dest_path,
                         &g_extract.cancelled, g_extract.error_msg,
                         sizeof(g_extract.error_msg));
  if (rc != 0 &&
      atomic_load_explicit(&g_extract.cancelled, memory_order_relaxed) == 0) {
    if (g_extract.error_msg[0] == '\0')
      extract_mark_error("Extraction failed");
    else
      atomic_store_explicit(&g_extract.error, 1, memory_order_release);
  }
  extract_finish();
  return NULL;
}

#if defined(ENABLE_LIBARCHIVE) && ENABLE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>

static void extract_archive_error(struct archive *a, const char *prefix) {
  const char *detail = a != NULL ? archive_error_string(a) : NULL;
  if (detail == NULL) detail = "unknown error";
  (void)snprintf(g_extract.error_msg, sizeof(g_extract.error_msg), "%s: %s",
                 prefix, detail);
  atomic_store_explicit(&g_extract.error, 1, memory_order_release);
}

static void *extract_thread_libarchive(void *arg) {
  (void)arg;
  struct archive *reader = archive_read_new();
  struct archive *writer = archive_write_disk_new();
  if (reader == NULL || writer == NULL) {
    extract_mark_error("Failed to initialize libarchive");
    if (reader != NULL) archive_read_free(reader);
    if (writer != NULL) archive_write_free(writer);
    extract_finish();
    return NULL;
  }

  archive_read_support_format_all(reader);
  archive_read_support_filter_all(reader);
  int options = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS;
#ifdef ARCHIVE_EXTRACT_SECURE_SYMLINKS
  options |= ARCHIVE_EXTRACT_SECURE_SYMLINKS;
#endif
#ifdef ARCHIVE_EXTRACT_SECURE_NODOTDOT
  options |= ARCHIVE_EXTRACT_SECURE_NODOTDOT;
#endif
  archive_write_disk_set_options(writer, options);
  archive_write_disk_set_standard_lookup(writer);

  if (archive_read_open_filename(reader, g_extract.archive_path, 65536) !=
      ARCHIVE_OK) {
    extract_archive_error(reader, "Cannot open archive");
    goto cleanup;
  }

  struct archive_entry *entry = NULL;
  while (atomic_load_explicit(&g_extract.cancelled, memory_order_relaxed) == 0) {
    int rc = archive_read_next_header(reader, &entry);
    if (rc == ARCHIVE_EOF) break;
    if (rc != ARCHIVE_OK && rc != ARCHIVE_WARN) {
      extract_archive_error(reader, "Archive read failed");
      break;
    }

    const char *name = archive_entry_pathname(entry);
    mode_t type = archive_entry_filetype(entry);
    if (name == NULL || name[0] == '\0' || archive_entry_symlink(entry) != NULL ||
        archive_entry_hardlink(entry) != NULL ||
        (type != AE_IFREG && type != AE_IFDIR)) {
      extract_mark_error("Archive contains an unsupported entry type");
      break;
    }

    char fullpath[PAL_PATH_MAX];
    if (archive_path_prepare(g_extract.dest_path, name, strlen(name),
                             type == AE_IFDIR, fullpath,
                             sizeof(fullpath)) != 0) {
      extract_mark_error("Archive entry path is unsafe");
      break;
    }
    archive_entry_set_pathname(entry, fullpath);

    if (archive_write_header(writer, entry) != ARCHIVE_OK) {
      extract_archive_error(writer, "Archive write header failed");
      break;
    }

    if (type == AE_IFREG) {
      const void *buffer = NULL;
      size_t size = 0U;
      int64_t offset = 0;
      for (;;) {
        if (atomic_load_explicit(&g_extract.cancelled, memory_order_relaxed) != 0)
          break;
        rc = archive_read_data_block(reader, &buffer, &size, &offset);
        if (rc == ARCHIVE_EOF) break;
        if (rc != ARCHIVE_OK) {
          extract_archive_error(reader, "Archive data read failed");
          break;
        }
        if (archive_write_data_block(writer, buffer, size, offset) != ARCHIVE_OK) {
          extract_archive_error(writer, "Archive data write failed");
          break;
        }
        atomic_fetch_add_explicit(&g_extract.bytes_extracted, (uint64_t)size,
                                  memory_order_relaxed);
      }
      if (atomic_load_explicit(&g_extract.error, memory_order_acquire) != 0)
        break;
    }

    if (archive_write_finish_entry(writer) != ARCHIVE_OK) {
      extract_archive_error(writer, "Archive entry finalization failed");
      break;
    }
  }

cleanup:
  (void)archive_read_close(reader);
  (void)archive_read_free(reader);
  (void)archive_write_close(writer);
  (void)archive_write_free(writer);
  extract_finish();
  return NULL;
}
#endif

static void extract_state_begin(const char *archive_path, const char *dest_path,
                                uint64_t total_bytes) {
  size_t archive_len = strlen(archive_path);
  size_t dest_len = strlen(dest_path);
  memcpy(g_extract.archive_path, archive_path, archive_len + 1U);
  memcpy(g_extract.dest_path, dest_path, dest_len + 1U);
  g_extract.error_msg[0] = '\0';
  atomic_store_explicit(&g_extract.done, 0, memory_order_relaxed);
  atomic_store_explicit(&g_extract.cancelled, 0, memory_order_relaxed);
  atomic_store_explicit(&g_extract.error, 0, memory_order_relaxed);
  atomic_store_explicit(&g_extract.bytes_extracted, 0U, memory_order_relaxed);
  atomic_store_explicit(&g_extract.total_bytes, total_bytes, memory_order_relaxed);
  atomic_store_explicit(&g_extract.active, 1, memory_order_release);
}

http_response_t *http_api_archive_start(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  if (atomic_load_explicit(&g_extract.active, memory_order_acquire) != 0)
    return http_api_error_json(HTTP_STATUS_409_CONFLICT,
                               "Extraction already in progress");

  const char *query = strchr(request->uri, '?');
  char path[PAL_PATH_MAX] = "";
  char dst[PAL_PATH_MAX] = "/";
  if (query == NULL || http_api_parse_path_param(query, path, sizeof(path)) != 0)
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "Missing or invalid path parameter");
  if (http_api_query_has_param(query, "dst") &&
      http_api_parse_query_param(query, "dst", dst, sizeof(dst)) != 0)
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "Invalid destination parameter");

  char safe_path[PAL_PATH_MAX];
  char safe_dst[PAL_PATH_MAX];
  if (!http_api_validate_path(path, safe_path, sizeof(safe_path)) ||
      !http_api_validate_path(dst, safe_dst, sizeof(safe_dst)))
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN,
                               "Path traversal blocked");

  struct stat st;
  if (stat(safe_path, &st) != 0 || !S_ISREG(st.st_mode))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "Archive path is not a regular file");

#if !defined(ENABLE_LIBARCHIVE) || !ENABLE_LIBARCHIVE
  if (!path_has_extension(safe_path, ".zip"))
    return http_api_error_json(HTTP_STATUS_415_UNSUPPORTED_MEDIA_TYPE,
                               "This build can extract ZIP files only");
#endif

  extract_state_begin(safe_path, safe_dst, (uint64_t)st.st_size);
#if defined(ENABLE_LIBARCHIVE) && ENABLE_LIBARCHIVE
  int rc = spawn_extract_thread(path_has_extension(safe_path, ".zip")
                                ? extract_thread_builtin
                                : extract_thread_libarchive);
#else
  int rc = spawn_extract_thread(extract_thread_builtin);
#endif
  if (rc != 0) {
    atomic_store_explicit(&g_extract.active, 0, memory_order_release);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                               "Failed to start extraction thread");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  static const char body[] = "{\"ok\":true,\"message\":\"Extraction started\"}";
  (void)http_response_set_body(resp, body, sizeof(body) - 1U);
  return resp;
}

http_response_t *http_api_archive_progress(const http_request_t *request) {
  (void)request;
  int active = atomic_load_explicit(&g_extract.active, memory_order_acquire);
  int done = atomic_load_explicit(&g_extract.done, memory_order_acquire);
  int cancelled = atomic_load_explicit(&g_extract.cancelled, memory_order_relaxed);
  int error = atomic_load_explicit(&g_extract.error, memory_order_acquire);
  uint64_t bytes = atomic_load_explicit(&g_extract.bytes_extracted,
                                        memory_order_relaxed);
  uint64_t total = atomic_load_explicit(&g_extract.total_bytes,
                                        memory_order_relaxed);

  char body[768];
  size_t pos = 0U;
  if (http_buf_append_cstr(body, sizeof(body), &pos, "{\"active\":") != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos,
                           active ? "true" : "false") != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos, ",\"done\":") != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos, done ? "true" : "false") != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos, ",\"cancelled\":") != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos,
                           cancelled ? "true" : "false") != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos, ",\"error\":") != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos, error ? "true" : "false") != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos,
                           ",\"bytes_extracted\":") != 0 ||
      http_buf_append_u64(body, sizeof(body), &pos, bytes) != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos, ",\"total_bytes\":") != 0 ||
      http_buf_append_u64(body, sizeof(body), &pos, total) != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos, ",\"error_msg\":\"") != 0)
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                               "Failed to serialize extraction progress");

  if ((done || error) &&
      http_json_escape_append(body, sizeof(body), &pos, g_extract.error_msg) != 0)
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                               "Failed to serialize extraction progress");
  if (http_buf_append_cstr(body, sizeof(body), &pos, "\"}") != 0)
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                               "Failed to serialize extraction progress");

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  (void)http_response_set_body(resp, body, pos);
  return resp;
}

http_response_t *http_api_archive_cancel(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  atomic_store_explicit(&g_extract.cancelled, 1, memory_order_relaxed);
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) return NULL;
  static const char body[] = "{\"ok\":true}";
  http_response_add_header(resp, "Content-Type", "application/json");
  (void)http_response_set_body(resp, body, sizeof(body) - 1U);
  return resp;
}

/*===========================================================================*
 * BULK DOWNLOAD — /api/archive/zip
 *
 * The selection is collected into a streaming ZIP: entries are produced while
 * the response is sent, so no temporary archive is written and multi-gigabyte
 * selections cost no extra disk space.
 *===========================================================================*/

#define ZIP_MAX_PATHS 64

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/** Decode a query value into @p dst (%XX escapes). */
static void decode_value(char *dst, size_t dst_size, const char *src,
                         size_t len) {
  size_t di = 0U;
  for (size_t si = 0U; si < len && di + 1U < dst_size; si++) {
    if (src[si] == '%' && si + 2U < len) {
      int hi = hex_digit(src[si + 1U]);
      int lo = hex_digit(src[si + 2U]);
      if (hi >= 0 && lo >= 0) {
        dst[di++] = (char)((hi << 4) | lo);
        si += 2U;
        continue;
      }
    }
    dst[di++] = src[si];
  }
  dst[di] = '\0';
}

/**
 * Collect every `path=` value, in order, into @p storage (max_paths slots of
 * FTP_PATH_MAX bytes each).
 *
 * @return Number of paths stored, with pointers written to @p out.
 */
static size_t collect_query_paths(const char *query, char *storage,
                                  size_t max_paths, char **out) {
  if (query == NULL || storage == NULL) return 0U;

  size_t count = 0U;
  const char *p = query;
  while (*p != '\0' && count < max_paths) {
    while (*p == '?' || *p == '&') p++;
    if (strncmp(p, "path=", 5U) == 0) {
      p += 5;
      size_t len = strcspn(p, "&");
      char *dst = storage + count * FTP_PATH_MAX;
      decode_value(dst, FTP_PATH_MAX, p, len);
      if (dst[0] != '\0') {
        out[count++] = dst;
      }
      p += len;
      continue;
    }
    p += strcspn(p, "&");
  }
  return count;
}

static ssize_t zip_stream_read(void *ctx, void *buf, size_t len) {
  return zip_writer_read((zip_writer_t *)ctx, buf, len);
}

static http_response_t *zip_response_for(zip_writer_t *zip, const char *name);

/*----------------------------------------------------------------------------*
 * Prepared archives
 *
 * A bulk selection is a long list of paths: far more than the 2 KB request
 * URI the parser accepts.  The UI therefore POSTs the selection once and then
 * downloads a short /api/archive/zip?id=N URL.  Slots are single use and
 * expire so a client that never fetches cannot pin memory.
 *----------------------------------------------------------------------------*/

#define ZIP_PENDING_SLOTS 4
#define ZIP_PENDING_TTL_SECONDS 300

typedef struct {
  int id;
  zip_writer_t *zip;
  char name[128];
  time_t created;
} zip_pending_t;

static zip_pending_t g_zip_pending[ZIP_PENDING_SLOTS];
static int g_zip_next_id = 1;

static void zip_pending_expire(void) {
  time_t now = time(NULL);
  for (size_t i = 0U; i < ZIP_PENDING_SLOTS; i++) {
    zip_pending_t *slot = &g_zip_pending[i];
    if (slot->zip != NULL &&
        difftime(now, slot->created) > (double)ZIP_PENDING_TTL_SECONDS) {
      zip_writer_destroy(slot->zip);
      slot->zip = NULL;
      slot->id = 0;
    }
  }
}

static int zip_pending_store(zip_writer_t *zip, const char *name) {
  zip_pending_expire();
  for (size_t i = 0U; i < ZIP_PENDING_SLOTS; i++) {
    if (g_zip_pending[i].zip == NULL) {
      g_zip_pending[i].zip = zip;
      g_zip_pending[i].id = g_zip_next_id++;
      g_zip_pending[i].created = time(NULL);
      (void)snprintf(g_zip_pending[i].name, sizeof(g_zip_pending[i].name), "%s",
                     name);
      return g_zip_pending[i].id;
    }
  }
  return 0;
}

static zip_writer_t *zip_pending_take(int id, char *name, size_t name_size) {
  for (size_t i = 0U; i < ZIP_PENDING_SLOTS; i++) {
    zip_pending_t *slot = &g_zip_pending[i];
    if (slot->zip != NULL && slot->id == id) {
      zip_writer_t *zip = slot->zip;
      if (name != NULL && name_size > 0U)
        (void)snprintf(name, name_size, "%s", slot->name);
      slot->zip = NULL;
      slot->id = 0;
      return zip;
    }
  }
  return NULL;
}

static int json_unescape(char *dst, size_t dst_size, const char *src,
                         size_t len) {
  size_t di = 0U;
  for (size_t si = 0U; si < len && di + 1U < dst_size; si++) {
    if (src[si] == '\\' && si + 1U < len) {
      si++;
      switch (src[si]) {
      case 'n': dst[di++] = '\n'; break;
      case 't': dst[di++] = '\t'; break;
      case 'r': dst[di++] = '\r'; break;
      default: dst[di++] = src[si]; break;
      }
      continue;
    }
    dst[di++] = src[si];
  }
  dst[di] = '\0';
  return (int)di;
}

/**
 * Read a JSON string array (body must contain "key": ["a","b"]).
 *
 * @return Number of entries stored in @p out (pointers into @p storage).
 */
static size_t json_string_array(const char *body, const char *key,
                                char *storage, size_t slot_size, size_t max,
                                char **out) {
  if (body == NULL || key == NULL) return 0U;

  char pattern[64];
  (void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(body, pattern);
  if (p == NULL) return 0U;
  p = strchr(p + strlen(pattern), '[');
  if (p == NULL) return 0U;
  p++;

  size_t count = 0U;
  while (*p != '\0' && *p != ']' && count < max) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
    if (*p != '"') break;
    p++;
    size_t len = 0U;
    while (p[len] != '\0' && p[len] != '"') {
      if (p[len] == '\\' && p[len + 1U] != '\0') len++;
      len++;
    }
    char *dst = storage + count * slot_size;
    (void)json_unescape(dst, slot_size, p, len);
    if (dst[0] != '\0') out[count++] = dst;
    p += len;
    if (*p == '"') p++;
  }
  return count;
}

static void zip_stream_close(void *ctx) {
  zip_writer_destroy((zip_writer_t *)ctx);
}

static http_response_t *api_archive_zip(const http_request_t *request) {
  if (request->method != HTTP_METHOD_GET) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                               "Use GET /api/archive/zip");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "Missing path parameters");
  }

  /* Short form used after POSTing a selection: /api/archive/zip?id=N */
  const char *id_param = strstr(query, "id=");
  if (id_param != NULL) {
    int id = atoi(id_param + 3);
    char pending_name[128] = {0};
    zip_writer_t *prepared = zip_pending_take(id, pending_name,
                                              sizeof(pending_name));
    if (prepared == NULL) {
      return http_api_error_json(HTTP_STATUS_404_NOT_FOUND,
                                 "Archive expired or already downloaded");
    }
    return zip_response_for(prepared, pending_name);
  }

  /* Requested values first, then their confined copies, in one allocation. */
  char *storage = malloc(2U * (size_t)ZIP_MAX_PATHS * FTP_PATH_MAX);
  if (storage == NULL) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                               "Out of memory");
  }

  char *requested[ZIP_MAX_PATHS];
  const char *safe[ZIP_MAX_PATHS];
  size_t count = collect_query_paths(query, storage, ZIP_MAX_PATHS, requested);

  size_t kept = 0U;
  for (size_t i = 0U; i < count; i++) {
    char *validated = storage + ((size_t)ZIP_MAX_PATHS + kept) * FTP_PATH_MAX;
    if (!http_api_validate_path(requested[i], validated, FTP_PATH_MAX)) {
      continue; /* unreadable or unconfined entry: skip, keep the rest */
    }
    safe[kept++] = validated;
  }
  if (kept == 0U) {
    free(storage);
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND,
                               "Nothing to archive in this selection");
  }

  zip_writer_t *zip = zip_writer_create(safe, kept);
  free(storage);
  if (zip == NULL || zip_writer_entry_count(zip) == 0U) {
    zip_writer_destroy(zip);
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND,
                               "Nothing to archive in this selection");
  }

  return zip_response_for(zip, NULL);
}

/**
 * @brief Build the archive response and hand the writer to the stream layer.
 *
 * @param name  Download name, or NULL to derive one from the timestamp.
 */
static http_response_t *zip_response_for(zip_writer_t *zip, const char *name) {
  if (zip == NULL) {
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND,
                               "Nothing to archive in this selection");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) {
    zip_writer_destroy(zip);
    return NULL;
  }

  char final_name[128];
  if (name != NULL && name[0] != '\0') {
    (void)snprintf(final_name, sizeof(final_name), "%s", name);
  } else {
    time_t now = time(NULL);
    struct tm tm_buf;
    if (localtime_r(&now, &tm_buf) == NULL) {
      (void)snprintf(final_name, sizeof(final_name), "zftpd-files.zip");
    } else {
      (void)strftime(final_name, sizeof(final_name), "zftpd-%Y%m%d-%H%M%S.zip",
                     &tm_buf);
    }
  }

  /*
   * Announce the exact length instead of using chunked encoding: console
   * browsers abort chunked downloads ("check your connection") and cannot
   * show progress without a size.
   */
  uint64_t archive_size = zip_writer_total_size(zip);
  char len_str[32];
  (void)snprintf(len_str, sizeof(len_str), "%" PRIu64, archive_size);

  http_response_add_header(resp, "Content-Type", "application/zip");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_add_header(resp, "Content-Length", len_str);
  http_response_add_header(resp, "Cache-Control", "no-store");
  char disposition[256];
  (void)snprintf(disposition, sizeof(disposition),
                 "attachment; filename=\"%s\"", final_name);
  http_response_add_header(resp, "Content-Disposition", disposition);
  if (zip_writer_truncated(zip) != 0) {
    http_response_add_header(resp, "X-Zftpd-Truncated", "entry-limit");
  }

  if (http_response_finalize(resp) != 0) {
    zip_writer_destroy(zip);
    http_response_destroy(resp);
    return NULL;
  }

  resp->stream_ctx = zip;
  resp->stream_read = zip_stream_read;
  resp->stream_close = zip_stream_close;
  resp->stream_chunked = 0; /* raw bytes, length already announced */
  return resp;
}

/**
 * @brief POST /api/archive/zip — prepare a bulk selection.
 *
 * Body: {"paths":["/a","/b",...],"name":"backup.zip"}
 * Reply: {"ok":true,"id":N,"name":"...","entries":M,"size":S}
 *
 * The client then downloads /api/archive/zip?id=N: keeping the path list out
 * of the URL is what makes large selections possible at all (the request URI
 * is capped well below the JSON body).
 */
static http_response_t *api_archive_zip_prepare(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                               "Use POST or GET");
  }
  if (request->body == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing JSON body");
  }

  char *storage = malloc(2U * (size_t)ZIP_MAX_PATHS * FTP_PATH_MAX);
  if (storage == NULL) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  char *requested[ZIP_MAX_PATHS];
  const char *safe[ZIP_MAX_PATHS];
  size_t count = json_string_array(request->body, "paths", storage,
                                   FTP_PATH_MAX, ZIP_MAX_PATHS, requested);

  size_t kept = 0U;
  for (size_t i = 0U; i < count; i++) {
    char *validated = storage + ((size_t)ZIP_MAX_PATHS + kept) * FTP_PATH_MAX;
    if (!http_api_validate_path(requested[i], validated, FTP_PATH_MAX)) continue;
    safe[kept++] = validated;
  }
  if (kept == 0U) {
    free(storage);
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND,
                               "Nothing to archive in this selection");
  }

  zip_writer_t *zip = zip_writer_create(safe, kept);

  /* "name" is a plain JSON string here, unlike the "paths" array. */
  char name[128] = {0};
  const char *name_key = strstr(request->body, "\"name\"");
  if (name_key != NULL) {
    const char *colon = strchr(name_key + 6U, ':');
    const char *quote = (colon != NULL) ? strchr(colon, '"') : NULL;
    if (quote != NULL) {
      const char *end = strchr(quote + 1U, '"');
      if (end != NULL) {
        (void)json_unescape(name, sizeof(name), quote + 1U,
                            (size_t)(end - quote - 1));
      }
    }
  }
  free(storage);

  if (zip == NULL || zip_writer_entry_count(zip) == 0U) {
    zip_writer_destroy(zip);
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND,
                               "Nothing to archive in this selection");
  }

  int id = zip_pending_store(zip, name);
  if (id == 0) {
    zip_writer_destroy(zip); /* too many prepared archives in flight */
    return http_api_error_json(HTTP_STATUS_409_CONFLICT,
                               "Too many prepared archives, retry shortly");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  char body[256];
  int len = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"id\":%d,\"entries\":%zu,\"size\":%" PRIu64
                     ",\"truncated\":%s}",
                     id, zip_writer_entry_count(zip),
                     zip_writer_total_size(zip),
                     zip_writer_truncated(zip) != 0 ? "true" : "false");
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

http_response_t *http_api_archive_handle(const http_request_t *request) {
  if (request == NULL) return NULL;
  if (http_api_route_is(request->uri, "/api/archive/zip")) {
    if (request->method == HTTP_METHOD_POST)
      return api_archive_zip_prepare(request);
    return api_archive_zip(request);
  }
  if (http_api_route_is(request->uri, "/api/extract_progress"))
    return http_api_archive_progress(request);
  if (http_api_route_is(request->uri, "/api/extract_cancel"))
    return http_api_archive_cancel(request);
  if (http_api_route_is(request->uri, "/api/extract"))
    return http_api_archive_start(request);
  return NULL;
}
