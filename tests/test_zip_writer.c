#include "archive/zip_writer.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { \
  if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return 1; \
  } \
} while (0)

#define EOCD_SIG "\x50\x4b\x05\x06"
#define LOCAL_SIG "\x50\x4b\x03\x04"
#define ARCHIVE_PATH "/tmp/zftpd-zip-test.zip"

static int write_file(const char *path, const void *data, size_t len) {
  FILE *fp = fopen(path, "wb");
  if (fp == NULL) return -1;
  if (len > 0U && fwrite(data, 1U, len, fp) != len) {
    fclose(fp);
    return -1;
  }
  return fclose(fp);
}

/** Read the whole archive into a heap buffer (also saved for inspection). */
static unsigned char *build_archive(zip_writer_t *zip, size_t *out_len) {
  size_t cap = 65536U, len = 0U;
  unsigned char *buffer = malloc(cap);
  if (buffer == NULL) return NULL;

  for (;;) {
    if (len + 4096U > cap) {
      unsigned char *grown = realloc(buffer, cap * 2U);
      if (grown == NULL) {
        free(buffer);
        return NULL;
      }
      buffer = grown;
      cap *= 2U;
    }
    ssize_t got = zip_writer_read(zip, buffer + len, 4096U);
    if (got < 0) {
      free(buffer);
      return NULL;
    }
    if (got == 0) break;
    len += (size_t)got;
  }

  FILE *fp = fopen(ARCHIVE_PATH, "wb");
  if (fp != NULL) {
    (void)fwrite(buffer, 1U, len, fp);
    (void)fclose(fp);
  }

  *out_len = len;
  return buffer;
}

/** First byte offset of @p needle inside @p hay, or -1. */
static long find(const unsigned char *hay, size_t hay_len, const void *needle,
                 size_t needle_len) {
  if (needle_len == 0U || hay_len < needle_len) return -1;
  for (size_t i = 0U; i + needle_len <= hay_len; i++) {
    if (memcmp(hay + i, needle, needle_len) == 0) return (long)i;
  }
  return -1;
}

static size_t eocd_entry_count(const unsigned char *zip, size_t len) {
  if (len < 22U) return 0U;
  size_t at = len - 22U; /* no archive comment is written */
  if (memcmp(zip + at, EOCD_SIG, 4U) != 0) return 0U;
  return (size_t)zip[at + 10U] | ((size_t)zip[at + 11U] << 8U);
}

static void crc32_bytes(uint32_t *crc, const unsigned char *data, size_t len) {
  static uint32_t table[256];
  static int ready = 0;
  if (ready == 0) {
    for (uint32_t i = 0U; i < 256U; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1U) ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
      table[i] = c;
    }
    ready = 1;
  }
  uint32_t c = *crc ^ 0xFFFFFFFFU;
  for (size_t i = 0U; i < len; i++) c = table[(c ^ data[i]) & 0xFFU] ^ (c >> 8U);
  *crc = c ^ 0xFFFFFFFFU;
}

/** Central directory CRC of the entry called @p name. */
static int central_crc(const unsigned char *zip, size_t len, const char *name,
                       uint32_t *out) {
  size_t n = strlen(name);
  for (size_t i = 0U; i + 46U + n <= len; i++) {
    if (memcmp(zip + i, "PK\x01\x02", 4U) != 0) continue;
    size_t name_len = (size_t)zip[i + 28U] | ((size_t)zip[i + 29U] << 8U);
    if (name_len != n || memcmp(zip + i + 46U, name, n) != 0) continue;
    *out = (uint32_t)zip[i + 16U] | ((uint32_t)zip[i + 17U] << 8U) |
           ((uint32_t)zip[i + 18U] << 16U) | ((uint32_t)zip[i + 19U] << 24U);
    return 1;
  }
  return 0;
}

static uint32_t read32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
         ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static uint64_t read64(const unsigned char *p) {
  return (uint64_t)read32(p) | ((uint64_t)read32(p + 4U) << 32U);
}

/* Explicit slow test: sparse input/output keep disk use small while the
 * writer actually processes a file crossing the 4 GiB ZIP64 boundary. */
static int test_zip64_sparse(void) {
  const uint64_t big_size = UINT64_C(0x100000000) + 64U;
  char big[] = "/tmp/zftpd-zip64-big-XXXXXX";
  char small[] = "/tmp/zftpd-zip64-small-XXXXXX";
  char output[] = "/tmp/zftpd-zip64-out-XXXXXX";
  int big_fd = mkstemp(big), small_fd = mkstemp(small), out_fd = mkstemp(output);
  CHECK(big_fd >= 0 && small_fd >= 0 && out_fd >= 0);
  CHECK(ftruncate(big_fd, (off_t)big_size) == 0);
  CHECK(write(small_fd, "x", 1U) == 1);
  close(big_fd);
  close(small_fd);

  const char *paths[] = {big, small};
  zip_writer_t *zip = zip_writer_create(paths, 2U);
  CHECK(zip != NULL && zip_writer_entry_count(zip) == 2U);
  uint64_t announced = zip_writer_total_size(zip);
  unsigned char *buffer = malloc(1024U * 1024U);
  CHECK(buffer != NULL);
  uint64_t produced = 0U;
  for (;;) {
    ssize_t got = zip_writer_read(zip, buffer, 1024U * 1024U);
    CHECK(got >= 0);
    if (got == 0) break;
    int all_zero = 1;
    for (ssize_t i = 0; i < got; i++) {
      if (buffer[i] != 0U) { all_zero = 0; break; }
    }
    if (all_zero) {
      CHECK(lseek(out_fd, (off_t)got, SEEK_CUR) >= 0);
    } else {
      CHECK(write(out_fd, buffer, (size_t)got) == got);
    }
    produced += (uint64_t)got;
  }
  CHECK(produced == announced);
  CHECK(ftruncate(out_fd, (off_t)announced) == 0);

  unsigned char tail[98];
  CHECK(pread(out_fd, tail, sizeof(tail), (off_t)(announced - sizeof(tail))) == (ssize_t)sizeof(tail));
  CHECK(read32(tail) == 0x06064b50U);       /* ZIP64 EOCD */
  CHECK(read32(tail + 56U) == 0x07064b50U); /* ZIP64 locator */
  CHECK(read64(tail + 64U) == announced - sizeof(tail));
  CHECK(read32(tail + 76U) == 0x06054b50U); /* classic EOCD */
  uint64_t cd_offset = read64(tail + 48U);
  unsigned char central[256];
  CHECK(pread(out_fd, central, sizeof(central), (off_t)cd_offset) == (ssize_t)sizeof(central));
  CHECK(read32(central) == 0x02014b50U);
  size_t first_len = 46U + (size_t)central[28] + ((size_t)central[29] << 8U) +
                     (size_t)central[30] + ((size_t)central[31] << 8U);
  CHECK(first_len + 46U < sizeof(central));
  CHECK(read32(central + first_len) == 0x02014b50U);
  CHECK(read32(central + first_len + 20U) == 1U);
  CHECK(read32(central + first_len + 24U) == 1U);
  CHECK(read32(central + first_len + 42U) == UINT32_MAX);

  free(buffer);
  zip_writer_destroy(zip);
  close(out_fd);
  CHECK(unlink(big) == 0 && unlink(small) == 0 && unlink(output) == 0);
  puts("test_zip_writer ZIP64: ok");
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--zip64") == 0) return test_zip64_sparse();
  char root[] = "/tmp/zftpd-zip-XXXXXX";
  CHECK(mkdtemp(root) != NULL);

  char file_a[512], dir_sub[512], file_b[512], file_empty[512];
  CHECK(snprintf(file_a, sizeof(file_a), "%s/a.txt", root) > 0);
  CHECK(snprintf(dir_sub, sizeof(dir_sub), "%s/sub", root) > 0);
  CHECK(snprintf(file_b, sizeof(file_b), "%s/b.bin", dir_sub) > 0);
  CHECK(snprintf(file_empty, sizeof(file_empty), "%s/empty", dir_sub) > 0);
  CHECK(mkdir(dir_sub, 0700) == 0);

  const char hello[] = "hello";
  unsigned char blob[1024];
  for (size_t i = 0U; i < sizeof(blob); i++) blob[i] = (unsigned char)(i & 0xFFU);

  CHECK(write_file(file_a, hello, sizeof(hello) - 1U) == 0);
  CHECK(write_file(file_b, blob, sizeof(blob)) == 0);
  CHECK(write_file(file_empty, NULL, 0U) == 0);

  const char *paths[2];
  paths[0] = root;
  zip_writer_t *zip = zip_writer_create(paths, 1U);
  CHECK(zip != NULL);
  /* root + a.txt + sub/ + sub/b.bin + sub/empty */
  CHECK(zip_writer_entry_count(zip) == 5U);
  CHECK(zip_writer_truncated(zip) == 0);

  /* The announced length must equal the bytes actually produced: the HTTP
   * layer answers with Content-Length, so a mismatch would stall clients. */
  uint64_t announced = zip_writer_total_size(zip);
  CHECK(announced > 0U);

  size_t len = 0U;
  unsigned char *archive = build_archive(zip, &len);
  CHECK((uint64_t)len == announced);
  zip_writer_destroy(zip);
  CHECK(archive != NULL);
  CHECK(len > 22U);

  /* Structure: starts with a local header, ends with the EOCD record. */
  CHECK(memcmp(archive, LOCAL_SIG, 4U) == 0);
  CHECK(memcmp(archive + len - 22U, EOCD_SIG, 4U) == 0);
  CHECK(eocd_entry_count(archive, len) == 5U);

  /* Archive names keep the picked folder as the top level entry. */
  const char *base = strrchr(root, '/');
  base = (base != NULL) ? base + 1 : root;
  char name_a[256], name_b[256], name_empty[256];
  CHECK(snprintf(name_a, sizeof(name_a), "%s/a.txt", base) > 0);
  CHECK(snprintf(name_b, sizeof(name_b), "%s/sub/b.bin", base) > 0);
  CHECK(snprintf(name_empty, sizeof(name_empty), "%s/sub/empty", base) > 0);

  CHECK(find(archive, len, name_a, strlen(name_a)) > 0);
  CHECK(find(archive, len, name_b, strlen(name_b)) > 0);
  CHECK(find(archive, len, name_empty, strlen(name_empty)) > 0);
  CHECK(find(archive, len, hello, sizeof(hello) - 1U) > 0);
  CHECK(find(archive, len, blob, sizeof(blob)) > 0);

  /* CRC in the central directory matches an independent computation. */
  uint32_t expected = 0U;
  crc32_bytes(&expected, (const unsigned char *)hello, sizeof(hello) - 1U);
  uint32_t stored = 0U;
  CHECK(central_crc(archive, len, name_a, &stored) == 1);
  CHECK(stored == expected);
  expected = 0U;
  crc32_bytes(&expected, blob, sizeof(blob));
  CHECK(central_crc(archive, len, name_b, &stored) == 1);
  CHECK(stored == expected);
  uint32_t empty_crc = 0xFFFFFFFFU;
  CHECK(central_crc(archive, len, name_empty, &empty_crc) == 1);
  CHECK(empty_crc == 0U);

  free(archive);

  /* A file picked on its own keeps its own name and size (regression: the
   * entry used to come out with an empty name and a zero size). */
  const char *single[1];
  single[0] = file_a;
  zip_writer_t *one = zip_writer_create(single, 1U);
  CHECK(one != NULL);
  CHECK(zip_writer_entry_count(one) == 1U);

  size_t one_len = 0U;
  unsigned char *single_zip = build_archive(one, &one_len);
  zip_writer_destroy(one);
  CHECK(single_zip != NULL);
  uint32_t single_crc = 0xFFFFFFFFU;
  CHECK(central_crc(single_zip, one_len, "a.txt", &single_crc) == 1);
  uint32_t hello_crc = 0U;
  crc32_bytes(&hello_crc, (const unsigned char *)hello, sizeof(hello) - 1U);
  CHECK(single_crc == hello_crc);
  free(single_zip);

  /* Descending through more than the initial 64 entry slots reallocates the
   * entry array.  Parent directory names must survive that growth. */
  char many_dir[512];
  CHECK(snprintf(many_dir, sizeof(many_dir), "%s/many", root) > 0);
  CHECK(mkdir(many_dir, 0700) == 0);
  for (unsigned i = 0U; i < 130U; i++) {
    char path[512];
    CHECK(snprintf(path, sizeof(path), "%s/file-%03u.txt", many_dir, i) > 0);
    CHECK(write_file(path, "x", 1U) == 0);
  }
  zip_writer_t *large = zip_writer_create(paths, 1U);
  CHECK(large != NULL);
  CHECK(zip_writer_entry_count(large) == 136U);
  uint64_t large_announced = zip_writer_total_size(large);
  size_t large_len = 0U;
  unsigned char *large_archive = build_archive(large, &large_len);
  CHECK(large_archive != NULL && (uint64_t)large_len == large_announced);
  CHECK(eocd_entry_count(large_archive, large_len) == 136U);
  char last_name[256];
  CHECK(snprintf(last_name, sizeof(last_name), "%s/many/file-129.txt", base) > 0);
  CHECK(find(large_archive, large_len, last_name, strlen(last_name)) > 0);
  CHECK(find(large_archive, large_len, name_b, strlen(name_b)) > 0);
  free(large_archive);
  zip_writer_destroy(large);
  for (unsigned i = 0U; i < 130U; i++) {
    char path[512];
    CHECK(snprintf(path, sizeof(path), "%s/file-%03u.txt", many_dir, i) > 0);
    CHECK(unlink(path) == 0);
  }
  CHECK(rmdir(many_dir) == 0);

  CHECK(unlink(file_a) == 0);
  CHECK(unlink(file_b) == 0);
  CHECK(unlink(file_empty) == 0);
  CHECK(rmdir(dir_sub) == 0);
  CHECK(rmdir(root) == 0);

  puts("test_zip_writer: ok");
  return 0;
}
