#include "transfer/transfer_manager.h"
#include "transfer_internal.h"
#include "platform/pal_fileio.h"
#if defined(ENABLE_LIBTORRENT) && ENABLE_LIBTORRENT
#include "transfer/backend_torrent.h"
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static transfer_job_t g_jobs[TRANSFER_MAX_ACTIVE];
static pthread_mutex_t g_jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_next_id = 1;

/** Slots currently occupied by a running worker (guarded by g_jobs_lock). */
static int g_running = 0;
static int g_max_concurrent = TRANSFER_MAX_CONCURRENT;
static int g_restored = 0;
static transfer_orphan_t g_orphans[TRANSFER_ORPHAN_MAX];
static size_t g_orphan_count = 0U;
static time_t g_orphan_scan_time = 0;
static int g_orphan_scanning = 0;
static const char *g_orphan_root = NULL;

#define ORPHAN_SCAN_INTERVAL 300
#define ORPHAN_SCAN_ENTRIES 20000U
#define ORPHAN_SCAN_DEPTH 16U

static void *transfer_worker(void *opaque);
static void dispatch_locked(void);
static void persist_locked(void);
static int delete_partial_locked(const transfer_job_t *job);
static int build_paths(const char *dst, const char *filename, char *final_path,
                       size_t final_size, char *part_path, size_t part_size);
static int build_torrent_resume_path(const transfer_job_t *job, char *out,
                                     size_t out_size) {
  char state_path[TRANSFER_STATE_PATH_MAX];
  if (transfer_state_path(state_path, sizeof(state_path)) != 0) return -1;
  int n = snprintf(out, out_size, "%s.torrent.%d.resume", state_path,
                   job->id);
  return n > 0 && (size_t)n < out_size ? 0 : -1;
}
static int is_magnet(const char *url) {
  return url != NULL && strncasecmp(url, "magnet:?", 8U) == 0;
}

#if (defined(ENABLE_LIBCURL) && ENABLE_LIBCURL) || \
    (defined(ENABLE_LIBNFS) && ENABLE_LIBNFS)
static int has_scheme(const char *url, const char *scheme) {
  size_t n = scheme != NULL ? strlen(scheme) : 0U;
  return url != NULL && scheme != NULL && strncasecmp(url, scheme, n) == 0;
}
#endif

void transfer_job_set_error(transfer_job_t *job, const char *fmt, ...) {
  if (job == NULL || fmt == NULL) return;
  va_list ap;
  va_start(ap, fmt);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
  (void)vsnprintf(job->error_msg, sizeof(job->error_msg), fmt, ap);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
  va_end(ap);
  atomic_store(&job->error, 1);
}

int transfer_job_wait_if_paused(transfer_job_t *job) {
  if (job == NULL) return -1;
  while (atomic_load(&job->paused) != 0 &&
         atomic_load(&job->cancel_requested) == 0) {
    usleep(100000U);
  }
  return atomic_load(&job->cancel_requested) != 0 ? -1 : 0;
}

int transfer_write_all(int fd, const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  while (len > 0U) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

static int part_is_tracked_locked(const char *path) {
  for (size_t i = 0U; i < TRANSFER_MAX_ACTIVE; i++) {
    const transfer_job_t *job = &g_jobs[i];
    if (job->id == 0 ||
        (atomic_load(&job->done) != 0 && atomic_load(&job->error) == 0))
      continue;
    char final_path[TRANSFER_PATH_MAX + TRANSFER_NAME_MAX + 2U];
    char part_path[sizeof(final_path) + 16U];
    if (build_paths(job->dst_path, job->filename, final_path,
                    sizeof(final_path), part_path, sizeof(part_path)) == 0 &&
        strcmp(path, part_path) == 0)
      return 1;
  }
  return 0;
}

typedef struct {
  transfer_orphan_t *files;
  size_t count;
  size_t visited;
} orphan_scan_t;

static void scan_part_directory(orphan_scan_t *scan, const char *dir,
                                unsigned depth) {
  if (depth > ORPHAN_SCAN_DEPTH || scan->visited >= ORPHAN_SCAN_ENTRIES ||
      scan->count >= TRANSFER_ORPHAN_MAX)
    return;
  struct stat root_st;
  if (lstat(dir, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) return;
  DIR *stream = opendir(dir);
  if (stream == NULL) return;
  struct dirent *ent;
  while ((ent = readdir(stream)) != NULL &&
         scan->visited < ORPHAN_SCAN_ENTRIES &&
         scan->count < TRANSFER_ORPHAN_MAX) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    scan->visited++;
    char path[TRANSFER_PART_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s%s%s", dir,
                     strcmp(dir, "/") == 0 ? "" : "/", ent->d_name);
    if (n < 0 || (size_t)n >= sizeof(path) ||
        (size_t)n >= TRANSFER_PATH_MAX) continue;
    struct stat st;
    if (lstat(path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      scan_part_directory(scan, path, depth + 1U);
    } else if (S_ISREG(st.st_mode) && st.st_size >= 0) {
      size_t len = strlen(path);
      static const char suffix[] = ".zftpd.part";
      if (len < sizeof(suffix) - 1U ||
          strcmp(path + len - (sizeof(suffix) - 1U), suffix) != 0)
        continue;
      transfer_orphan_t *item = &scan->files[scan->count++];
      (void)snprintf(item->path, sizeof(item->path), "%s", path);
      item->size = (uint64_t)st.st_size;
    }
  }
  (void)closedir(stream);
}

static void scan_orphans_and_publish(void) {
  transfer_orphan_t *found = calloc(TRANSFER_ORPHAN_MAX, sizeof(*found));
  if (found == NULL) {
    pthread_mutex_lock(&g_jobs_lock);
    g_orphan_scanning = 0;
    pthread_mutex_unlock(&g_jobs_lock);
    return;
  }
  orphan_scan_t scan = {found, 0U, 0U};
  if (g_orphan_root != NULL) {
    scan_part_directory(&scan, g_orphan_root, 0U);
  } else {
    static const char *const roots[] = {
        "/data", "/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1",
        "/mnt/usb2", "/mnt/usb3", "/mnt/usb4", "/mnt/usb5",
        "/mnt/usb6", "/mnt/usb7", "/user", NULL};
    for (size_t i = 0U; roots[i] != NULL &&
                        scan.visited < ORPHAN_SCAN_ENTRIES &&
                        scan.count < TRANSFER_ORPHAN_MAX; i++)
      scan_part_directory(&scan, roots[i], 0U);
  }
  pthread_mutex_lock(&g_jobs_lock);
  g_orphan_count = 0U;
  for (size_t i = 0U; i < scan.count; i++) {
    if (!part_is_tracked_locked(found[i].path))
      g_orphans[g_orphan_count++] = found[i];
  }
  g_orphan_scan_time = time(NULL);
  g_orphan_scanning = 0;
  pthread_mutex_unlock(&g_jobs_lock);
  free(found);
}

static void *orphan_scan_worker(void *unused) {
  (void)unused;
  scan_orphans_and_publish();
  return NULL;
}

void transfer_orphans_set_root(const char *root) {
  pthread_mutex_lock(&g_jobs_lock);
  g_orphan_root = root;
  g_orphan_count = 0U;
  g_orphan_scan_time = 0;
  pthread_mutex_unlock(&g_jobs_lock);
}

void transfer_orphans_scan_now(void) {
  pthread_mutex_lock(&g_jobs_lock);
  if (g_orphan_scanning != 0) {
    pthread_mutex_unlock(&g_jobs_lock);
    return;
  }
  g_orphan_scanning = 1;
  pthread_mutex_unlock(&g_jobs_lock);
  scan_orphans_and_publish();
}

size_t transfer_orphans_snapshot(transfer_orphan_t *out, size_t capacity,
                                 int *scanning) {
  pthread_mutex_lock(&g_jobs_lock);
  time_t now = time(NULL);
  if (g_orphan_scanning == 0 &&
      (g_orphan_scan_time == 0 || now - g_orphan_scan_time >= ORPHAN_SCAN_INTERVAL)) {
    g_orphan_scanning = 1;
    pthread_t tid;
    pthread_attr_t attr;
    (void)pthread_attr_init(&attr);
    (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, orphan_scan_worker, NULL) != 0)
      g_orphan_scanning = 0;
    (void)pthread_attr_destroy(&attr);
  }
  if (scanning != NULL) *scanning = g_orphan_scanning;
  size_t count = 0U;
  if (out != NULL) {
    for (size_t i = 0U; i < g_orphan_count && count < capacity; i++) {
      if (!part_is_tracked_locked(g_orphans[i].path)) out[count++] = g_orphans[i];
    }
  }
  pthread_mutex_unlock(&g_jobs_lock);
  return count;
}

int transfer_orphan_delete(const char *path) {
  if (path == NULL) return -1;
  pthread_mutex_lock(&g_jobs_lock);
  size_t index = g_orphan_count;
  for (size_t i = 0U; i < g_orphan_count; i++) {
    if (strcmp(g_orphans[i].path, path) == 0) { index = i; break; }
  }
  if (index == g_orphan_count || part_is_tracked_locked(path)) {
    pthread_mutex_unlock(&g_jobs_lock);
    return -1;
  }
  struct stat st;
  if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
      unlink(path) != 0) {
    pthread_mutex_unlock(&g_jobs_lock);
    return -1;
  }
  if (index + 1U < g_orphan_count)
    memmove(&g_orphans[index], &g_orphans[index + 1U],
            (g_orphan_count - index - 1U) * sizeof(g_orphans[0]));
  g_orphan_count--;
  pthread_mutex_unlock(&g_jobs_lock);
  return 0;
}

static void sanitize_filename(char *name) {
  if (name == NULL) return;
  for (size_t i = 0U; name[i] != '\0'; i++) {
    unsigned char c = (unsigned char)name[i];
    if (c < 32U || strchr("/\\:*?\"<>|", (int)c) != NULL) name[i] = '_';
  }
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = (char)tolower((unsigned char)c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static const char *magnet_param(const char *url, const char *key,
                                size_t *value_size) {
  const size_t key_size = strlen(key);
  const char *field = url + 8;
  while (*field != '\0') {
    const char *end = field + strcspn(field, "&#");
    if ((size_t)(end - field) > key_size &&
        strncasecmp(field, key, key_size) == 0 && field[key_size] == '=') {
      *value_size = (size_t)(end - field) - key_size - 1U;
      return field + key_size + 1U;
    }
    if (*end != '&') break;
    field = end + 1;
  }
  *value_size = 0U;
  return NULL;
}

static void extract_filename(const char *url, char *out, size_t out_size) {
  if (is_magnet(url)) {
    size_t name_size = 0U;
    const char *start = magnet_param(url, "dn", &name_size);
    const char *end = start != NULL ? start + name_size : NULL;
    if (start == NULL || name_size == 0U) {
      size_t xt_size = 0U;
      const char *xt = magnet_param(url, "xt", &xt_size);
      const char *hash = NULL;
      size_t prefix_size = 0U;
      if (xt != NULL && xt_size > 9U && strncasecmp(xt, "urn:btih:", 9U) == 0) {
        hash = xt + 9U;
        prefix_size = 9U;
      } else if (xt != NULL && xt_size > 9U &&
                 strncasecmp(xt, "urn:btmh:", 9U) == 0) {
        hash = xt + 9U;
        prefix_size = 9U;
      }
      if (hash != NULL) {
        size_t hash_size = xt_size - prefix_size;
        if (hash_size > 40U) hash_size = 40U;
        (void)snprintf(out, out_size, "magnet-%.*s", (int)hash_size, hash);
      } else {
        (void)snprintf(out, out_size, "magnet-download");
      }
    } else {
      size_t pos = 0U;
      while (start < end && pos + 1U < out_size) {
        unsigned char c = (unsigned char)*start++;
        if (c == '%' && end - start >= 2) {
          int hi = hex_value(start[0]);
          int lo = hex_value(start[1]);
          if (hi >= 0 && lo >= 0) {
            c = (unsigned char)((hi << 4) | lo);
            start += 2;
          }
        } else if (c == '+') {
          c = ' ';
        }
        out[pos++] = c == 0U ? '_' : (char)c;
      }
      out[pos] = '\0';
    }
    sanitize_filename(out);
    if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0 || out[0] == '\0')
      (void)snprintf(out, out_size, "magnet-download");
    return;
  }
  const char *end = url + strcspn(url, "?#");
  const char *start = end;
  while (start > url && start[-1] != '/') start--;
  size_t pos = 0U;
  while (start < end && pos + 1U < out_size) {
    if (*start == '%' && start + 2 < end) {
      int hi = hex_value(start[1]);
      int lo = hex_value(start[2]);
      if (hi >= 0 && lo >= 0) {
        out[pos++] = (char)((hi << 4) | lo);
        start += 3;
        continue;
      }
    }
    out[pos++] = *start++;
  }
  out[pos] = '\0';
  sanitize_filename(out);
}

int transfer_url_supported(const char *url, char *reason, size_t reason_size) {
  if (reason != NULL && reason_size > 0U) reason[0] = '\0';
  if (url == NULL || url[0] == '\0') {
    if (reason != NULL) (void)snprintf(reason, reason_size, "Missing URL");
    return 0;
  }
#if defined(ENABLE_LIBTORRENT) && ENABLE_LIBTORRENT
  if (is_magnet(url)) {
    if (transfer_torrent_validate(url)) return 1;
    if (reason != NULL && reason_size > 0U)
      (void)snprintf(reason, reason_size, "Invalid magnet link or info hash");
    return 0;
  }
#else
  if (is_magnet(url)) {
    if (reason != NULL && reason_size > 0U)
      (void)snprintf(reason, reason_size, "BitTorrent is unavailable in this build");
    return 0;
  }
#endif
#if defined(ENABLE_LIBCURL) && ENABLE_LIBCURL
  if (has_scheme(url, "http://") || has_scheme(url, "https://") ||
      has_scheme(url, "ftp://") || has_scheme(url, "ftps://")) return 1;
#endif
#if defined(ENABLE_LIBNFS) && ENABLE_LIBNFS
  if (has_scheme(url, "nfs://")) return 1;
#endif
  if (reason != NULL && reason_size > 0U) {
    (void)snprintf(reason, reason_size,
                   "Unsupported URL scheme or backend unavailable");
  }
  return 0;
}

static transfer_job_t *find_job(int id) {
  for (size_t i = 0U; i < TRANSFER_MAX_ACTIVE; i++) {
    if (g_jobs[i].id == id) return &g_jobs[i];
  }
  return NULL;
}

static transfer_job_t *alloc_job(void) {
  for (size_t i = 0U; i < TRANSFER_MAX_ACTIVE; i++) {
    if (g_jobs[i].id == 0) return &g_jobs[i];
  }
  for (size_t i = 0U; i < TRANSFER_MAX_ACTIVE; i++) {
    if (atomic_load(&g_jobs[i].done) != 0 &&
        (atomic_load(&g_jobs[i].error) == 0 ||
         atomic_load(&g_jobs[i].cancelled) != 0)) return &g_jobs[i];
  }
  return NULL;
}

static int run_backend(transfer_job_t *job, int fd) {
  (void)fd; /* unused when every backend is compiled out */
#if defined(ENABLE_LIBNFS) && ENABLE_LIBNFS
  if (has_scheme(job->url, "nfs://")) return transfer_backend_nfs_run(job, fd);
#endif
#if defined(ENABLE_LIBCURL) && ENABLE_LIBCURL
  if (has_scheme(job->url, "http://") || has_scheme(job->url, "https://") ||
      has_scheme(job->url, "ftp://") || has_scheme(job->url, "ftps://")) {
    return transfer_backend_curl_run(job, fd);
  }
#endif
  transfer_job_set_error(job, "No transfer backend for URL");
  return -1;
}

/** Build "<dst>/<name>" and its ".zftpd.part" sibling; 0 on success. */
static int build_paths(const char *dst, const char *filename, char *final_path,
                       size_t final_size, char *part_path, size_t part_size) {
  int n = snprintf(final_path, final_size, "%s%s%s", dst,
                   strcmp(dst, "/") == 0 ? "" : "/", filename);
  if (n < 0 || (size_t)n >= final_size) return -1;
  n = snprintf(part_path, part_size, "%s.zftpd.part", final_path);
  if (n < 0 || (size_t)n >= part_size) return -1;
  return 0;
}

/**
 * Start a queued job. Called with g_jobs_lock held.
 *
 * @return 0 when the worker is running, -1 when the job was failed instead.
 */
static int start_worker_locked(transfer_job_t *job) {
  pthread_t tid;
  pthread_attr_t attr;
  (void)pthread_attr_init(&attr);
  (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&tid, &attr, transfer_worker, job);
  (void)pthread_attr_destroy(&attr);

  atomic_store(&job->queued, 0);
  if (rc != 0) {
    atomic_store(&job->active, 0);
    transfer_job_set_error(job, "Failed to start transfer thread");
    atomic_store(&job->done, 1);
    return -1;
  }

  atomic_store(&job->active, 1);
  job->start_time = time(NULL);
  g_running++;
  return 0;
}

/**
 * Move queued jobs into the free download slots, oldest job first. Called with
 * g_jobs_lock held.
 */
static void dispatch_locked(void) {
  for (;;) {
    if (g_running >= g_max_concurrent) return;

    transfer_job_t *next = NULL;
    for (size_t i = 0U; i < TRANSFER_MAX_ACTIVE; i++) {
      transfer_job_t *job = &g_jobs[i];
      if (job->id == 0) continue;
      if (atomic_load(&job->queued) == 0) continue;
      if (atomic_load(&job->paused) != 0) continue;
      if (atomic_load(&job->done) != 0) continue;
      if (next == NULL || job->id < next->id) next = job;
    }
    if (next == NULL) return;

    (void)start_worker_locked(next);
  }
}

/** Write the visible download list to the state file. g_jobs_lock is held. */
static void persist_locked(void) {
  char path[TRANSFER_STATE_PATH_MAX];
  if (transfer_state_path(path, sizeof(path)) != 0) return;

  transfer_state_entry_t *entries =
      calloc(TRANSFER_MAX_ACTIVE, sizeof(*entries));
  if (entries == NULL) return;

  size_t count = 0U;
  for (size_t i = 0U; i < TRANSFER_MAX_ACTIVE; i++) {
    transfer_job_t *job = &g_jobs[i];
    if (job->id == 0) continue;
    transfer_state_entry_t *entry = &entries[count++];
    entry->id = job->id;
    entry->paused = atomic_load(&job->paused);
    entry->status = atomic_load(&job->cancelled) != 0 ? 'C' :
                    atomic_load(&job->error) != 0 ? 'E' :
                    atomic_load(&job->done) != 0 ? 'D' :
                    entry->paused != 0 ? 'P' :
                    atomic_load(&job->queued) != 0 ? 'Q' : 'A';
    entry->total_size = atomic_load(&job->total_size);
    entry->downloaded = atomic_load(&job->downloaded);
    (void)snprintf(entry->url, sizeof(entry->url), "%s", job->url);
    (void)snprintf(entry->dst, sizeof(entry->dst), "%s", job->dst_path);
    (void)snprintf(entry->filename, sizeof(entry->filename), "%s",
                   job->filename);
    if (entry->status == 'E' || entry->status == 'C')
      (void)snprintf(entry->error_msg, sizeof(entry->error_msg), "%s",
                     job->error_msg);
  }

  (void)transfer_state_write(path, entries, count);
  free(entries);
}

#if defined(ENABLE_LIBTORRENT) && ENABLE_LIBTORRENT
static int torrent_progress(void *opaque, uint64_t done, uint64_t total,
                            uint64_t speed) {
  transfer_job_t *job = (transfer_job_t *)opaque;
  atomic_store(&job->downloaded, done);
  atomic_store(&job->total_size, total);
  atomic_store(&job->current_speed, speed);
  if (atomic_load(&job->cancel_requested) != 0) return -1;
  return atomic_load(&job->paused) != 0 ? 1 : 0;
}

static int run_torrent(transfer_job_t *job, const char *final_path,
                       const char *part_path) {
  struct stat st;
  if (lstat(final_path, &st) == 0) {
    transfer_job_set_error(job, "Destination already exists");
    return -1;
  }
  if (lstat(part_path, &st) == 0) {
    if (!S_ISDIR(st.st_mode)) {
      transfer_job_set_error(job, "Torrent partial path is not a directory");
      return -1;
    }
  } else if (errno != ENOENT || mkdir(part_path, 0755) != 0) {
    transfer_job_set_error(job, "Cannot create torrent directory: %s",
                           strerror(errno));
    return -1;
  }

  char resume_path[TRANSFER_STATE_PATH_MAX + 48U];
  const char *resume_file =
      build_torrent_resume_path(job, resume_path, sizeof(resume_path)) == 0
          ? resume_path : NULL;
  char error[TRANSFER_ERROR_MAX] = {0};
  int rc = transfer_torrent_run(job->url, part_path, resume_file,
                                torrent_progress, job,
                                error, sizeof(error));
  atomic_store(&job->current_speed, 0U);
  if (rc != 0) {
    if (atomic_load(&job->cancel_requested) == 0)
      transfer_job_set_error(job, "%s", error[0] ? error : "BitTorrent failed");
    return -1;
  }
  if (rename(part_path, final_path) != 0) {
    transfer_job_set_error(job, "Final rename failed: %s", strerror(errno));
    return -1;
  }
  return 0;
}
#endif

static void *transfer_worker(void *opaque) {
  transfer_job_t *job = (transfer_job_t *)opaque;
  char final_path[TRANSFER_PATH_MAX + TRANSFER_NAME_MAX + 2U];
  char part_path[sizeof(final_path) + 16U];
  if (build_paths(job->dst_path, job->filename, final_path,
                  sizeof(final_path), part_path, sizeof(part_path)) != 0) {
    transfer_job_set_error(job, "Destination path too long");
    goto finished;
  }

#if defined(ENABLE_LIBTORRENT) && ENABLE_LIBTORRENT
  if (is_magnet(job->url)) {
    (void)run_torrent(job, final_path, part_path);
    if (atomic_load(&job->cancel_requested) != 0)
      (void)delete_partial_locked(job);
    goto finished;
  }
#endif

  struct stat st;
  job->resume_from = (stat(part_path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
                         ? (uint64_t)st.st_size
                         : 0U;
  atomic_store(&job->downloaded, job->resume_from);

  /* Reading an existing prefix lets the HTTP backend verify it before
   * resuming with several byte ranges. Keep write-only as a fallback for
   * destinations where the process cannot read a pre-existing partial. */
  int fd = open(part_path, O_RDWR | O_CREAT, 0644);
  if (fd < 0 && (errno == EACCES || errno == EPERM))
    fd = open(part_path, O_WRONLY | O_CREAT, 0644);
  if (fd < 0) {
    transfer_job_set_error(job, "Cannot create partial file: %s", strerror(errno));
    goto finished;
  }
  if (job->resume_from > 0U) {
    if (lseek(fd, (off_t)job->resume_from, SEEK_SET) < 0) {
      transfer_job_set_error(job, "Cannot resume partial file: %s", strerror(errno));
      (void)close(fd);
      goto finished;
    }
  } else if (ftruncate(fd, 0) != 0) {
    transfer_job_set_error(job, "Cannot truncate partial file: %s", strerror(errno));
    (void)close(fd);
    goto finished;
  }

  int rc = run_backend(job, fd);
  if (rc == 0 && fsync(fd) != 0) {
    transfer_job_set_error(job, "fsync failed: %s", strerror(errno));
    rc = -1;
  }
  (void)close(fd);

  if (rc == 0 && atomic_load(&job->cancel_requested) == 0) {
    if (rename(part_path, final_path) != 0) {
      transfer_job_set_error(job, "Final rename failed: %s", strerror(errno));
    }
  } else if (atomic_load(&job->cancel_requested) != 0) {
    (void)unlink(part_path);
  }

finished:
  /* Free the slot, pull the next queued download in and drop this job from the
   * persisted queue. */
  pthread_mutex_lock(&g_jobs_lock);
  atomic_store(&job->active, 0);
  atomic_store(&job->done, 1);
  if (g_running > 0) g_running--;
  if (atomic_load(&job->remove_requested) != 0) {
    if (delete_partial_locked(job) == 0) {
      memset(job, 0, sizeof(*job));
    } else {
      atomic_store(&job->remove_requested, 0);
      atomic_store(&job->cancelled, 0);
      transfer_job_set_error(job, "Cannot delete partial file: %s",
                             strerror(errno));
    }
  }
  dispatch_locked();
  persist_locked();
  pthread_mutex_unlock(&g_jobs_lock);
  return NULL;
}

int transfer_start(const char *url, const char *dst_dir, int *out_id,
                   int *out_queued, char *out_name, size_t out_name_size,
                   char *error, size_t error_size) {
  char reason[TRANSFER_ERROR_MAX];
  if (!transfer_url_supported(url, reason, sizeof(reason)) || dst_dir == NULL) {
    if (error != NULL && error_size > 0U)
      (void)snprintf(error, error_size, "%s", reason[0] ? reason : "Invalid destination");
    return -1;
  }
  if (strlen(url) >= TRANSFER_URL_MAX || strlen(dst_dir) >= TRANSFER_PATH_MAX) {
    if (error != NULL && error_size > 0U)
      (void)snprintf(error, error_size, "Transfer URL or destination is too long");
    return -1;
  }

  char filename[TRANSFER_NAME_MAX];
  extract_filename(url, filename, sizeof(filename));
  pthread_mutex_lock(&g_jobs_lock);
  for (size_t i = 0U; i < TRANSFER_MAX_ACTIVE; i++) {
    const transfer_job_t *existing = &g_jobs[i];
    if (existing->id == 0 || strcmp(existing->dst_path, dst_dir) != 0 ||
        strcmp(existing->filename, filename) != 0) continue;
    if (atomic_load(&existing->done) == 0 ||
        atomic_load(&existing->error) != 0) {
      pthread_mutex_unlock(&g_jobs_lock);
      if (error != NULL && error_size > 0U)
        (void)snprintf(error, error_size,
                       "This download already exists. Resume, retry or delete it in Transfers.");
      return -1;
    }
  }
  transfer_job_t *job = alloc_job();
  if (job == NULL) {
    pthread_mutex_unlock(&g_jobs_lock);
    if (error != NULL && error_size > 0U)
      (void)snprintf(error, error_size, "Transfer queue is full");
    return -1;
  }

  memset(job, 0, sizeof(*job));
  job->id = g_next_id++;
  (void)snprintf(job->url, sizeof(job->url), "%s", url);
  (void)snprintf(job->dst_path, sizeof(job->dst_path), "%s", dst_dir);
  (void)snprintf(job->filename, sizeof(job->filename), "%s", filename);
  if (job->filename[0] == '\0') {
    (void)snprintf(job->filename, sizeof(job->filename), "download_%d", job->id);
  }
  job->start_time = time(NULL);
  atomic_store(&job->queued, 1);

  /* Queue first, then fill whatever download slots are free: ordering is by
   * job id, so this job starts only when the older ones have a slot. */
  persist_locked();
  dispatch_locked();

  if (out_id != NULL) *out_id = job->id;
  if (out_queued != NULL) *out_queued = atomic_load(&job->queued);
  if (out_name != NULL && out_name_size > 0U)
    (void)snprintf(out_name, out_name_size, "%s", job->filename);
  pthread_mutex_unlock(&g_jobs_lock);
  return 0;
}

size_t transfer_snapshot_all(transfer_snapshot_t *out, size_t capacity) {
  if (out == NULL || capacity == 0U) return 0U;
  size_t count = 0U;
  pthread_mutex_lock(&g_jobs_lock);
  for (size_t i = 0U; i < TRANSFER_MAX_ACTIVE && count < capacity; i++) {
    transfer_job_t *job = &g_jobs[i];
    if (job->id == 0) continue;
    transfer_snapshot_t *snap = &out[count++];
    memset(snap, 0, sizeof(*snap));
    snap->id = job->id;
    snap->active = atomic_load(&job->active);
    snap->done = atomic_load(&job->done);
    snap->paused = atomic_load(&job->paused);
    snap->error = atomic_load(&job->error);
    snap->queued = atomic_load(&job->queued);
    snap->cancelled = atomic_load(&job->cancelled);
    if (snap->queued != 0) {
      /* FIFO position among the jobs still waiting for a slot. */
      snap->queue_position = 1;
      for (size_t j = 0U; j < TRANSFER_MAX_ACTIVE; j++) {
        const transfer_job_t *other = &g_jobs[j];
        if (other == job || other->id == 0) continue;
        if (atomic_load(&other->queued) != 0 && other->id < job->id) {
          snap->queue_position++;
        }
      }
    }
    snap->total_size = atomic_load(&job->total_size);
    snap->downloaded = atomic_load(&job->downloaded);
    (void)snprintf(snap->url, sizeof(snap->url), "%s", job->url);
    (void)snprintf(snap->filename, sizeof(snap->filename), "%s", job->filename);
    (void)snprintf(snap->dst_path, sizeof(snap->dst_path), "%s",
                   job->dst_path);
    if (snap->error != 0) {
      (void)snprintf(snap->error_msg, sizeof(snap->error_msg), "%s",
                     job->error_msg);
    }
    double elapsed = difftime(time(NULL), job->start_time);
    uint64_t session_bytes = snap->downloaded >= job->resume_from
                                 ? snap->downloaded - job->resume_from
                                 : 0U;
    snap->speed = is_magnet(job->url)
                      ? (double)atomic_load(&job->current_speed)
                      : (elapsed > 0.0 ? (double)session_bytes / elapsed : 0.0);
  }
  pthread_mutex_unlock(&g_jobs_lock);
  return count;
}

int transfer_toggle_pause(int id, int *out_paused) {
  pthread_mutex_lock(&g_jobs_lock);
  transfer_job_t *job = find_job(id);
  if (job == NULL || atomic_load(&job->done) != 0) {
    pthread_mutex_unlock(&g_jobs_lock);
    return -1;
  }
  int paused = atomic_load(&job->paused) == 0 ? 1 : 0;
  atomic_store(&job->paused, paused);
  if (out_paused != NULL) *out_paused = paused;
  /* Resuming a job that is still waiting may be able to start it right away;
   * pausing one that is running is handled by the backend. */
  if (paused == 0) dispatch_locked();
  persist_locked();
  pthread_mutex_unlock(&g_jobs_lock);
  return 0;
}

static int delete_partial_locked(const transfer_job_t *job) {
  char final_path[TRANSFER_PATH_MAX + TRANSFER_NAME_MAX + 2U];
  char part_path[sizeof(final_path) + 16U];
  if (build_paths(job->dst_path, job->filename, final_path,
                  sizeof(final_path), part_path, sizeof(part_path)) != 0)
    return -1;
  if (is_magnet(job->url)) {
    struct stat st;
    if (lstat(part_path, &st) == 0) {
      if (!S_ISDIR(st.st_mode) ||
          pal_dir_remove_recursive_pub(part_path) != FTP_OK) return -1;
    } else if (errno != ENOENT) {
      return -1;
    }
    char resume_path[TRANSFER_STATE_PATH_MAX + 48U];
    if (build_torrent_resume_path(job, resume_path,
                                  sizeof(resume_path)) == 0 &&
        unlink(resume_path) != 0 && errno != ENOENT) return -1;
    return 0;
  }
  if (unlink(part_path) != 0 && errno != ENOENT) return -1;
  return 0;
}

int transfer_cancel(int id) {
  pthread_mutex_lock(&g_jobs_lock);
  transfer_job_t *job = find_job(id);
  if (job == NULL || atomic_load(&job->done) != 0) {
    pthread_mutex_unlock(&g_jobs_lock);
    return -1;
  }
  atomic_store(&job->cancelled, 1);

  if (atomic_load(&job->queued) != 0) {
    /* Never started: no worker to interrupt, retire it here so the slot and
     * the queue entry go away immediately. */
    if (delete_partial_locked(job) != 0) {
      pthread_mutex_unlock(&g_jobs_lock);
      return -1;
    }
    atomic_store(&job->queued, 0);
    atomic_store(&job->paused, 0);
    transfer_job_set_error(job, "Cancelled by user");
    atomic_store(&job->done, 1);
    persist_locked();
    pthread_mutex_unlock(&g_jobs_lock);
    return 0;
  }

  atomic_store(&job->cancel_requested, 1);
  atomic_store(&job->paused, 0);
  pthread_mutex_unlock(&g_jobs_lock);
  return 0;
}

int transfer_retry(int id) {
  pthread_mutex_lock(&g_jobs_lock);
  transfer_job_t *job = find_job(id);
  if (job == NULL || atomic_load(&job->done) == 0 ||
      atomic_load(&job->error) == 0 ||
      atomic_load(&job->cancelled) != 0) {
    pthread_mutex_unlock(&g_jobs_lock);
    return -1;
  }
  atomic_store(&job->done, 0);
  atomic_store(&job->error, 0);
  atomic_store(&job->paused, 0);
  atomic_store(&job->queued, 1);
  atomic_store(&job->cancel_requested, 0);
  job->error_msg[0] = '\0';
  job->start_time = time(NULL);
  persist_locked();
  dispatch_locked();
  pthread_mutex_unlock(&g_jobs_lock);
  return 0;
}

int transfer_delete(int id) {
  pthread_mutex_lock(&g_jobs_lock);
  transfer_job_t *job = find_job(id);
  if (job == NULL) {
    pthread_mutex_unlock(&g_jobs_lock);
    return -1;
  }
  if (atomic_load(&job->active) != 0) {
    /* The worker owns the open file: interrupt it and remove the record only
     * after it has closed and unlinked its partial file. */
    atomic_store(&job->remove_requested, 1);
    atomic_store(&job->cancelled, 1);
    atomic_store(&job->cancel_requested, 1);
    atomic_store(&job->paused, 0);
    persist_locked();
    pthread_mutex_unlock(&g_jobs_lock);
    return 0;
  }
  if ((atomic_load(&job->done) == 0 || atomic_load(&job->error) != 0) &&
      delete_partial_locked(job) != 0) {
    pthread_mutex_unlock(&g_jobs_lock);
    return -1;
  }
  memset(job, 0, sizeof(*job));
  dispatch_locked();
  persist_locked();
  pthread_mutex_unlock(&g_jobs_lock);
  return 0;
}

void transfer_set_max_concurrent(int max) {
  pthread_mutex_lock(&g_jobs_lock);
  g_max_concurrent = (max > 0) ? max : TRANSFER_MAX_CONCURRENT;
  dispatch_locked();
  pthread_mutex_unlock(&g_jobs_lock);
}

/** True when the download already reached its final name. */
static int job_completed_on_disk(const transfer_state_entry_t *entry) {
  char final_path[TRANSFER_PATH_MAX + TRANSFER_NAME_MAX + 2U];
  char part_path[sizeof(final_path) + 16U];
  if (build_paths(entry->dst, entry->filename, final_path, sizeof(final_path),
                  part_path, sizeof(part_path)) != 0) {
    return 0;
  }

  struct stat st;
  return (stat(final_path, &st) == 0) && (stat(part_path, &st) != 0);
}

int transfer_manager_restore(void) {
  char path[TRANSFER_STATE_PATH_MAX];
  if (transfer_state_path(path, sizeof(path)) != 0) {
    return 0; /* nowhere to persist: nothing to restore either */
  }

  transfer_state_entry_t *entries =
      calloc(TRANSFER_MAX_ACTIVE, sizeof(*entries));
  if (entries == NULL) return -1;

  size_t count = transfer_state_read(path, entries, TRANSFER_MAX_ACTIVE);

  pthread_mutex_lock(&g_jobs_lock);
  if (g_restored != 0) {
    pthread_mutex_unlock(&g_jobs_lock);
    free(entries);
    return 0;
  }
  g_restored = 1;

  for (size_t i = 0U; i < count; i++) {
    const transfer_state_entry_t *entry = &entries[i];
    if ((entry->status == 'A' || entry->status == 'Q' ||
         entry->status == 'P') && job_completed_on_disk(entry)) continue;
    if (!transfer_url_supported(entry->url, NULL, 0U) ||
        entry->dst[0] != '/' || strchr(entry->filename, '/') != NULL ||
        strchr(entry->filename, '\\') != NULL) continue;
    if (find_job(entry->id) != NULL) continue;
    int duplicate = 0;
    for (size_t j = 0U; j < TRANSFER_MAX_ACTIVE; j++) {
      const transfer_job_t *other = &g_jobs[j];
      if (other->id != 0 && strcmp(other->dst_path, entry->dst) == 0 &&
          strcmp(other->filename, entry->filename) == 0 &&
          (atomic_load(&other->done) == 0 ||
           atomic_load(&other->error) != 0)) {
        duplicate = 1;
        break;
      }
    }
    if (duplicate != 0) continue;

    transfer_job_t *job = alloc_job();
    if (job == NULL) break;

    memset(job, 0, sizeof(*job));
    job->id = entry->id;
    (void)snprintf(job->url, sizeof(job->url), "%s", entry->url);
    (void)snprintf(job->dst_path, sizeof(job->dst_path), "%s", entry->dst);
    (void)snprintf(job->filename, sizeof(job->filename), "%s", entry->filename);
    job->start_time = time(NULL);
    atomic_store(&job->total_size, entry->total_size);
    atomic_store(&job->downloaded, entry->downloaded);
    if (entry->status == 'D' || entry->status == 'E' ||
        entry->status == 'C') {
      atomic_store(&job->done, 1);
      if (entry->status == 'E') {
        atomic_store(&job->error, 1);
        (void)snprintf(job->error_msg, sizeof(job->error_msg), "%s",
                       entry->error_msg[0] != '\0' ? entry->error_msg :
                       "Download failed");
      }
      if (entry->status == 'C') atomic_store(&job->cancelled, 1);
    } else {
      atomic_store(&job->queued, 1);
      if (entry->status == 'P') atomic_store(&job->paused, 1);
    }
    if (entry->status != 'D' && entry->status != 'C') {
      char final_path[TRANSFER_PATH_MAX + TRANSFER_NAME_MAX + 2U];
      char part_path[sizeof(final_path) + 16U];
      struct stat st;
      if (build_paths(entry->dst, entry->filename, final_path,
                      sizeof(final_path), part_path, sizeof(part_path)) == 0 &&
          stat(part_path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0)
        atomic_store(&job->downloaded, (uint64_t)st.st_size);
    }
    if (job->id >= g_next_id) g_next_id = job->id + 1;
  }

  persist_locked();
  dispatch_locked();
  pthread_mutex_unlock(&g_jobs_lock);

  free(entries);
  return 0;
}
