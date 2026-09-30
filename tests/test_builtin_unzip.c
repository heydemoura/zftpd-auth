#include "builtin_unzip.h"
#include "pal_fileio.h"
#include "pal_limits.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

#include "archive_fixtures.h"

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

static int read_is(const char *path, const char *expected) {
  char buf[64];
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  ssize_t n = read(fd, buf, sizeof(buf) - 1U);
  close(fd);
  if (n < 0) return 0;
  buf[n] = '\0';
  return strcmp(buf, expected) == 0;
}

int main(void) {
  char raw_base[] = "/tmp/zftpd-unzip-XXXXXX";
  char raw_outside[] = "/tmp/zftpd-unzip-out-XXXXXX";
  CHECK(mkdtemp(raw_base) != NULL && mkdtemp(raw_outside) != NULL);
  char base[PAL_PATH_MAX], outside[PAL_PATH_MAX];
  CHECK(realpath(raw_base, base) != NULL && realpath(raw_outside, outside) != NULL);

  char dest[PAL_PATH_MAX], good_zip[PAL_PATH_MAX], bad_zip[PAL_PATH_MAX];
  char link_zip[PAL_PATH_MAX], file[PAL_PATH_MAX], outside_file[PAL_PATH_MAX];
  char safe_link_file[PAL_PATH_MAX];
  CHECK(snprintf(dest, sizeof(dest), "%s/dest", base) > 0);
  CHECK(snprintf(good_zip, sizeof(good_zip), "%s/good.zip", base) > 0);
  CHECK(snprintf(bad_zip, sizeof(bad_zip), "%s/bad.zip", base) > 0);
  CHECK(snprintf(link_zip, sizeof(link_zip), "%s/link.zip", base) > 0);
  CHECK(snprintf(file, sizeof(file), "%s/dir/file.txt", dest) > 0);
  CHECK(snprintf(outside_file, sizeof(outside_file), "%s/file.txt", outside) > 0);
  CHECK(snprintf(safe_link_file, sizeof(safe_link_file),
                 "%s/app/Makefile", dest) > 0);
  CHECK(mkdir(dest, 0700) == 0);
  CHECK(write_blob(good_zip, k_good_zip, sizeof(k_good_zip)) == 0);
  CHECK(write_blob(bad_zip, k_traversal_zip, sizeof(k_traversal_zip)) == 0);
  CHECK(write_blob(link_zip, k_symlink_zip, sizeof(k_symlink_zip)) == 0);

  _Atomic int cancelled = 0;
  char error[256];
  CHECK(builtin_unzip(good_zip, dest, &cancelled, error, sizeof(error)) == 0);
  CHECK(read_is(file, "hello"));
  CHECK(write_blob(file, "old", 3U) == 0);
  CHECK(builtin_unzip(good_zip, dest, &cancelled, error, sizeof(error)) == 0);
  CHECK(read_is(file, "hello"));

  char escape[PAL_PATH_MAX];
  CHECK(snprintf(escape, sizeof(escape), "%s/escape.txt", base) > 0);
  CHECK(builtin_unzip(bad_zip, dest, &cancelled, error, sizeof(error)) != 0);
  struct stat st;
  CHECK(lstat(escape, &st) != 0 && errno == ENOENT);
  CHECK(builtin_unzip(link_zip, dest, &cancelled, error, sizeof(error)) != 0);
  CHECK(builtin_unzip("tests/fixtures/safe-link.zip", dest, &cancelled,
                      error, sizeof(error)) == 0);
  CHECK(read_is(safe_link_file, "all:\n\techo ok\n"));
  CHECK(lstat(safe_link_file, &st) == 0 && S_ISREG(st.st_mode));

  /* Optional local archive for reproducing a reported extraction failure. */
  const char *sample_zip = getenv("ZFTPD_TEST_ZIP");
  if (sample_zip != NULL)
    CHECK(builtin_unzip(sample_zip, dest, &cancelled, error,
                        sizeof(error)) == 0);

  CHECK(pal_dir_remove_recursive_pub(dest) == FTP_OK);
  CHECK(mkdir(dest, 0700) == 0);
  char dir_link[PAL_PATH_MAX];
  CHECK(snprintf(dir_link, sizeof(dir_link), "%s/dir", dest) > 0);
  CHECK(symlink(outside, dir_link) == 0);
  CHECK(builtin_unzip(good_zip, dest, &cancelled, error, sizeof(error)) != 0);
  CHECK(lstat(outside_file, &st) != 0 && errno == ENOENT);

  CHECK(unlink(dir_link) == 0);
  CHECK(mkdir(dir_link, 0700) == 0);
  CHECK(symlink(outside_file, file) == 0);
  CHECK(builtin_unzip(good_zip, dest, &cancelled, error, sizeof(error)) != 0);
  CHECK(lstat(outside_file, &st) != 0 && errno == ENOENT);

  CHECK(unlink(file) == 0);
  CHECK(write_blob(outside_file, "outside", 7U) == 0);
  CHECK(link(outside_file, file) == 0);
  CHECK(builtin_unzip(good_zip, dest, &cancelled, error, sizeof(error)) != 0);
  CHECK(read_is(outside_file, "outside"));

  atomic_store(&cancelled, 1);
  CHECK(builtin_unzip(good_zip, dest, &cancelled, error, sizeof(error)) != 0);
  CHECK(strstr(error, "Cancelled") != NULL);

  CHECK(pal_dir_remove_recursive_pub(base) == FTP_OK);
  CHECK(pal_dir_remove_recursive_pub(outside) == FTP_OK);
  puts("test_builtin_unzip: ok");
  return 0;
}
