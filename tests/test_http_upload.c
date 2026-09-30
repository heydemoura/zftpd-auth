#include "../src/http/http_server_internal.h"
#include "http_api.h"
#include "http_csrf.h"
#include "pal_fileio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static int file_equals(const char *path, const char *expected) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return 0;
  char buf[128] = {0};
  size_t n = fread(buf, 1U, sizeof(buf) - 1U, f);
  fclose(f);
  return n == strlen(expected) && memcmp(buf, expected, n) == 0;
}

static int prepare_request(http_connection_t *conn, const char *root,
                           const char *name, size_t total,
                           const char *initial_body) {
  const char *token = http_csrf_get_token();
  int n = snprintf(conn->buffer, sizeof(conn->buffer),
                   "POST /api/upload?path=%s&name=%s HTTP/1.1\r\n"
                   "Host: local\r\nX-CSRF-Token: %s\r\n"
                   "Content-Length: %zu\r\n\r\n%s",
                   root, name, token, total,
                   initial_body != NULL ? initial_body : "");
  CHECK(n > 0 && (size_t)n < sizeof(conn->buffer));
  conn->buffer_used = (size_t)n;
  return 0;
}

static int run_complete_upload(const char *root) {
  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  http_connection_t conn = {.fd = sv[0], .upload = {.fd = -1}};
  CHECK(prepare_request(&conn, root, "complete.bin", 5U, "hello") == 0);
  http_request_head_t head;
  CHECK(http_peek_request_head(conn.buffer, conn.buffer_used, &head) == 0);
  CHECK(http_upload_matches(&head));
  CHECK(http_upload_start(&conn, &head) < 0);

  char path[FTP_PATH_MAX];
  CHECK(snprintf(path, sizeof(path), "%s/complete.bin", root) > 0);
  CHECK(file_equals(path, "hello"));
  char response[256] = {0};
  CHECK(recv(sv[1], response, sizeof(response) - 1U, 0) > 0);
  CHECK(strstr(response, "200 OK") != NULL);
  http_upload_reset(&conn);
  close(sv[0]); close(sv[1]);
  CHECK(unlink(path) == 0);
  return 0;
}

static int run_streaming_upload(const char *root) {
  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  http_connection_t conn = {.fd = sv[0], .upload = {.fd = -1}};
  CHECK(prepare_request(&conn, root, "stream.bin", 11U, "hello ") == 0);
  http_request_head_t head;
  CHECK(http_peek_request_head(conn.buffer, conn.buffer_used, &head) == 0);
  CHECK(http_upload_start(&conn, &head) == 0);
  CHECK(http_upload_is_active(&conn));
  CHECK(send(sv[1], "world", 5U, 0) == 5);
  CHECK(http_upload_continue(&conn) < 0);

  char path[FTP_PATH_MAX];
  CHECK(snprintf(path, sizeof(path), "%s/stream.bin", root) > 0);
  CHECK(file_equals(path, "hello world"));
  http_upload_reset(&conn);
  close(sv[0]); close(sv[1]);
  CHECK(unlink(path) == 0);
  return 0;
}

static int run_escape_rejection(const char *root) {
  char outside[] = "/tmp/zftpd-upload-out-XXXXXX";
  CHECK(mkdtemp(outside) != NULL);
  char link_path[FTP_PATH_MAX];
  CHECK(snprintf(link_path, sizeof(link_path), "%s/escape", root) > 0);
  CHECK(symlink(outside, link_path) == 0);

  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  http_connection_t conn = {.fd = sv[0], .upload = {.fd = -1}};
  char escaped_dir[FTP_PATH_MAX];
  CHECK(snprintf(escaped_dir, sizeof(escaped_dir), "%s/escape/new", root) > 0);
  CHECK(prepare_request(&conn, escaped_dir, "pwn.bin", 1U, "x") == 0);
  http_request_head_t head;
  CHECK(http_peek_request_head(conn.buffer, conn.buffer_used, &head) == 0);
  CHECK(http_upload_start(&conn, &head) < 0);

  char outside_file[FTP_PATH_MAX];
  CHECK(snprintf(outside_file, sizeof(outside_file), "%s/new/pwn.bin", outside) > 0);
  CHECK(pal_path_exists(outside_file) == 0);
  http_upload_reset(&conn);
  close(sv[0]); close(sv[1]);
  CHECK(unlink(link_path) == 0);
  CHECK(rmdir(outside) == 0);
  return 0;
}

int main(void) {
  char root[] = "/tmp/zftpd-http-upload-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  http_api_set_root(root);
  CHECK(http_csrf_init() == 0);
  CHECK(run_complete_upload(root) == 0);
  CHECK(run_streaming_upload(root) == 0);
  CHECK(run_escape_rejection(root) == 0);

  http_request_head_t head = {.method = HTTP_METHOD_POST};
  memcpy(head.uri, "/api/upload-extra", 18U);
  CHECK(!http_upload_matches(&head));
  http_api_set_root("/");
  CHECK(rmdir(root) == 0);
  puts("test_http_upload: ok");
  return 0;
}
