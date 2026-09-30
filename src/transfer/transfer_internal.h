#ifndef ZFTPD_TRANSFER_INTERNAL_H
#define ZFTPD_TRANSFER_INTERNAL_H

#include "transfer/transfer_manager.h"
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

typedef struct {
  atomic_int active;
  atomic_int done;
  atomic_int paused;
  atomic_int cancel_requested;
  atomic_int error;
  atomic_int queued;    /**< waiting for a free download slot */
  atomic_int cancelled; /**< cancelled before/while running   */
  atomic_int remove_requested; /**< delete record when worker exits */
  int id;
  char url[TRANSFER_URL_MAX];
  char dst_path[TRANSFER_PATH_MAX];
  char filename[TRANSFER_NAME_MAX];
  char error_msg[TRANSFER_ERROR_MAX];
  atomic_uint_fast64_t total_size;
  atomic_uint_fast64_t downloaded;
  atomic_uint_fast64_t current_speed;
  uint64_t resume_from;
  time_t start_time;
} transfer_job_t;

/*===========================================================================*
 * QUEUE AND PERSISTENCE (transfer_manager.c / transfer_state.c)
 *===========================================================================*/

/** Longest state file path (directory + file name). */
#define TRANSFER_STATE_PATH_MAX 512

/** One unfinished download as stored on disk. */
typedef struct {
  int id;
  int paused;
  char status; /**< A active, Q queued, P paused, D done, E error, C cancelled */
  uint64_t total_size;
  uint64_t downloaded;
  char url[TRANSFER_URL_MAX];
  char dst[TRANSFER_PATH_MAX];
  char filename[TRANSFER_NAME_MAX];
  char error_msg[TRANSFER_ERROR_MAX];
} transfer_state_entry_t;

/** Resolve the state file path, creating its directory when needed. */
int transfer_state_path(char *out, size_t out_size);

/** Force the state directory (unit tests); NULL restores auto-detection. */
void transfer_state_set_dir(const char *dir);

/** Persist entries (line based, one per unfinished download). */
int transfer_state_write(const char *path, const transfer_state_entry_t *entries,
                         size_t count);

/** Load entries; malformed lines are skipped. Returns the entry count. */
size_t transfer_state_read(const char *path, transfer_state_entry_t *out,
                           size_t capacity);

void transfer_job_set_error(transfer_job_t *job, const char *fmt, ...);
/** Synchronous scan and root override for unit tests. */
void transfer_orphans_scan_now(void);
void transfer_orphans_set_root(const char *root);
int transfer_job_wait_if_paused(transfer_job_t *job);
int transfer_write_all(int fd, const void *data, size_t len);

#if defined(ENABLE_LIBCURL) && ENABLE_LIBCURL
int transfer_backend_curl_run(transfer_job_t *job, int fd);
#endif
#if defined(ENABLE_LIBNFS) && ENABLE_LIBNFS
int transfer_backend_nfs_run(transfer_job_t *job, int fd);
#endif

#endif
