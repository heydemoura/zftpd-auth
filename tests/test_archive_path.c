#include "archive_path.h"
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

static int write_text(const char *path, const char *text) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  size_t len = strlen(text);
  int rc = write(fd, text, len) == (ssize_t)len ? 0 : -1;
  close(fd);
  return rc;
}

static int test_relative_validation(void) {
  CHECK(archive_path_is_safe_relative("dir/file.txt", 12U) == 1);
  CHECK(archive_path_is_safe_relative("dir\\file.txt", 12U) == 1);
  CHECK(archive_path_is_safe_relative("../file", 7U) == 0);
  CHECK(archive_path_is_safe_relative("dir/../file", 11U) == 0);
  CHECK(archive_path_is_safe_relative("./file", 6U) == 0);
  CHECK(archive_path_is_safe_relative("dir//file", 9U) == 0);
  CHECK(archive_path_is_safe_relative("/absolute", 9U) == 0);
  CHECK(archive_path_is_safe_relative("C:/file", 7U) == 0);
  const char control[] = {'a', '/', 1, 'b'};
  CHECK(archive_path_is_safe_relative(control, sizeof(control)) == 0);
  return 0;
}

static int test_prepare_and_symlink_guards(void) {
  char raw_root[] = "/tmp/zftpd-archive-root-XXXXXX";
  char raw_outside[] = "/tmp/zftpd-archive-out-XXXXXX";
  CHECK(mkdtemp(raw_root) != NULL && mkdtemp(raw_outside) != NULL);

  char root[PAL_PATH_MAX], outside[PAL_PATH_MAX];
  CHECK(realpath(raw_root, root) != NULL);
  CHECK(realpath(raw_outside, outside) != NULL);

  char prepared[PAL_PATH_MAX];
  CHECK(archive_path_prepare(root, "one/two/file.bin", 16U, 0,
                             prepared, sizeof(prepared)) == 0);
  char parent[PAL_PATH_MAX];
  CHECK(snprintf(parent, sizeof(parent), "%s/one/two", root) > 0);
  struct stat st;
  CHECK(lstat(parent, &st) == 0 && S_ISDIR(st.st_mode));

  CHECK(archive_path_prepare(root, "dir\\child\\", 10U, 1,
                             prepared, sizeof(prepared)) == 0);
  CHECK(lstat(prepared, &st) == 0 && S_ISDIR(st.st_mode));

  char target[PAL_PATH_MAX], leaf[PAL_PATH_MAX];
  CHECK(snprintf(target, sizeof(target), "%s/target.txt", outside) > 0);
  CHECK(snprintf(leaf, sizeof(leaf), "%s/leaf", root) > 0);
  CHECK(write_text(target, "outside") == 0);
  CHECK(symlink(target, leaf) == 0);
  CHECK(archive_path_prepare(root, "leaf", 4U, 0,
                             prepared, sizeof(prepared)) != 0);
  CHECK(unlink(leaf) == 0);
  CHECK(link(target, leaf) == 0);
  CHECK(archive_path_prepare(root, "leaf", 4U, 0,
                             prepared, sizeof(prepared)) != 0);
  CHECK(unlink(leaf) == 0);

  char escape[PAL_PATH_MAX];
  CHECK(snprintf(escape, sizeof(escape), "%s/escape", root) > 0);
  CHECK(symlink(outside, escape) == 0);
  CHECK(archive_path_prepare(root, "escape/pwn.txt", 14U, 0,
                             prepared, sizeof(prepared)) != 0);
  char escaped_file[PAL_PATH_MAX];
  CHECK(snprintf(escaped_file, sizeof(escaped_file), "%s/pwn.txt", outside) > 0);
  CHECK(lstat(escaped_file, &st) != 0 && errno == ENOENT);

  char alias[PAL_PATH_MAX];
  CHECK(snprintf(alias, sizeof(alias), "%s-alias", root) > 0);
  CHECK(symlink(root, alias) == 0);
  CHECK(archive_path_prepare(alias, "file", 4U, 0,
                             prepared, sizeof(prepared)) != 0);

  CHECK(unlink(alias) == 0);
  CHECK(unlink(escape) == 0);
  CHECK(unlink(target) == 0);
  CHECK(pal_dir_remove_recursive_pub(root) == FTP_OK);
  CHECK(pal_dir_remove_recursive_pub(outside) == FTP_OK);
  return 0;
}

int main(void) {
  CHECK(test_relative_validation() == 0);
  CHECK(test_prepare_and_symlink_guards() == 0);
  puts("test_archive_path: ok");
  return 0;
}
