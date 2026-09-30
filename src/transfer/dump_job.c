/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file dump_job.c
 * @brief Decrypted game dump, see include/transfer/dump_job.h
 *
 * Everything it needs already exists in the tree: zip_writer collects and
 * streams the archive (ZIP64 included, decrypted SELF entries on consoles),
 * and the file-tree mode is a plain recursive copy that goes through the VFS
 * for SELF containers.
 */

#include "transfer/dump_job.h"

#include "archive/zip_writer.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include "pal_filesystem.h"
#endif

#define DUMP_COPY_CHUNK 65536U

struct dump_job {
  char title_id[32];
  char source[DUMP_PATH_MAX];
  dump_format_t format;
  int decrypt;
  atomic_int cancel;
  zip_writer_t *zip;
  uint64_t bytes;
  uint64_t bytes_written;
  size_t entry_count;
  dump_progress_fn progress;
  void *progress_ctx;
};

static int measure_tree(const char *path, uint64_t *bytes, size_t *entries,
                        int decrypt, int depth) {
  if (depth > 32) return -1;
  struct stat st;
  if (lstat(path, &st) != 0) return -1;
  if (S_ISLNK(st.st_mode)) return 0;
  if (S_ISREG(st.st_mode)) {
    uint64_t size = st.st_size > 0 ? (uint64_t)st.st_size : 0U;
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    if (decrypt != 0) {
      vfs_node_t node;
      if (psx_vfs_try_open_self(&node, path) == 1) {
        size = node.size;
        vfs_close(&node);
      }
    }
#else
    (void)decrypt;
#endif
    if (UINT64_MAX - *bytes < size) return -1;
    *bytes += size;
    (*entries)++;
    return 0;
  }
  if (!S_ISDIR(st.st_mode)) return 0;
  DIR *dir = opendir(path);
  if (dir == NULL) return -1;
  int result = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    char child[DUMP_PATH_MAX];
    int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (n < 0 || (size_t)n >= sizeof(child) ||
        measure_tree(child, bytes, entries, decrypt, depth + 1) != 0) {
      result = -1;
      break;
    }
  }
  closedir(dir);
  return result;
}

/** Candidate mountpoints of a running title, in preference order. */
#if defined(PLATFORM_PS4)
static const char *const k_sandbox_suffixes[] = {
    "-app0-patch0-union", "-app0", NULL};
static const char *const k_sandbox_root = "/mnt/sandbox/pfsmnt";
#else
static const char *const k_sandbox_suffixes[] = {"_000/app0", "_000/patch0",
                                                  "_001/app0", "_000",
                                                  NULL};
static const char *const k_sandbox_root = "/mnt/sandbox";
#endif

static int readable_directory(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
         access(path, R_OK | X_OK) == 0;
}

int dump_resolve_source(const char *title_id, char *out, size_t out_size) {
  if (title_id == NULL || out == NULL || out_size < 2U) return -1;
  out[0] = '\0';

  /* Every sandbox slot the title may have mounted, most likely first. */
  for (size_t i = 0U; k_sandbox_suffixes[i] != NULL; i++) {
    int n = snprintf(out, out_size, "%s/%s%s", k_sandbox_root, title_id,
                     k_sandbox_suffixes[i]);
    if (n <= 0 || (size_t)n >= out_size) continue;
    if (readable_directory(out)) return 1;
  }

#if !defined(PLATFORM_PS4)
  /* The sandbox directory may carry an unexpected suffix: look for any entry
   * that starts with the title id and holds an app0. */
  DIR *dir = opendir(k_sandbox_root);
  if (dir != NULL) {
    size_t id_len = strlen(title_id);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      if (strncmp(entry->d_name, title_id, id_len) != 0) continue;
      int n = snprintf(out, out_size, "%s/%s/app0", k_sandbox_root,
                       entry->d_name);
      if (n > 0 && (size_t)n < out_size && readable_directory(out)) {
        closedir(dir);
        return 1;
      }
    }
    closedir(dir);
  }
#endif

  /*
   * No fallback to /user/app: that directory holds the ENCRYPTED image, so
   * dumping it would produce something unusable.  A dump only ever comes from
   * the sandbox mount, which means the title must be running — the caller
   * launches it and waits for this to appear.
   *
   * The returned text doubles as the diagnostic shown to the user.
   */
#if defined(PLATFORM_PS4)
  (void)snprintf(out, out_size,
                 "no PS4 sandbox for %s (tried %s/%s-app0-patch0-union and "
                 "%s/%s-app0)", title_id, k_sandbox_root, title_id,
                 k_sandbox_root, title_id);
#else
  (void)snprintf(out, out_size,
                 "no sandbox for %s (tried /mnt/sandbox/%s_000/app0, patch0, "
                 "_001/app0 and any matching directory: %s)",
                 title_id, title_id, strerror(errno));
#endif
  return -1;
}

int dump_source_ready(const char *title_id) {
  char source[DUMP_PATH_MAX];
  return dump_resolve_source(title_id, source, sizeof(source)) == 1;
}

dump_job_t *dump_job_create(const char *title_id, dump_format_t format,
                            int decrypt) {
  if (title_id == NULL || title_id[0] == '\0') return NULL;

  char source[DUMP_PATH_MAX];
  if (dump_resolve_source(title_id, source, sizeof(source)) < 0) return NULL;
  return dump_job_create_at(title_id, source, format, decrypt);
}

dump_job_t *dump_job_create_at(const char *title_id, const char *source,
                               dump_format_t format, int decrypt) {
  if (title_id == NULL || title_id[0] == '\0') return NULL;
  if (source == NULL || source[0] == '\0') return NULL;

  dump_job_t *job = calloc(1U, sizeof(*job));
  if (job == NULL) return NULL;

  (void)snprintf(job->title_id, sizeof(job->title_id), "%s", title_id);
  (void)snprintf(job->source, sizeof(job->source), "%s", source);
  job->format = format;
  job->decrypt = (decrypt != 0) ? 1 : 0;
  atomic_init(&job->cancel, 0);

  if (format == DUMP_FORMAT_ZIP) {
    const char *paths[1];
    paths[0] = job->source;
    /* The decryption flag has to be in place before the walk: decrypted SELF
     * entries have a different size, and sizes are stated up front. */
    job->zip = zip_writer_create_decrypt(paths, 1U, job->decrypt);
    if (job->zip == NULL) {
      free(job);
      return NULL;
    }
    job->bytes = zip_writer_total_size(job->zip);
  } else if (measure_tree(job->source, &job->bytes, &job->entry_count,
                          job->decrypt, 0) != 0) {
    free(job);
    return NULL;
  }

  return job;
}

void dump_job_cancel(dump_job_t *job) {
  if (job == NULL) return;
  atomic_store(&job->cancel, 1);
}

const char *dump_job_title(const dump_job_t *job) {
  return (job != NULL) ? job->title_id : "";
}

const char *dump_job_source(const dump_job_t *job) {
  return (job != NULL) ? job->source : "";
}

uint64_t dump_job_size(const dump_job_t *job) {
  return (job != NULL) ? job->bytes : 0U;
}

size_t dump_job_entries(const dump_job_t *job) {
  if (job == NULL) return 0U;
  return job->zip != NULL ? zip_writer_entry_count(job->zip) : job->entry_count;
}

int dump_job_truncated(const dump_job_t *job) {
  if (job == NULL || job->zip == NULL) return 0;
  return zip_writer_truncated(job->zip);
}

ssize_t dump_job_read(dump_job_t *job, void *buf, size_t len) {
  if (job == NULL || job->zip == NULL || buf == NULL || len == 0U) {
    errno = EINVAL;
    return -1;
  }
  return zip_writer_read(job->zip, buf, len);
}

/*===========================================================================*
 * Local targets
 *===========================================================================*/

static int job_cancelled(const dump_job_t *job) {
  return atomic_load(&job->cancel) != 0;
}

static int write_with_progress(dump_job_t *job, int fd, const uint8_t *data,
                               size_t length) {
  while (length > 0U) {
    if (job_cancelled(job)) {
      errno = ECANCELED;
      return -1;
    }
    ssize_t written = write(fd, data, length);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return -1;
    data += written;
    length -= (size_t)written;
    job->bytes_written += (uint64_t)written;
    if (job->progress != NULL)
      job->progress(job->bytes_written, job->progress_ctx);
  }
  return 0;
}

static int copy_file(dump_job_t *job, const char *src, const char *dst,
                     uint8_t *buffer) {
  if (job_cancelled(job)) {
    errno = ECANCELED;
    return -1;
  }
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (job->decrypt != 0) {
    vfs_node_t node;
    if (psx_vfs_try_open_self(&node, src) == 1) {
      int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (out < 0) {
        vfs_close(&node);
        return -1;
      }
      int rc = 0;
      for (;;) {
        ssize_t got = psx_vfs_read(&node, buffer, DUMP_COPY_CHUNK);
        if (got < 0) {
          rc = -1;
          break;
        }
        if (got == 0) break;
        if (write_with_progress(job, out, buffer, (size_t)got) != 0) {
          rc = -1;
          break;
        }
      }
      int saved_errno = errno;
      (void)close(out);
      vfs_close(&node);
      if (rc != 0) {
        (void)unlink(dst); /* a partial ELF must not look like a valid dump */
        errno = saved_errno;
      }
      return rc;
    }
  }
#endif

  int in = open(src, O_RDONLY);
  if (in < 0) return -1;
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) {
    close(in);
    return -1;
  }

  int rc = 0;
  for (;;) {
    ssize_t got = read(in, buffer, DUMP_COPY_CHUNK);
    if (got < 0) {
      if (errno == EINTR) continue;
      rc = -1;
      break;
    }
    if (got == 0) break;
    if (write_with_progress(job, out, buffer, (size_t)got) != 0) {
      rc = -1;
      break;
    }
  }

  (void)close(in);
  (void)close(out);
  return rc;
}

/** Copy a directory tree, creating the destination as it goes. */
static int copy_tree(dump_job_t *job, const char *src, const char *dst,
                     uint8_t *buffer, int depth) {
  if (depth > 32) return -1;
  if (mkdir(dst, 0755) != 0 && errno != EEXIST) return -1;

  DIR *dir = opendir(src);
  if (dir == NULL) return -1;

  int rc = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (job_cancelled(job)) {
      errno = ECANCELED;
      rc = -1;
      break;
    }

    char child_src[DUMP_PATH_MAX], child_dst[DUMP_PATH_MAX];
    if (snprintf(child_src, sizeof(child_src), "%s/%s", src, entry->d_name) <= 0)
      continue;
    if (snprintf(child_dst, sizeof(child_dst), "%s/%s", dst, entry->d_name) <= 0)
      continue;

    struct stat st;
    if (lstat(child_src, &st) != 0) continue;
    if (S_ISLNK(st.st_mode)) continue; /* never follow links out of the dump */

    if (S_ISDIR(st.st_mode)) {
      if (copy_tree(job, child_src, child_dst, buffer, depth + 1) != 0) {
        rc = -1;
        break;
      }
    } else if (S_ISREG(st.st_mode)) {
      if (copy_file(job, child_src, child_dst, buffer) != 0) {
        rc = -1;
        break;
      }
    }
  }

  closedir(dir);
  return rc;
}

int dump_job_write_local(dump_job_t *job, const char *dest_dir, char *error,
                         size_t error_size, dump_progress_fn progress,
                         void *progress_ctx) {
  if (job == NULL || dest_dir == NULL || dest_dir[0] == '\0') {
    if (error != NULL) (void)snprintf(error, error_size, "Missing destination");
    return -1;
  }
  job->progress = progress;
  job->progress_ctx = progress_ctx;
  job->bytes_written = 0U;

  struct stat st;
  if (stat(dest_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    if (error != NULL)
      (void)snprintf(error, error_size, "Destination is not a directory");
    return -1;
  }
  if (job_cancelled(job)) {
    if (error != NULL) (void)snprintf(error, error_size, "Cancelled");
    return -1;
  }

  if (job->format == DUMP_FORMAT_FILES) {
    uint8_t *buffer = malloc(DUMP_COPY_CHUNK);
    if (buffer == NULL) return -1;
    char target[DUMP_PATH_MAX];
    if (snprintf(target, sizeof(target), "%s/%s", dest_dir, job->title_id) <= 0) {
      free(buffer);
      return -1;
    }
    int rc = copy_tree(job, job->source, target, buffer, 0);
    free(buffer);
    if (rc != 0 && error != NULL) {
      if (errno == ECANCELED)
        (void)snprintf(error, error_size, "Cancelled");
      else
        (void)snprintf(error, error_size, "Copy failed (%s)", strerror(errno));
    }
    return rc;
  }

  /* ZIP: write to a partial file and rename, like every other transfer. */
  char part[DUMP_PATH_MAX], final[DUMP_PATH_MAX];
  if (snprintf(final, sizeof(final), "%s/%s.zip", dest_dir, job->title_id) <= 0)
    return -1;
  if (snprintf(part, sizeof(part), "%s.zftpd.part", final) <= 0) return -1;

  int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    if (error != NULL)
      (void)snprintf(error, error_size, "Cannot create %s: %s", part,
                     strerror(errno));
    return -1;
  }

  uint8_t *buffer = malloc(DUMP_COPY_CHUNK);
  if (buffer == NULL) {
    (void)close(fd);
    (void)unlink(part);
    return -1;
  }

  int rc = 0;
  int failure_errno = 0;
  for (;;) {
    if (job_cancelled(job)) {
      failure_errno = ECANCELED;
      rc = -1;
      break;
    }
    ssize_t got = zip_writer_read(job->zip, buffer, DUMP_COPY_CHUNK);
    if (got < 0) {
      failure_errno = errno;
      rc = -1;
      break;
    }
    if (got == 0) break;
    if (write_with_progress(job, fd, buffer, (size_t)got) != 0) {
      failure_errno = errno;
      rc = -1;
      break;
    }
  }
  free(buffer);

  if (rc == 0 && fsync(fd) != 0) rc = -1;
  if (close(fd) != 0 && rc == 0) rc = -1;
  if (rc == 0 && rename(part, final) != 0) rc = -1;

  if (rc != 0) {
    (void)unlink(part);
    if (failure_errno != 0) errno = failure_errno;
    if (error != NULL) {
      if (failure_errno == ECANCELED)
        (void)snprintf(error, error_size, "Cancelled");
      else
        (void)snprintf(error, error_size, "Write failed (%s)", strerror(errno));
    }
  }
  return rc;
}

void dump_job_destroy(dump_job_t *job) {
  if (job == NULL) return;
  zip_writer_destroy(job->zip);
  free(job);
}
