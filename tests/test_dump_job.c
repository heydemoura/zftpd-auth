/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file test_dump_job.c
 * @brief Dump pipeline on the host: ZIP stream, local write and cancellation.
 *
 * The console source path is resolved from the title sandbox; on the host the
 * pipeline is exercised through dump_job_create_at() with a temporary tree.
 */

#include "transfer/dump_job.h"
#include "pal_fileio.h"
#include "pal_limits.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

#define BIG_FILE_SIZE 300000U

static int write_blob(const char *path, const void *data, size_t size) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  size_t off = 0U;
  while (off < size) {
    ssize_t n = write(fd, (const unsigned char *)data + off, size - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { close(fd); return -1; }
    off += (size_t)n;
  }
  return close(fd);
}

static uint64_t file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return UINT64_MAX;
  return (uint64_t)st.st_size;
}

static void cancel_from_progress(uint64_t bytes, void *ctx) {
  (void)bytes;
  dump_job_cancel((dump_job_t *)ctx);
}

int main(void) {
  char raw_base[] = "/tmp/zftpd-dump-XXXXXX";
  CHECK(mkdtemp(raw_base) != NULL);
  char base[PAL_PATH_MAX];
  CHECK(realpath(raw_base, base) != NULL);

  char src[PAL_PATH_MAX], sub[PAL_PATH_MAX];
  char big[PAL_PATH_MAX], small[PAL_PATH_MAX];
  CHECK(snprintf(src, sizeof(src), "%s/src", base) > 0);
  CHECK(snprintf(sub, sizeof(sub), "%s/src/sub", base) > 0);
  CHECK(snprintf(big, sizeof(big), "%s/eboot.bin", src) > 0);
  CHECK(snprintf(small, sizeof(small), "%s/data.bin", sub) > 0);
  CHECK(mkdir(src, 0700) == 0 && mkdir(sub, 0700) == 0);

  uint8_t *big_data = malloc(BIG_FILE_SIZE);
  CHECK(big_data != NULL);
  for (size_t i = 0U; i < BIG_FILE_SIZE; i++) big_data[i] = (uint8_t)(i % 251U);
  CHECK(write_blob(big, big_data, BIG_FILE_SIZE) == 0);
  CHECK(write_blob(small, "hello", 5U) == 0);

  /* The host has no /mnt/sandbox/pfsmnt: resolution must say so instead of
   * silently falling back to an encrypted image. */
  char resolved[DUMP_PATH_MAX];
  CHECK(dump_resolve_source("TEST00000", resolved, sizeof(resolved)) == -1);
  CHECK(dump_source_ready("TEST00000") == 0);

  /* ── ZIP stream: sizes are stated up front, bytes match ──────────────── */
  dump_job_t *job = dump_job_create_at("TEST00000", src, DUMP_FORMAT_ZIP, 1);
  CHECK(job != NULL);
  CHECK(dump_job_entries(job) >= 3U); /* src, eboot.bin, sub, data.bin */
  CHECK(dump_job_size(job) > BIG_FILE_SIZE);
  CHECK(dump_job_truncated(job) == 0);
  CHECK(strcmp(dump_job_title(job), "TEST00000") == 0);
  CHECK(strcmp(dump_job_source(job), src) == 0);

  uint64_t announced = dump_job_size(job);
  uint8_t *buf = malloc(65536U);
  CHECK(buf != NULL);
  uint64_t streamed = 0U;
  uint32_t first_magic = 0U;
  uint8_t tail[64] = {0};
  size_t tail_len = 0U;
  for (;;) {
    ssize_t got = dump_job_read(job, buf, 65536U);
    CHECK(got >= 0);
    if (got == 0) break;
    if (streamed == 0U && got >= 4) {
      first_magic = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                    ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    }
    if ((size_t)got >= sizeof(tail)) {
      memcpy(tail, buf + got - (ssize_t)sizeof(tail), sizeof(tail));
      tail_len = sizeof(tail);
    } else {
      if (tail_len + (size_t)got > sizeof(tail))
        tail_len = sizeof(tail) - (size_t)got;
      memmove(tail, tail + (sizeof(tail) - tail_len), tail_len);
      memcpy(tail + tail_len, buf, (size_t)got);
      tail_len += (size_t)got;
    }
    streamed += (uint64_t)got;
  }
  CHECK(streamed == announced);
  CHECK(first_magic == 0x04034b50U); /* local file header */
  int saw_eocd = 0;
  for (size_t i = 0U; i + 4U <= tail_len; i++) {
    if (memcmp(tail + i, "PK\x05\x06", 4U) == 0) saw_eocd = 1;
  }
  CHECK(saw_eocd != 0); /* central directory terminator */
  dump_job_destroy(job);

  /* ── Local ZIP: atomic rename, announced size on disk ────────────────── */
  char dest[PAL_PATH_MAX], zip_path[PAL_PATH_MAX], part_path[PAL_PATH_MAX];
  CHECK(snprintf(dest, sizeof(dest), "%s/dest", base) > 0);
  CHECK(snprintf(zip_path, sizeof(zip_path), "%s/TEST00000.zip", dest) > 0);
  CHECK(snprintf(part_path, sizeof(part_path), "%s.zftpd.part", zip_path) > 0);
  CHECK(mkdir(dest, 0700) == 0);

  job = dump_job_create_at("TEST00000", src, DUMP_FORMAT_ZIP, 1);
  CHECK(job != NULL);
  uint64_t zip_size = dump_job_size(job);
  char error[256] = {0};
  CHECK(dump_job_write_local(job, dest, error, sizeof(error),
                             NULL, NULL) == 0);
  CHECK(file_size(zip_path) == zip_size);
  CHECK(access(part_path, F_OK) != 0);
  dump_job_destroy(job);

  /* ── File tree: same walk, one file per entry ────────────────────────── */
  CHECK(unlink(zip_path) == 0);
  job = dump_job_create_at("TEST00000", src, DUMP_FORMAT_FILES, 1);
  CHECK(job != NULL);
  CHECK(dump_job_size(job) == BIG_FILE_SIZE + 5U);
  CHECK(dump_job_entries(job) == 2U);
  CHECK(dump_job_write_local(job, dest, error, sizeof(error), NULL, NULL) == 0);
  char copied_big[PAL_PATH_MAX], copied_small[PAL_PATH_MAX];
  CHECK(snprintf(copied_big, sizeof(copied_big), "%s/TEST00000/eboot.bin", dest) > 0);
  CHECK(snprintf(copied_small, sizeof(copied_small), "%s/TEST00000/sub/data.bin", dest) > 0);
  CHECK(file_size(copied_big) == BIG_FILE_SIZE);
  CHECK(file_size(copied_small) == 5U);
  dump_job_destroy(job);
  CHECK(pal_dir_remove_recursive_pub(dest) == FTP_OK);
  CHECK(mkdir(dest, 0700) == 0);

  /* ── Cancellation before the first chunk ─────────────────────────────── */
  job = dump_job_create_at("TEST00000", src, DUMP_FORMAT_FILES, 1);
  CHECK(job != NULL);
  dump_job_cancel(job);
  error[0] = '\0';
  CHECK(dump_job_write_local(job, dest, error, sizeof(error), NULL, NULL) != 0);
  CHECK(strcmp(error, "Cancelled") == 0);
  char partial_tree[PAL_PATH_MAX];
  CHECK(snprintf(partial_tree, sizeof(partial_tree), "%s/TEST00000", dest) > 0);
  CHECK(access(partial_tree, F_OK) != 0);
  dump_job_destroy(job);

  /* ── Cancellation between chunks: the partial archive is dropped ─────── */
  job = dump_job_create_at("TEST00000", src, DUMP_FORMAT_ZIP, 1);
  CHECK(job != NULL);
  error[0] = '\0';
  CHECK(dump_job_write_local(job, dest, error, sizeof(error),
                             cancel_from_progress, job) != 0);
  CHECK(strcmp(error, "Cancelled") == 0);
  CHECK(access(zip_path, F_OK) != 0);
  CHECK(access(part_path, F_OK) != 0);
  dump_job_destroy(job);

  free(buf);
  free(big_data);
  CHECK(pal_dir_remove_recursive_pub(base) == FTP_OK);
  return 0;
}
