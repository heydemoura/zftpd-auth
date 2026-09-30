#include "../src/http/http_api_internal.h"
#include "archive_fixtures.h"
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

static int response_code(const http_response_t *resp, int code) {
  char prefix[32];
  if (resp == NULL) return 0;
  (void)snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d", code);
  return strncmp(resp->data, prefix, strlen(prefix)) == 0;
}

static int wait_done(int expect_error) {
  for (int i = 0; i < 300; i++) {
    http_request_t req = {0};
    req.method = HTTP_METHOD_GET;
    http_response_t *resp = http_api_archive_progress(&req);
    CHECK(resp != NULL);
    int done = strstr(resp->data, "\"done\":true") != NULL;
    int error = strstr(resp->data, "\"error\":true") != NULL;
    http_response_destroy(resp);
    if (done) return error == expect_error ? 0 : -1;
    usleep(10000);
  }
  return -1;
}

static int start_extract(const char *archive, const char *dest, int expected) {
  http_request_t req = {0};
  req.method = HTTP_METHOD_POST;
  CHECK(snprintf(req.uri, sizeof(req.uri), "/api/extract?path=%s&dst=%s",
                 archive, dest) > 0);
  http_response_t *resp = http_api_archive_start(&req);
  CHECK(response_code(resp, expected));
  http_response_destroy(resp);
  return 0;
}

int main(void) {
  char raw_base[] = "/tmp/zftpd-http-archive-XXXXXX";
  char raw_out[] = "/tmp/zftpd-http-archive-out-XXXXXX";
  CHECK(mkdtemp(raw_base) != NULL && mkdtemp(raw_out) != NULL);
  char base[PAL_PATH_MAX], outside[PAL_PATH_MAX];
  CHECK(realpath(raw_base, base) != NULL && realpath(raw_out, outside) != NULL);
  http_api_set_root(base);

  char dest[PAL_PATH_MAX], good[PAL_PATH_MAX], bad[PAL_PATH_MAX], linkzip[PAL_PATH_MAX];
  CHECK(snprintf(dest, sizeof(dest), "%s/dest", base) > 0);
  CHECK(snprintf(good, sizeof(good), "%s/good.zip", base) > 0);
  CHECK(snprintf(bad, sizeof(bad), "%s/bad.zip", base) > 0);
  CHECK(snprintf(linkzip, sizeof(linkzip), "%s/link.zip", base) > 0);
  CHECK(mkdir(dest, 0700) == 0);
  CHECK(write_blob(good, k_good_zip, sizeof(k_good_zip)) == 0);
  CHECK(write_blob(bad, k_traversal_zip, sizeof(k_traversal_zip)) == 0);
  CHECK(write_blob(linkzip, k_symlink_zip, sizeof(k_symlink_zip)) == 0);

  CHECK(start_extract(good, dest, 200) == 0);
  CHECK(wait_done(0) == 0);
  char file[PAL_PATH_MAX];
  CHECK(snprintf(file, sizeof(file), "%s/dir/file.txt", dest) > 0);
  struct stat st;
  CHECK(lstat(file, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 5);

  CHECK(start_extract(bad, dest, 200) == 0);
  CHECK(wait_done(1) == 0);
  char escape[PAL_PATH_MAX];
  CHECK(snprintf(escape, sizeof(escape), "%s/escape.txt", base) > 0);
  CHECK(lstat(escape, &st) != 0 && errno == ENOENT);

  CHECK(start_extract(linkzip, dest, 200) == 0);
  CHECK(wait_done(1) == 0);
  char link[PAL_PATH_MAX];
  CHECK(snprintf(link, sizeof(link), "%s/link", dest) > 0);
  CHECK(lstat(link, &st) != 0 && errno == ENOENT);

  CHECK(pal_dir_remove_recursive_pub(dest) == FTP_OK);
  CHECK(mkdir(dest, 0700) == 0);
  char dir_link[PAL_PATH_MAX], outside_file[PAL_PATH_MAX];
  CHECK(snprintf(dir_link, sizeof(dir_link), "%s/dir", dest) > 0);
  CHECK(snprintf(outside_file, sizeof(outside_file), "%s/file.txt", outside) > 0);
  CHECK(symlink(outside, dir_link) == 0);
  CHECK(start_extract(good, dest, 200) == 0);
  CHECK(wait_done(1) == 0);
  CHECK(lstat(outside_file, &st) != 0 && errno == ENOENT);

  http_response_t *resp = NULL;
  http_request_t malformed = {0};
  malformed.method = HTTP_METHOD_POST;
  CHECK(snprintf(malformed.uri, sizeof(malformed.uri),
                 "/api/extract?path=%s&dst=%%ZZ", good) > 0);
  resp = http_api_archive_start(&malformed);
  CHECK(response_code(resp, 400));
  http_response_destroy(resp);

  http_request_t wrong = {0};
  wrong.method = HTTP_METHOD_GET;
  (void)snprintf(wrong.uri, sizeof(wrong.uri), "/api/extract?path=%s", good);
  resp = http_api_archive_start(&wrong);
  CHECK(response_code(resp, 405));
  http_response_destroy(resp);
  (void)snprintf(wrong.uri, sizeof(wrong.uri), "/api/extracting");
  CHECK(http_api_archive_handle(&wrong) == NULL);

  http_api_set_root("/");
  CHECK(pal_dir_remove_recursive_pub(base) == FTP_OK);
  CHECK(pal_dir_remove_recursive_pub(outside) == FTP_OK);
  puts("test_http_archive: ok");
  return 0;
}
