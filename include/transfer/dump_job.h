/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file dump_job.h
 * @brief Decrypted game dump (browser download or console path)
 *
 * Source is the running title's sandbox mount (/mnt/sandbox/<TITLE>_000/app0),
 * where the kernel exposes the application decrypted; SELF/SPRX inside the tree
 * are decrypted through the self pager while they are read.
 *
 * The same walk feeds two destinations:
 *   - streamed to a client (ZIP bytes through a callback, no temp space), or
 *   - written on the console: one ZIP archive, or a plain directory tree.
 */

#ifndef DUMP_JOB_H
#define DUMP_JOB_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DUMP_PATH_MAX 1024

typedef enum {
  DUMP_FORMAT_ZIP = 0,   /**< one archive, streamed or written */
  DUMP_FORMAT_FILES = 1  /**< directory tree, local targets only */
} dump_format_t;

typedef struct dump_job dump_job_t;

/** Producer callback: returns bytes produced, 0 at end, -1 on error. */
typedef ssize_t (*dump_read_fn)(void *ctx, void *buf, size_t len);
typedef void (*dump_progress_fn)(uint64_t bytes, void *ctx);

/**
 * @brief Resolve the source path for a title.
 *
 * @return 1 when the running sandbox mount was found, -1 when it is not
 *         mounted.  There is no fallback to the encrypted /user/app image.
 */
int dump_resolve_source(const char *title_id, char *out, size_t out_size);

/** 1 when the sandbox for @p title_id is mounted right now. */
int dump_source_ready(const char *title_id);

dump_job_t *dump_job_create(const char *title_id, dump_format_t format,
                            int decrypt);

/**
 * @brief Create a job from an explicit source path.
 *
 * dump_job_create() resolves the sandbox mount from the title id first; this
 * variant skips the lookup, so the archive/copy pipeline can be exercised
 * without a console.
 */
dump_job_t *dump_job_create_at(const char *title_id, const char *source,
                               dump_format_t format, int decrypt);

/**
 * @brief Ask a running job to stop.
 *
 * Safe to call from another thread: the producer checks the flag at every
 * chunk and gives up with errno ECANCELED.
 */
void dump_job_cancel(dump_job_t *job);

const char *dump_job_title(const dump_job_t *job);
const char *dump_job_source(const dump_job_t *job);
uint64_t dump_job_size(const dump_job_t *job);
size_t dump_job_entries(const dump_job_t *job);
int dump_job_truncated(const dump_job_t *job);

/**
 * @brief Produce the dump as ZIP bytes (DUMP_FORMAT_ZIP only).
 *
 * Feeds the HTTP streaming response directly: returns the bytes written into
 * @p buf, 0 at the end of the archive, -1 on error.
 */
ssize_t dump_job_read(dump_job_t *job, void *buf, size_t len);

/** Write the dump under @p dest_dir (ZIP archive or file tree). */
int dump_job_write_local(dump_job_t *job, const char *dest_dir, char *error,
                         size_t error_size, dump_progress_fn progress,
                         void *progress_ctx);

void dump_job_destroy(dump_job_t *job);

#endif /* DUMP_JOB_H */
