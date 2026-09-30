#include "../src/http/http_server_internal.h"
#include "event_loop.h"
#include "http_csrf.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static void *run_loop(void *arg) {
  (void)event_loop_run((event_loop_t *)arg);
  return NULL;
}


static uint16_t reserve_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0U;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return 0U;
  }
  socklen_t len = sizeof(addr);
  uint16_t port = getsockname(fd, (struct sockaddr *)&addr, &len) == 0
                      ? ntohs(addr.sin_port)
                      : 0U;
  close(fd);
  return port;
}

static int connect_client(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}


static int test_body_framing(uint16_t port, const char *root) {
  int fd = connect_client(port);
  CHECK(fd >= 0);
  const char *token = http_csrf_get_token();
  CHECK(token != NULL && token[0] != '\0');

  char request[2048];
  int n = snprintf(request, sizeof(request),
      "POST /api/create_file?path=%s&name=framed.bin HTTP/1.1\r\n"
      "Host: local\r\nX-CSRF-Token: %s\r\nContent-Length: 3\r\n\r\n"
      "abcEXTRA", root, token);
  CHECK(n > 0 && (size_t)n < sizeof(request));
  CHECK(send(fd, request, (size_t)n, 0) == n);
  char response[2048] = {0};
  CHECK(recv(fd, response, sizeof(response) - 1U, 0) > 0);
  CHECK(strstr(response, "HTTP/1.1 200 OK") != NULL);
  close(fd);

  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/framed.bin", root) > 0);
  struct stat st;
  CHECK(stat(path, &st) == 0 && st.st_size == 3);
  int in = open(path, O_RDONLY);
  CHECK(in >= 0);
  char body[4] = {0};
  CHECK(read(in, body, 3U) == 3 && strcmp(body, "abc") == 0);
  close(in);
  unlink(path);
  return 0;
}

static int test_head_has_no_body(uint16_t port) {
  int fd = connect_client(port);
  CHECK(fd >= 0);
  static const char request[] =
      "HEAD /api/status HTTP/1.1\r\nHost: local\r\n\r\n";
  CHECK(send(fd, request, sizeof(request) - 1U, 0) ==
        (ssize_t)(sizeof(request) - 1U));
  char response[4096];
  size_t used = 0U;
  while (used + 1U < sizeof(response)) {
    ssize_t n = recv(fd, response + used, sizeof(response) - used - 1U, 0);
    if (n < 0) { close(fd); return 1; }
    if (n == 0) break;
    used += (size_t)n;
  }
  close(fd);
  response[used] = '\0';
  char *body = strstr(response, "\r\n\r\n");
  CHECK(body != NULL);
  body += 4;
  CHECK(*body == '\0');
  return 0;
}

static int test_file_download_does_not_block_status(uint16_t port,
                                                     const char *root) {
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/large.bin", root) > 0);
  int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(file >= 0);
  CHECK(ftruncate(file, 16 * 1024 * 1024) == 0);
  close(file);

  int download = connect_client(port);
  CHECK(download >= 0);
  char request[1024];
  int len = snprintf(request, sizeof(request),
                     "GET /api/file/get?path=%s HTTP/1.1\r\n"
                     "Host: local\r\n\r\n", path);
  CHECK(len > 0 && (size_t)len < sizeof(request));
  CHECK(send(download, request, (size_t)len, 0) == len);
  /* Leave the large response unread: a synchronous sender fills its socket
   * buffer and prevents the event loop from serving the next request. */
  usleep(100000U);

  int status = connect_client(port);
  CHECK(status >= 0);
  static const char status_request[] =
      "GET /api/status HTTP/1.1\r\nHost: local\r\n\r\n";
  CHECK(send(status, status_request, sizeof(status_request) - 1U, 0) ==
        (ssize_t)(sizeof(status_request) - 1U));
  char reply[1024] = {0};
  CHECK(recv(status, reply, sizeof(reply) - 1U, 0) > 0);
  CHECK(strstr(reply, "HTTP/1.1 200 OK") != NULL);
  close(status);
  close(download);
  unlink(path);
  return 0;
}

static int test_file_range_through_background_worker(uint16_t port,
                                                      const char *root) {
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/range.bin", root) > 0);
  int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(file >= 0);
  CHECK(write(file, "0123456789", 10) == 10);
  close(file);

  int client = connect_client(port);
  CHECK(client >= 0);
  char request[1024];
  int len = snprintf(request, sizeof(request),
                     "GET /api/file/get?path=%s HTTP/1.1\r\n"
                     "Host: local\r\nRange: bytes=3-5\r\n\r\n", path);
  CHECK(len > 0 && (size_t)len < sizeof(request));
  CHECK(send(client, request, (size_t)len, 0) == len);
  char reply[2048];
  size_t used = 0U;
  while (used < sizeof(reply) - 1U) {
    ssize_t got = recv(client, reply + used, sizeof(reply) - used - 1U, 0);
    CHECK(got >= 0);
    if (got == 0) break;
    used += (size_t)got;
  }
  close(client);
  reply[used] = '\0';
  CHECK(strstr(reply, "HTTP/1.1 206 Partial Content\r\n") != NULL);
  CHECK(strstr(reply, "Content-Range: bytes 3-5/10\r\n") != NULL);
  CHECK(strstr(reply, "Content-Length: 3\r\n") != NULL);
  char *body = strstr(reply, "\r\n\r\n");
  CHECK(body != NULL && used - (size_t)(body + 4 - reply) == 3U);
  CHECK(memcmp(body + 4, "345", 3U) == 0);
  CHECK(unlink(path) == 0);
  return 0;
}

/* A browser that aborts a download resets the connection (RST, not FIN): the
 * server's pending write must fail with EPIPE and only drop that connection.
 * The ZIP stream uses the same send path as a game dump (raw bytes through
 * write(), not sendfile), and without SO_NOSIGPIPE / SIGPIPE ignored the write
 * kills the daemon — this test process would die the same way, so surviving it
 * is the assertion. */
static int test_aborted_download_keeps_server_alive(uint16_t port,
                                                    const char *root) {
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/abort.bin", root) > 0);
  int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(file >= 0);
  CHECK(ftruncate(file, 16 * 1024 * 1024) == 0);
  close(file);

  const char *token = http_csrf_get_token();
  CHECK(token != NULL && token[0] != '\0');
  char payload[1024];
  int payload_len = snprintf(payload, sizeof(payload),
                             "{\"paths\":[\"%s\"],\"name\":\"abort.zip\"}", path);
  CHECK(payload_len > 0 && (size_t)payload_len < sizeof(payload));

  int prep = connect_client(port);
  CHECK(prep >= 0);
  char request[2048];
  int len = snprintf(request, sizeof(request),
                     "POST /api/archive/zip HTTP/1.1\r\nHost: local\r\n"
                     "X-CSRF-Token: %s\r\nConnection: close\r\n"
                     "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
                     token, payload_len, payload);
  CHECK(len > 0 && (size_t)len < sizeof(request));
  CHECK(send(prep, request, (size_t)len, 0) == len);

  char response[4096];
  size_t used = 0U;
  while (used + 1U < sizeof(response)) {
    ssize_t got = recv(prep, response + used, sizeof(response) - used - 1U, 0);
    if (got <= 0) break;
    used += (size_t)got;
  }
  close(prep);
  response[used] = '\0';
  const char *id_key = strstr(response, "\"id\":");
  CHECK(strstr(response, "\"ok\":true") != NULL && id_key != NULL);
  int id = atoi(id_key + 5);
  CHECK(id > 0);

  int client = connect_client(port);
  CHECK(client >= 0);
  char get[256];
  int get_len = snprintf(get, sizeof(get),
                         "GET /api/archive/zip?id=%d HTTP/1.1\r\nHost: local\r\n\r\n",
                         id);
  CHECK(get_len > 0 && (size_t)get_len < sizeof(get));
  CHECK(send(client, get, (size_t)get_len, 0) == get_len);
  char buffer[8192];
  CHECK(recv(client, buffer, sizeof(buffer), 0) > 0);

  struct linger lg;
  lg.l_onoff = 1;
  lg.l_linger = 0;
  (void)setsockopt(client, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
  close(client);
  usleep(200000U);

  int status = connect_client(port);
  CHECK(status >= 0);
  static const char status_request[] =
      "GET /api/status HTTP/1.1\r\nHost: local\r\n\r\n";
  CHECK(send(status, status_request, sizeof(status_request) - 1U, 0) ==
        (ssize_t)(sizeof(status_request) - 1U));
  char reply[1024] = {0};
  CHECK(recv(status, reply, sizeof(reply) - 1U, 0) > 0);
  CHECK(strstr(reply, "HTTP/1.1 200 OK") != NULL);
  close(status);
  CHECK(unlink(path) == 0);
  return 0;
}

int main(void) {
  event_loop_t *loop = event_loop_create();
  CHECK(loop != NULL);
  char root[] = "/tmp/zftpd-http-server-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  CHECK(http_csrf_init() == 0);
  uint16_t port = reserve_port();
  CHECK(port != 0U);
  char bind_addr[64];
  CHECK(snprintf(bind_addr, sizeof(bind_addr), "127.0.0.1:%u", (unsigned)port) > 0);
  http_server_t *server = http_server_create(loop, bind_addr, root);
  CHECK(server != NULL && server->listen_fd >= 0);

  int clients[3];
  for (size_t i = 0; i < 3U; i++) {
    clients[i] = connect_client(port);
    CHECK(clients[i] >= 0);
    const char req[] = "GET /api/status HTTP/1.1\r\nHost: local\r\n\r\n";
    CHECK(send(clients[i], req, sizeof(req) - 1U, 0) == (ssize_t)(sizeof(req) - 1U));
  }

  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, run_loop, loop) == 0);
  for (size_t i = 0; i < 3U; i++) {
    char response[1024] = {0};
    ssize_t n = recv(clients[i], response, sizeof(response) - 1U, 0);
    CHECK(n > 0);
    CHECK(strstr(response, "HTTP/1.1 200 OK") != NULL);
    close(clients[i]);
  }

  CHECK(test_body_framing(port, root) == 0);
  CHECK(test_head_has_no_body(port) == 0);
  CHECK(test_file_download_does_not_block_status(port, root) == 0);
  CHECK(test_file_range_through_background_worker(port, root) == 0);
  CHECK(test_aborted_download_keeps_server_alive(port, root) == 0);

  for (unsigned i = 0U; i < 100U && atomic_load(&server->connection_count) != 0; i++)
    usleep(1000U);
  CHECK(atomic_load(&server->connection_count) == 0);

  event_loop_stop(loop);
  CHECK(pthread_join(thread, NULL) == 0);
  http_server_destroy(server);
  CHECK(server->listen_fd == -1 && server->wake_r == -1 && server->wake_w == -1);
  event_loop_destroy(loop);
  rmdir(root);
  puts("test_http_server: ok");
  return 0;
}
