#ifndef ZFTPD_TRANSFER_MANAGER_H
#define ZFTPD_TRANSFER_MANAGER_H

#include <stddef.h>
#include <stdint.h>

/** Jobs tracked at once: queued + running + finished-but-not-dismissed. */
#define TRANSFER_MAX_ACTIVE 16

/** Downloads running at the same time; the rest wait in the queue. */
#ifndef TRANSFER_MAX_CONCURRENT
#define TRANSFER_MAX_CONCURRENT 2
#endif

#define TRANSFER_URL_MAX 2048
#define TRANSFER_PATH_MAX 1024
#define TRANSFER_NAME_MAX 256
#define TRANSFER_ERROR_MAX 256
#define TRANSFER_ORPHAN_MAX 64
#define TRANSFER_PART_PATH_MAX (TRANSFER_PATH_MAX + TRANSFER_NAME_MAX + 16)

typedef struct {
  char path[TRANSFER_PART_PATH_MAX];
  uint64_t size;
} transfer_orphan_t;

typedef struct {
  int id;
  int active;
  int done;
  int paused;
  int error;
  int queued;         /**< 1 while waiting for a free download slot        */
  int queue_position; /**< 1-based queue position, 0 when not queued       */
  int cancelled;      /**< 1 when the job was cancelled by the user        */
  char url[TRANSFER_URL_MAX];
  char filename[TRANSFER_NAME_MAX];
  char dst_path[TRANSFER_PATH_MAX];
  char error_msg[TRANSFER_ERROR_MAX];
  uint64_t total_size;
  uint64_t downloaded;
  double speed;
} transfer_snapshot_t;

int transfer_url_supported(const char *url, char *reason, size_t reason_size);
/**
 * Queue a download.
 *
 * The job starts immediately when a download slot is free; otherwise it waits
 * in the queue (out_queued is set to 1) and starts on its own once an older
 * job finishes.
 */
int transfer_start(const char *url, const char *dst_dir, int *out_id,
                   int *out_queued, char *out_name, size_t out_name_size,
                   char *error, size_t error_size);
size_t transfer_snapshot_all(transfer_snapshot_t *out, size_t capacity);
int transfer_toggle_pause(int id, int *out_paused);
int transfer_cancel(int id);
int transfer_retry(int id);
/** Delete a tracked job and its unfinished .part file; keep completed files. */
int transfer_delete(int id);

/** Cached, periodically refreshed partial files without a download record. */
size_t transfer_orphans_snapshot(transfer_orphan_t *out, size_t capacity,
                                 int *scanning);
/** Delete a discovered orphan only if it is still an untracked regular file. */
int transfer_orphan_delete(const char *path);

/** Maximum concurrent downloads; 0 or less restores the default. */
void transfer_set_max_concurrent(int max);

/**
 * Re-enqueue the unfinished downloads recorded in the state file.
 *
 * Called once at startup: after a payload re-injection the in-memory queue is
 * gone, but the .zftpd.part files are still on disk, so the restored jobs
 * resume from the byte offset already downloaded.  Idempotent.
 */
int transfer_manager_restore(void);

#endif
