/*
MIT License

Copyright (c) 2026 Seregon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/** @file copy.c @brief Atomic single-file copy pipeline. */

#include "pal_fileio.h"
#include "fileio_internal.h"
#include "ftp_log.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <unistd.h>

/*
 * Copy pipeline geometry.
 *
 *   readers --pread(chunk k)--> ring[k % SLOTS] --in-order write()--> dst
 *
 * - CHUNK: size of one read()/write() request. Large requests amortise the
 *   per-syscall cost of PFS/exFAT and keep NVMe transfers big.
 * - SLOTS: ring depth. More than two slots absorb write-latency jitter (PFS
 *   flushes) so the reader never stalls on a single slow write.
 * - READERS: concurrent pread() workers. Two readers keep more than one read
 *   request in flight on NVMe (F_NOCACHE disables kernel read-ahead). The
 *   single writer always writes chunks in file order, so the destination is
 *   produced sequentially (no sparse gaps on exFAT/PFS).
 */
#ifndef PAL_FILE_COPY_BUFFER_SIZE
#if defined(PLATFORM_PS5)
#define PAL_FILE_COPY_BUFFER_SIZE (8U * 1024U * 1024U) /* NVMe / M.2 / USB */
#elif defined(PLATFORM_PS4)
#define PAL_FILE_COPY_BUFFER_SIZE (1024U * 1024U) /* HDD, tight memory */
#else
#define PAL_FILE_COPY_BUFFER_SIZE (8U * 1024U * 1024U)
#endif
#endif

#ifndef PAL_FILE_COPY_SLOTS
#define PAL_FILE_COPY_SLOTS 4U
#endif

#ifndef PAL_FILE_COPY_READERS
#if defined(PLATFORM_PS4)
#define PAL_FILE_COPY_READERS 1U /* avoid extra seeks on spinning disks */
#else
#define PAL_FILE_COPY_READERS 2U
#endif
#endif

#if PAL_FILE_COPY_SLOTS < 2U
#error "PAL_FILE_COPY_SLOTS must be at least 2"
#endif
#if PAL_FILE_COPY_READERS < 1U
#error "PAL_FILE_COPY_READERS must be at least 1"
#endif

/* Files that fit in one chunk gain nothing from a reader thread. */
#define PAL_COPY_PIPELINE_MIN_BYTES ((uint64_t)PAL_FILE_COPY_BUFFER_SIZE + 1U)

enum { COPY_SLOT_FREE = 0, COPY_SLOT_BUSY = 1, COPY_SLOT_READY = 2 };

typedef struct {
  uint8_t *buf[PAL_FILE_COPY_SLOTS];
  size_t len[PAL_FILE_COPY_SLOTS];
  int state[PAL_FILE_COPY_SLOTS];
  unsigned nslots;
  size_t chunk_sz;
  int src_fd;
  uint64_t size;       /* source size snapshot from stat()        */
  uint64_t nchunks;    /* ceil(size / chunk_sz)                   */
  uint64_t next_claim; /* next chunk index a reader will fetch    */
  uint64_t next_write; /* next chunk index the writer will commit */
  int stop;            /* writer finished, cancelled or failed    */
  int reader_err;      /* first read errno (0 = ok)               */
  pthread_mutex_t mtx;
  pthread_cond_t cv_ready; /* writer waits for next chunk to be READY */
  pthread_cond_t cv_free;  /* readers wait for their slot to be FREE  */
} copy_ring_t;

static size_t copy_chunk_len(const copy_ring_t *r, uint64_t k) {
  uint64_t off = k * (uint64_t)r->chunk_sz;
  uint64_t rem = r->size - off;
  return (rem < (uint64_t)r->chunk_sz) ? (size_t)rem : r->chunk_sz;
}

static void *copy_reader_thread(void *arg) {
  copy_ring_t *r = (copy_ring_t *)arg;

  pthread_mutex_lock(&r->mtx);
  for (;;) {
    if ((r->stop != 0) || (r->next_claim >= r->nchunks)) break;

    uint64_t k = r->next_claim++;
    unsigned s = (unsigned)(k % r->nslots);

    /* Chunk k may only reuse its slot once chunk k - nslots has been
     * written; checking next_write (not just the slot state) keeps two
     * readers that map to the same slot from taking it out of order. */
    while ((r->stop == 0) && ((k >= r->next_write + r->nslots) ||
                              (r->state[s] != COPY_SLOT_FREE))) {
      pthread_cond_wait(&r->cv_free, &r->mtx);
    }
    if (r->stop != 0) break;
    r->state[s] = COPY_SLOT_BUSY;
    pthread_mutex_unlock(&r->mtx);

    uint8_t *dst = r->buf[s];
    size_t want = copy_chunk_len(r, k);
    off_t off = (off_t)(k * (uint64_t)r->chunk_sz);
    size_t got = 0U;
    int err = 0;
    while (got < want) {
      ssize_t n = pread(r->src_fd, dst + got, want - got, off + (off_t)got);
      if (n > 0) {
        got += (size_t)n;
        continue;
      }
      if (n == 0) break; /* file shrank: short chunk ends the copy */
      if (errno == EINTR) continue;
      err = errno;
      break;
    }

    pthread_mutex_lock(&r->mtx);
    if (err != 0) {
      if (r->reader_err == 0) r->reader_err = err;
      r->stop = 1;
      pthread_cond_broadcast(&r->cv_ready);
      pthread_cond_broadcast(&r->cv_free);
      break;
    }
    r->len[s] = got;
    r->state[s] = COPY_SLOT_READY;
    pthread_cond_broadcast(&r->cv_ready);
  }
  pthread_mutex_unlock(&r->mtx);
  return NULL;
}

static int copy_write_full(int fd, const uint8_t *data, size_t size,
                           int *saved_errno) {
  size_t offset = 0U;
  while (offset < size) {
    ssize_t n = write(fd, data + offset, size - offset);
    if (n > 0) {
      offset += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    /* PFS reports a full filesystem as write()==0. */
    if (saved_errno != NULL) *saved_errno = (n == 0) ? ENOSPC : errno;
    return -1;
  }
  return 0;
}

static ftp_error_t copy_report_progress(uint64_t bytes, uint64_t *cumulative,
                                        pal_copy_progress_cb_t cb,
                                        void *user_data) {
  uint64_t total = bytes;
  if (cumulative != NULL) {
    *cumulative += bytes;
    total = *cumulative;
  }
  if (cb != NULL && cb(total, user_data) < 0) return FTP_ERR_CANCELLED;
  return FTP_OK;
}

/* Allocates `count` chunks as one VM mapping; avoids the buddy allocator. */
static uint8_t *copy_map_buffers(size_t chunk, unsigned count) {
  void *p = mmap(NULL, chunk * (size_t)count, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  return (p == MAP_FAILED) ? NULL : (uint8_t *)p;
}

/*
 * Sequential copy from `offset` to EOF with pread()/write(). Used for small
 * files, as a fallback when the pipeline cannot start, and to pick up bytes
 * appended to the source after stat().
 */
static ftp_error_t copy_serial_from(int src_fd, int dst_fd, uint8_t *buf,
                                    size_t buf_sz, uint64_t offset,
                                    const char *src_path,
                                    const char *dst_path,
                                    pal_copy_progress_cb_t cb, void *user_data,
                                    uint64_t *cumulative, int *out_errno) {
  for (;;) {
    ssize_t r = pread(src_fd, buf, buf_sz, (off_t)offset);
    if (r > 0) {
      int write_errno = 0;
      if (copy_write_full(dst_fd, buf, (size_t)r, &write_errno) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[XDEV] write failed: errno=%d written_so_far=%llu dst=%.*s",
                 write_errno, (unsigned long long)offset, PAL_LOG_PATH_CHARS,
                 dst_path);
        ftp_log_line(FTP_LOG_WARN, msg);
        if (out_errno != NULL) *out_errno = write_errno;
        return FTP_ERR_FILE_WRITE;
      }
      offset += (uint64_t)r;
      if (copy_report_progress((uint64_t)r, cumulative, cb, user_data) ==
          FTP_ERR_CANCELLED) {
        return FTP_ERR_CANCELLED;
      }
      continue;
    }
    if (r == 0) return FTP_OK;
    if (errno == EINTR) continue;

    int e = errno;
    char msg[256];
    snprintf(msg, sizeof(msg),
             "[XDEV] read failed: errno=%d written_so_far=%llu src=%.*s", e,
             (unsigned long long)offset, PAL_LOG_PATH_CHARS, src_path);
    ftp_log_line(FTP_LOG_WARN, msg);
    if (out_errno != NULL) *out_errno = e;
    return FTP_ERR_FILE_READ;
  }
}

/*
 * Pipelined copy of a large file. Returns FTP_ERR_OUT_OF_MEMORY with
 * *started == 0 when neither buffers nor a reader thread could be set up, so
 * the caller can fall back to the serial path.
 */
static ftp_error_t copy_pipeline(int src_fd, int dst_fd, uint64_t size,
                                 const char *src_path, const char *dst_path,
                                 pal_copy_progress_cb_t cb, void *user_data,
                                 uint64_t *cumulative, int *out_errno,
                                 int *started) {
  *started = 0;

  size_t chunk = (size_t)PAL_FILE_COPY_BUFFER_SIZE;
  unsigned nslots = PAL_FILE_COPY_SLOTS;
  uint8_t *base = NULL;

  /* Degrade gracefully under VM pressure: fewer slots, then smaller chunks. */
  while (base == NULL) {
    base = copy_map_buffers(chunk, nslots);
    if (base != NULL) break;
    if (nslots > 2U) {
      nslots--;
    } else if (chunk > (size_t)(1024U * 1024U)) {
      chunk /= 2U;
    } else {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] pipeline mmap failed: errno=%d — "
               "falling back to serial copy for %.*s",
               errno, PAL_LOG_PATH_CHARS, src_path);
      ftp_log_line(FTP_LOG_WARN, msg);
      return FTP_ERR_OUT_OF_MEMORY;
    }
  }

  copy_ring_t ring;
  memset(&ring, 0, sizeof(ring));
  for (unsigned i = 0U; i < nslots; i++) {
    ring.buf[i] = base + (size_t)i * chunk;
    ring.state[i] = COPY_SLOT_FREE;
  }
  ring.nslots = nslots;
  ring.chunk_sz = chunk;
  ring.src_fd = src_fd;
  ring.size = size;
  ring.nchunks = (size + (uint64_t)chunk - 1U) / (uint64_t)chunk;
  pthread_mutex_init(&ring.mtx, NULL);
  pthread_cond_init(&ring.cv_ready, NULL);
  pthread_cond_init(&ring.cv_free, NULL);

  pthread_t readers[PAL_FILE_COPY_READERS];
  unsigned nreaders = 0U;
  unsigned want_readers = PAL_FILE_COPY_READERS;
  if (want_readers > nslots - 1U) want_readers = nslots - 1U;
  int pt_ret = 0;
  for (unsigned i = 0U; i < want_readers; i++) {
    pt_ret = pthread_create(&readers[nreaders], NULL, copy_reader_thread, &ring);
    if (pt_ret != 0) break;
    nreaders++;
  }

  ftp_error_t out_err = FTP_OK;

  if (nreaders == 0U) {
    /* PS4 can fail with EAGAIN (thread limit) or ENOMEM (stack). */
    char msg[256];
    snprintf(msg, sizeof(msg),
             "[XDEV] pthread_create failed: errno=%d — "
             "falling back to serial copy for %.*s",
             pt_ret, PAL_LOG_PATH_CHARS, src_path);
    ftp_log_line(FTP_LOG_WARN, msg);
    out_err = FTP_ERR_OUT_OF_MEMORY;
    goto teardown;
  }
  *started = 1;

  int write_failed = 0;
  int write_errno = 0;
  int cancelled = 0;
  int short_eof = 0;
  uint64_t written_total = 0U;

  pthread_mutex_lock(&ring.mtx);
  while (ring.next_write < ring.nchunks) {
    unsigned s = (unsigned)(ring.next_write % ring.nslots);
    while ((ring.state[s] != COPY_SLOT_READY) && (ring.stop == 0)) {
      pthread_cond_wait(&ring.cv_ready, &ring.mtx);
    }
    if (ring.state[s] != COPY_SLOT_READY) break; /* reader error */

    size_t nbytes = ring.len[s];
    size_t expect = copy_chunk_len(&ring, ring.next_write);
    pthread_mutex_unlock(&ring.mtx);

    if (copy_write_full(dst_fd, ring.buf[s], nbytes, &write_errno) != 0) {
      write_failed = 1;
    } else {
      written_total += (uint64_t)nbytes;
      /* Callback runs unlocked: it may block (pause) without stalling readers
       * on the mutex. */
      if (copy_report_progress((uint64_t)nbytes, cumulative, cb, user_data) ==
          FTP_ERR_CANCELLED) {
        cancelled = 1;
      }
    }

    pthread_mutex_lock(&ring.mtx);
    if ((write_failed != 0) || (cancelled != 0)) break;
    ring.state[s] = COPY_SLOT_FREE;
    ring.next_write++;
    pthread_cond_broadcast(&ring.cv_free);
    if (nbytes < expect) {
      short_eof = 1;
      break;
    }
  }
  ring.stop = 1;
  pthread_cond_broadcast(&ring.cv_free);
  pthread_cond_broadcast(&ring.cv_ready);
  int reader_err = ring.reader_err;
  pthread_mutex_unlock(&ring.mtx);

  for (unsigned i = 0U; i < nreaders; i++) {
    (void)pthread_join(readers[i], NULL);
  }
  if (reader_err == 0) reader_err = ring.reader_err;

  /* Preserve write errors ahead of reader errors and cancellation. */
  if (write_failed != 0) {
    char msg[256];
    snprintf(msg, sizeof(msg), "[COPY] write failed: errno=%d dst=%.*s",
             write_errno, PAL_LOG_PATH_CHARS, dst_path);
    ftp_log_line(FTP_LOG_WARN, msg);
    if (out_errno != NULL) *out_errno = write_errno;
    out_err = FTP_ERR_FILE_WRITE;
  } else if (reader_err != 0) {
    char msg[256];
    snprintf(msg, sizeof(msg), "[COPY] read failed: errno=%d src=%.*s",
             reader_err, PAL_LOG_PATH_CHARS, src_path);
    ftp_log_line(FTP_LOG_WARN, msg);
    if (out_errno != NULL) *out_errno = reader_err;
    out_err = FTP_ERR_FILE_READ;
  } else if (cancelled != 0) {
    out_err = FTP_ERR_CANCELLED;
  } else if (short_eof == 0) {
    /* Bytes appended after stat() are still copied, as with read()-to-EOF. */
    out_err = copy_serial_from(src_fd, dst_fd, ring.buf[0], ring.chunk_sz,
                               written_total, src_path, dst_path, cb,
                               user_data, cumulative, out_errno);
  }

teardown:
  pthread_mutex_destroy(&ring.mtx);
  pthread_cond_destroy(&ring.cv_ready);
  pthread_cond_destroy(&ring.cv_free);
  (void)munmap(base, chunk * (size_t)nslots);
  return out_err;
}

static atomic_uint_fast32_t g_tmp_counter = ATOMIC_VAR_INIT(0U);

static ftp_error_t build_temp_path(const char *dst_path, char *out,
                                   size_t out_size) {
  char parent[FTP_PATH_MAX];
  ftp_error_t err = fileio_parent_path(dst_path, parent, sizeof(parent));
  if (err != FTP_OK) return err;

  char name[64];
  uint_fast32_t counter = atomic_fetch_add(&g_tmp_counter, 1U);
  int n = snprintf(name, sizeof(name), ".zftpd.%lu.%lu.tmp",
                   (unsigned long)getpid(), (unsigned long)counter);
  if (n < 0 || (size_t)n >= sizeof(name)) return FTP_ERR_PATH_TOO_LONG;
  return fileio_join_path(parent, name, out, out_size);
}

static ftp_error_t create_temp_file(const char *dst_path, mode_t mode,
                                    char *tmp_path, size_t tmp_size,
                                    int *fd_out, int *out_errno) {
  if (fd_out == NULL) return FTP_ERR_INVALID_PARAM;
  for (unsigned attempt = 0U; attempt < 16U; attempt++) {
    ftp_error_t err = build_temp_path(dst_path, tmp_path, tmp_size);
    if (err != FTP_OK) return err;
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifndef PLATFORM_PS4
#ifndef PLATFORM_PS5
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#endif
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(tmp_path, flags, mode);
    if (fd >= 0) { *fd_out = fd; return FTP_OK; }
    int error = errno;
    if (error == EEXIST) continue;
    if (out_errno != NULL) *out_errno = error;
    return fileio_error_from_errno(error, FTP_ERR_FILE_OPEN);
  }
  if (out_errno != NULL) *out_errno = EEXIST;
  return FTP_ERR_FILE_OPEN;
}

/*===========================================================================*
 * FILE OPERATIONS
 *===========================================================================*/

ftp_error_t
pal_file_copy_atomic_ex(const char *src_path, const char *dst_path,
                        pal_copy_progress_cb_t cb, void *user_data,
                        uint64_t *cumulative, int *out_errno) {
  if ((src_path == NULL) || (dst_path == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }
  if (out_errno != NULL) *out_errno = 0;

  struct stat st;
  if (stat(src_path, &st) < 0)
    return fileio_error_from_errno(errno, FTP_ERR_FILE_STAT);

  if ((st.st_mode & S_IFMT) != S_IFREG) {
    return FTP_ERR_INVALID_PARAM;
  }

  int src_fd = -1;
  int dst_fd = -1;
  int tmp_created = 0;
  uint8_t *copy_buf = NULL;
  size_t copy_buf_size = 0U;
  ftp_error_t out_err = FTP_ERR_FILE_WRITE;

  char tmp_path[FTP_PATH_MAX] = {0};

  src_fd = open(src_path, O_RDONLY);

  /* Source pages are one-shot copy data; avoid polluting the page cache where supported. */
  if (src_fd >= 0) {
#ifdef F_NOCACHE
    (void)fcntl(src_fd, F_NOCACHE, 1);
#endif
#ifdef F_RDAHEAD
    (void)fcntl(src_fd, F_RDAHEAD, 1);
#endif
#if defined(POSIX_FADV_SEQUENTIAL) && !defined(PLATFORM_PS4) && !defined(PS4)
    (void)posix_fadvise(src_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  }
  if (src_fd < 0) {
    int e = errno;
    {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] open(src) failed: errno=%d path=%.*s", e,
               PAL_LOG_PATH_CHARS, src_path);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    if (out_errno != NULL) {
      *out_errno = e;
    }
    out_err = fileio_error_from_errno(e, FTP_ERR_FILE_OPEN);
    goto cleanup;
  }

  mode_t mode = (mode_t)(st.st_mode & 0777);

  /* PFS may report a full filesystem as write()==0, so reject known ENOSPC early. */
  if (st.st_size > 0) {
    char dst_dir[FTP_PATH_MAX];
    if (fileio_parent_path(dst_path, dst_dir, sizeof(dst_dir)) == FTP_OK) {
      struct statvfs vfs;
      if (statvfs(dst_dir, &vfs) == 0) {
        uint64_t free_bytes =
            (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
        uint64_t need_bytes = (uint64_t)st.st_size;
        if (need_bytes > free_bytes) {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "[XDEV] pre-flight ENOSPC: need=%llu free=%llu dst=%.*s",
                   (unsigned long long)need_bytes,
                   (unsigned long long)free_bytes, PAL_LOG_PATH_CHARS,
                   dst_path);
          ftp_log_line(FTP_LOG_WARN, msg);
          if (out_errno != NULL) {
            *out_errno = ENOSPC;
          }
          out_err = FTP_ERR_FILE_WRITE;
          goto cleanup;
        }
      }
    }
  }

  out_err = create_temp_file(dst_path, mode, tmp_path, sizeof(tmp_path),
                             &dst_fd, out_errno);
  if (out_err != FTP_OK) {
    if (out_errno != NULL && *out_errno != 0) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] create temp failed: errno=%d dst=%.*s", *out_errno,
               PAL_LOG_PATH_CHARS, dst_path);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    goto cleanup;
  }
  tmp_created = 1;

  if (st.st_size == 0) {
    /* Still honour data appended after stat(), like the read-to-EOF path. */
    uint8_t probe[4096];
    out_err = copy_serial_from(src_fd, dst_fd, probe, sizeof(probe), 0U,
                               src_path, dst_path, cb, user_data, cumulative,
                               out_errno);
    if (out_err != FTP_OK) goto cleanup;
    goto copy_done;
  }

  if ((uint64_t)st.st_size >= PAL_COPY_PIPELINE_MIN_BYTES) {
    int started = 0;
    out_err = copy_pipeline(src_fd, dst_fd, (uint64_t)st.st_size, src_path,
                            dst_path, cb, user_data, cumulative, out_errno,
                            &started);
    if (started != 0) {
      if (out_err != FTP_OK) goto cleanup;
      goto copy_done;
    }
  }

  /* VM-backed buffers avoid fragmenting the daemon buddy allocator. */
  copy_buf_size = ((uint64_t)st.st_size < (uint64_t)PAL_FILE_COPY_BUFFER_SIZE)
                      ? (size_t)st.st_size
                      : (size_t)PAL_FILE_COPY_BUFFER_SIZE;
  copy_buf = copy_map_buffers(copy_buf_size, 1U);
  if (copy_buf == NULL) {
    {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] serial mmap failed: bufsz=%u errno=%d src=%.*s",
               (unsigned)copy_buf_size,
               errno,
               PAL_LOG_PATH_CHARS, src_path);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    out_err = FTP_ERR_OUT_OF_MEMORY;
    goto cleanup;
  }

  out_err = copy_serial_from(src_fd, dst_fd, copy_buf, copy_buf_size, 0U,
                             src_path, dst_path, cb, user_data, cumulative,
                             out_errno);
  if (out_err != FTP_OK) goto cleanup;

copy_done:;

  /* Evict only clean source pages; DONTNEED on writable PFS fds can force a costly sync. */
#if defined(POSIX_FADV_DONTNEED) && !defined(PLATFORM_PS4) && !defined(PS4)
  if (src_fd >= 0) {
    (void)posix_fadvise(src_fd, 0, 0, POSIX_FADV_DONTNEED);
  }
#endif

  if (rename(tmp_path, dst_path) < 0) {
    {
      int e = errno;
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] rename(tmp->dst) failed: errno=%d tmp=%.*s dst=%.*s",
               e, PAL_LOG_PATH_PAIR_CHARS, tmp_path, PAL_LOG_PATH_PAIR_CHARS,
               dst_path);
      ftp_log_line(FTP_LOG_WARN, msg);
      if (out_errno != NULL) {
        *out_errno = e;
      }
    }
    out_err = FTP_ERR_FILE_WRITE;
    goto cleanup;
  }

  out_err = FTP_OK;

cleanup:
  if (copy_buf != NULL) {
    (void)munmap(copy_buf, copy_buf_size);
  }
  if (dst_fd >= 0) {
    (void)close(dst_fd);
  }
  if (src_fd >= 0) {
    (void)close(src_fd);
  }
  if (out_err != FTP_OK && tmp_created != 0) {
    (void)unlink(tmp_path);
  }
  return out_err;
}
