/* ══ BUILTIN UNZIP ═══════════════════════════════════════════════════════════
 * Self-contained ZIP extractor backed by the bundled miniz reader.
 *
 * No platform library dependencies — compiles and runs on PS4/PS5 without
 * libarchive or libz.
 *
 * Supported by miniz: Store (method 0), Deflate (method 8), Zip64, data
 * descriptors, and CRC validation.
 *
 * ZIP reference:  PKWARE APPNOTE.TXT v6.3.4
 * Deflate ref:    RFC 1951
 * ═════════════════════════════════════════════════════════════════════════ */

#include "builtin_unzip.h"
#include "archive_path.h"
#include "pal_limits.h"
#include "ftp_types.h"
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-align"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wredundant-decls"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wstrict-prototypes"
#pragma clang diagnostic ignored "-Wundef"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wwrite-strings"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wwrite-strings"
#endif
#include "../external/miniz/miniz.h"
#include "../external/miniz/miniz.c"
#include "../external/miniz/miniz_tinfl.c"
#include "../external/miniz/miniz_zip.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define ZIP_WRITE_BUF_SIZE (256U * 1024U)




/* Write callback: append to output fd */
typedef struct {
  int     fd;
  uint8_t buf[ZIP_WRITE_BUF_SIZE];
  size_t  buf_pos;
} write_ctx_t;

static int write_flush(write_ctx_t *wc) {
  if (wc->buf_pos == 0) return 0;
  size_t written = 0;
  while (written < wc->buf_pos) {
    ssize_t n = write(wc->fd, wc->buf + written, wc->buf_pos - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    written += (size_t)n;
  }
  wc->buf_pos = 0;
  return 0;
}

static int write_byte(void *userdata, const uint8_t *data, size_t len) {
  write_ctx_t *wc = (write_ctx_t *)userdata;
  size_t off = 0U;
  while (off < len) {
    size_t space = ZIP_WRITE_BUF_SIZE - wc->buf_pos;
    size_t chunk = len - off;
    if (chunk > space) {
      chunk = space;
    }
    memcpy(wc->buf + wc->buf_pos, data + off, chunk);
    wc->buf_pos += chunk;
    off += chunk;
    if (wc->buf_pos >= ZIP_WRITE_BUF_SIZE) {
      if (write_flush(wc) != 0) return -1;
    }
  }
  return 0;
}

typedef struct {
  int fd;
} unzip_read_ctx_t;

static size_t unzip_miniz_read(void *opaque, mz_uint64 file_ofs,
                               void *buf, size_t len) {
  unzip_read_ctx_t *ctx = (unzip_read_ctx_t *)opaque;
  uint8_t *out = (uint8_t *)buf;
  size_t done = 0U;

  if (ctx == NULL || ctx->fd < 0 || buf == NULL) {
    return 0U;
  }

  off_t off = (off_t)file_ofs;
  if ((mz_uint64)off != file_ofs) {
    return 0U;
  }

  while (done < len) {
    ssize_t n = pread(ctx->fd, out + done, len - done, off + (off_t)done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (n == 0) {
      break;
    }
    done += (size_t)n;
  }

  return done;
}

typedef struct {
  write_ctx_t   wc;
  _Atomic int *cancelled;
} unzip_write_ctx_t;

static size_t unzip_miniz_write(void *opaque, mz_uint64 file_ofs,
                                const void *buf, size_t len) {
  unzip_write_ctx_t *ctx = (unzip_write_ctx_t *)opaque;
  (void)file_ofs;

  if (ctx == NULL || buf == NULL) {
    return 0U;
  }
  if (ctx->cancelled != NULL &&
      atomic_load_explicit(ctx->cancelled, memory_order_relaxed) != 0) {
    return 0U;
  }

  return (write_byte(&ctx->wc, (const uint8_t *)buf, len) == 0) ? len : 0U;
}

/* Materialize a safe in-archive symlink as a regular file. This also works on
 * console USB filesystems that cannot store symlinks. Links escaping the
 * extraction tree, linking to another symlink or targeting a hardlink fail. */
static int materialize_zip_link(mz_zip_archive *zip, mz_uint index,
                                const char *dest_dir, const char *entry_name,
                                const mz_zip_archive_file_stat *file_stat,
                                _Atomic int *cancelled, char *error_msg,
                                size_t error_msg_size) {
  if (file_stat->m_uncomp_size == 0U ||
      file_stat->m_uncomp_size >= PAL_PATH_MAX) goto unsafe_link;
  size_t target_len = 0U;
  char *target = mz_zip_reader_extract_to_heap(zip, index, &target_len, 0);
  if (target == NULL) goto unsafe_link;
  int safe = target_len == (size_t)file_stat->m_uncomp_size &&
             archive_path_is_safe_relative(target, target_len);
  if (!safe) { mz_free(target); goto unsafe_link; }

  char source_entry[PAL_PATH_MAX];
  const char *slash = strrchr(entry_name, '/');
  size_t prefix = slash != NULL ? (size_t)(slash - entry_name + 1) : 0U;
  if (prefix + target_len >= sizeof(source_entry)) {
    mz_free(target); goto unsafe_link;
  }
  memcpy(source_entry, entry_name, prefix);
  memcpy(source_entry + prefix, target, target_len);
  source_entry[prefix + target_len] = '\0';
  mz_free(target);

  int target_is_archived = 0;
  mz_uint num_files = mz_zip_reader_get_num_files(zip);
  for (mz_uint j = 0U; j < num_files; j++) {
    char candidate[PAL_PATH_MAX];
    mz_uint needed = mz_zip_reader_get_filename(zip, j, NULL, 0);
    if (needed == 0U || needed > (mz_uint)sizeof(candidate)) continue;
    (void)mz_zip_reader_get_filename(zip, j, candidate,
                                     (mz_uint)sizeof(candidate));
    if (strcmp(candidate, source_entry) != 0) continue;
    mz_zip_archive_file_stat candidate_stat;
    if (mz_zip_reader_file_stat(zip, j, &candidate_stat) &&
        !candidate_stat.m_is_directory && candidate_stat.m_is_supported &&
        (((mode_t)(candidate_stat.m_external_attr >> 16)) & S_IFMT) != S_IFLNK)
      target_is_archived = 1;
    break;
  }
  if (!target_is_archived) goto unsafe_link;

  char source[PAL_PATH_MAX], output[PAL_PATH_MAX];
  if (archive_path_prepare(dest_dir, source_entry, strlen(source_entry), 0,
                           source, sizeof(source)) != 0 ||
      archive_path_prepare(dest_dir, entry_name, strlen(entry_name), 0,
                           output, sizeof(output)) != 0 ||
      strcmp(source, output) == 0)
    goto unsafe_link;

  int in_flags = O_RDONLY;
#ifdef O_NOFOLLOW
  in_flags |= O_NOFOLLOW;
#endif
  int input = open(source, in_flags);
  struct stat input_st;
  if (input < 0 || fstat(input, &input_st) != 0 ||
      !S_ISREG(input_st.st_mode) || input_st.st_nlink != 1) {
    if (input >= 0) (void)close(input);
    goto unsafe_link;
  }
  if (unlink(output) != 0 && errno != ENOENT) {
    (void)close(input);
    goto unsafe_link;
  }
  int out_flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
  out_flags |= O_NOFOLLOW;
#endif
  int output_fd = open(output, out_flags, 0666);
  if (output_fd < 0) { (void)close(input); goto unsafe_link; }
  int result = 0;
  unsigned char buf[64U * 1024U];
  for (;;) {
    if (cancelled != NULL &&
        atomic_load_explicit(cancelled, memory_order_relaxed) != 0) {
      result = -1;
      break;
    }
    ssize_t n = read(input, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) result = -1;
    if (n <= 0) break;
    size_t off = 0U;
    while (off < (size_t)n) {
      ssize_t written = write(output_fd, buf + off, (size_t)n - off);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) { result = -1; break; }
      off += (size_t)written;
    }
    if (result != 0) break;
  }
  if (close(input) != 0) result = -1;
  if (close(output_fd) != 0) result = -1;
  if (result != 0) {
    (void)unlink(output);
    if (error_msg != NULL)
      snprintf(error_msg, error_msg_size, "Cannot copy ZIP link: %s", entry_name);
  }
  return result;

unsafe_link:
  if (error_msg != NULL)
    snprintf(error_msg, error_msg_size, "Unsafe or missing ZIP link target: %s",
             entry_name);
  return -1;
}


int builtin_unzip(const char *zip_path, const char *dest_dir,
                  _Atomic int *cancelled,
                  char *error_msg, size_t error_msg_size) {

  if (error_msg && error_msg_size > 0) error_msg[0] = '\0';

  int zip_fd = open(zip_path, O_RDONLY);
  if (zip_fd < 0) {
    if (error_msg) snprintf(error_msg, error_msg_size,
                            "Cannot open ZIP: %s", strerror(errno));
    return -1;
  }

  struct stat st;
  if (fstat(zip_fd, &st) != 0 || st.st_size < 0) {
    if (error_msg) snprintf(error_msg, error_msg_size, "Cannot stat ZIP: %s",
                            strerror(errno));
    close(zip_fd);
    return -1;
  }

  mz_zip_archive zip;
  mz_zip_zero_struct(&zip);
  unzip_read_ctx_t read_ctx;
  read_ctx.fd = zip_fd;
  zip.m_pRead = unzip_miniz_read;
  zip.m_pIO_opaque = &read_ctx;

  if (!mz_zip_reader_init(&zip, (mz_uint64)st.st_size, 0)) {
    mz_zip_error mz_code = mz_zip_get_last_error(&zip);
    const char *mz_err = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
    if (error_msg) {
      if (mz_code == MZ_ZIP_FAILED_FINDING_CENTRAL_DIR)
        snprintf(error_msg, error_msg_size,
                 "Not a valid or complete ZIP archive (directory missing)");
      else
        snprintf(error_msg, error_msg_size, "Invalid ZIP: %s",
                 mz_err ? mz_err : "unknown error");
    }
    close(zip_fd);
    return -1;
  }

  int result = 0;
  mz_uint num_files = mz_zip_reader_get_num_files(&zip);

  for (mz_uint i = 0; i < num_files; i++) {
    if (cancelled != NULL &&
        atomic_load_explicit(cancelled, memory_order_relaxed) != 0) {
      if (error_msg) snprintf(error_msg, error_msg_size, "Cancelled");
      result = -1;
      break;
    }

    char entry_name[PAL_PATH_MAX];
    mz_uint name_need = mz_zip_reader_get_filename(&zip, i, NULL, 0);
    if (name_need == 0U || name_need > (mz_uint)sizeof(entry_name)) {
      if (error_msg) snprintf(error_msg, error_msg_size,
                              "ZIP entry path too long");
      result = -1;
      break;
    }
    (void)mz_zip_reader_get_filename(&zip, i, entry_name,
                                     (mz_uint)sizeof(entry_name));

    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(&zip, i, &file_stat)) {
      const char *mz_err = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
      if (error_msg) snprintf(error_msg, error_msg_size,
                              "ZIP metadata error: %s",
                              mz_err ? mz_err : "unknown error");
      result = -1;
      break;
    }

    size_t name_len = strlen(entry_name);
    if (name_len == 0U) {
      continue;
    }

    mode_t archived_mode = (mode_t)(file_stat.m_external_attr >> 16);
    if ((archived_mode & S_IFMT) == S_IFLNK) {
      /* Resolve links after ordinary entries, regardless of ZIP ordering. */
      continue;
    }

    if (file_stat.m_is_directory ||
        entry_name[name_len - 1U] == '/' ||
        entry_name[name_len - 1U] == '\\') {
      while (name_len > 0U &&
             (entry_name[name_len - 1U] == '/' ||
              entry_name[name_len - 1U] == '\\')) {
        name_len--;
      }
      if (name_len == 0U) {
        continue;
      }

      char dest_full[PAL_PATH_MAX];
      if (archive_path_prepare(dest_dir, entry_name, name_len, 1,
                               dest_full, sizeof(dest_full)) != 0) {
        if (error_msg)
          snprintf(error_msg, error_msg_size, "Unsafe ZIP directory: %s",
                   entry_name);
        result = -1;
        break;
      }
      continue;
    }

    if (!file_stat.m_is_supported) {
      if (error_msg && file_stat.m_is_encrypted) {
        snprintf(error_msg, error_msg_size,
                 "Encrypted ZIP entry is not supported: %s", entry_name);
      } else if (error_msg) {
        snprintf(error_msg, error_msg_size,
                 "Unsupported ZIP compression method %u in: %s",
                 (unsigned)file_stat.m_method, entry_name);
      }
      result = -1;
      break;
    }

    char dest_full[PAL_PATH_MAX];
    if (archive_path_prepare(dest_dir, entry_name, name_len, 0,
                             dest_full, sizeof(dest_full)) != 0) {
      if (error_msg)
        snprintf(error_msg, error_msg_size, "Unsafe ZIP entry: %s", entry_name);
      result = -1;
      break;
    }

    if (unlink(dest_full) != 0 && errno != ENOENT) {
      if (error_msg)
        snprintf(error_msg, error_msg_size, "Cannot replace %s: %s",
                 dest_full, strerror(errno));
      result = -1;
      break;
    }

    int open_flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    open_flags |= O_NOFOLLOW;
#endif
    int out_fd = open(dest_full, open_flags, 0666);
    struct stat out_st;
    if (out_fd < 0 || fstat(out_fd, &out_st) != 0 ||
        !S_ISREG(out_st.st_mode) || out_st.st_nlink != 1) {
      int saved_errno = errno;
      if (out_fd >= 0) close(out_fd);
      (void)unlink(dest_full);
      if (error_msg)
        snprintf(error_msg, error_msg_size, "Cannot create %s: %s",
                 dest_full, strerror(saved_errno));
      result = -1;
      break;
    }

    unzip_write_ctx_t write_ctx;
    write_ctx.wc.fd = out_fd;
    write_ctx.wc.buf_pos = 0U;
    write_ctx.cancelled = cancelled;

    if (!mz_zip_reader_extract_to_callback(&zip, i, unzip_miniz_write,
                                           &write_ctx, 0) ||
        write_flush(&write_ctx.wc) != 0) {
      const char *mz_err = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
      if (error_msg && cancelled != NULL &&
        atomic_load_explicit(cancelled, memory_order_relaxed) != 0) {
        snprintf(error_msg, error_msg_size, "Cancelled");
      } else if (error_msg) {
        snprintf(error_msg, error_msg_size, "Failed to extract %s: %s",
                 entry_name, mz_err ? mz_err : strerror(errno));
      }
      close(out_fd);
      (void)unlink(dest_full);
      result = -1;
      break;
    }

    close(out_fd);
  }

  /* Resolve only links to regular files extracted inside this archive. */
  for (mz_uint i = 0U; result == 0 && i < num_files; i++) {
    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(&zip, i, &file_stat)) { result = -1; break; }
    mode_t archived_mode = (mode_t)(file_stat.m_external_attr >> 16);
    if ((archived_mode & S_IFMT) != S_IFLNK) continue;
    char entry_name[PAL_PATH_MAX];
    mz_uint name_need = mz_zip_reader_get_filename(&zip, i, NULL, 0);
    if (name_need == 0U || name_need > (mz_uint)sizeof(entry_name)) {
      result = -1;
      break;
    }
    (void)mz_zip_reader_get_filename(&zip, i, entry_name,
                                     (mz_uint)sizeof(entry_name));
    result = materialize_zip_link(&zip, i, dest_dir, entry_name, &file_stat,
                                  cancelled, error_msg, error_msg_size);
  }

  (void)mz_zip_reader_end(&zip);
  close(zip_fd);
  return result;
}
