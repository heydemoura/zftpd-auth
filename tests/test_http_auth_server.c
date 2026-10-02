/* End to end: login cookie through the real server, including the
 * background download path and the upload path that bypass the router. */
#include "../src/http/http_server_internal.h"
#include "event_loop.h"
#include "http_auth.h"
#include "http_csrf.h"
#include "http_share.h"

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
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return 0U; }
  socklen_t len = sizeof(addr);
  uint16_t port = getsockname(fd, (struct sockaddr *)&addr, &len) == 0 ? ntohs(addr.sin_port) : 0U;
  close(fd);
  return port;
}

/* Sends a raw request and returns the whole response (NUL terminated). */
static int exchange(uint16_t port, const char *request, char *out, size_t out_size) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
  size_t len = strlen(request);
  if (send(fd, request, len, 0) != (ssize_t)len) { close(fd); return -1; }
  size_t used = 0U;
  for (;;) {
    ssize_t n = recv(fd, out + used, out_size - used - 1U, 0);
    if (n <= 0) break;
    used += (size_t)n;
    if (used + 1U >= out_size) break;
  }
  close(fd);
  out[used] = '\0';
  return (int)used;
}

static int status_of(const char *response) {
  return strncmp(response, "HTTP/1.1 ", 9) == 0 ? atoi(response + 9) : -1;
}

int main(void) {
  char root[] = "/tmp/zftpd-auth-srv-XXXXXX";
  char state[] = "/tmp/zftpd-auth-srvstate-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  CHECK(mkdtemp(state) != NULL);
  char pub[600], file[700];
  (void)snprintf(pub, sizeof(pub), "%s/pub", root);
  (void)snprintf(file, sizeof(file), "%s/pub/data.bin", root);
  CHECK(mkdir(pub, 0755) == 0);
  {
    FILE *f = fopen(file, "w");
    CHECK(f != NULL);
    fputs("0123456789", f);
    fclose(f);
  }

  http_auth_set_state_dir(state);
  CHECK(setenv("ZFTPD_ADMIN_PASSWORD", "adminpw1", 1) == 0);
  unsetenv("ZFTPD_HTTP_AUTH");
  CHECK(http_auth_init() == 0 && http_auth_enabled() == 1);
  http_share_init();
  CHECK(http_csrf_init() == 0);
  char err[128];
  CHECK(http_auth_user_add("bob", "bobpass1", HTTP_ROLE_USER, err, sizeof(err)) == 0);

  event_loop_t *loop = event_loop_create();
  CHECK(loop != NULL);
  uint16_t port = reserve_port();
  CHECK(port != 0U);
  char bind_addr[64];
  (void)snprintf(bind_addr, sizeof(bind_addr), "127.0.0.1:%u", (unsigned)port);
  http_server_t *server = http_server_create(loop, bind_addr, root);
  CHECK(server != NULL);
  /* The server set the HTTP root: rules can be created now. */
  const char *folders[] = {pub};
  CHECK(http_auth_folders_set(folders, 1U, err, sizeof(err)) == 0);
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, run_loop, loop) == 0);

  char req[4096];
  char resp[16384];
  const char *csrf = http_csrf_get_token();

  /* Status is public and reports the gate. */
  CHECK(exchange(port, "GET /api/status HTTP/1.1\r\nHost: l\r\n\r\n", resp, sizeof(resp)) > 0);
  CHECK(status_of(resp) == 200);
  CHECK(strstr(resp, "\"auth\":{\"enabled\":true,\"authenticated\":false") != NULL);

  /* Everything else needs a login. */
  (void)snprintf(req, sizeof(req), "GET /api/list?path=%s HTTP/1.1\r\nHost: l\r\n\r\n", pub);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 401);
  (void)snprintf(req, sizeof(req), "GET /api/file/get?path=%s HTTP/1.1\r\nHost: l\r\n\r\n", file);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 401);

  /* Wrong password. */
  const char *bad_body = "{\"login\":\"bob\",\"password\":\"nope\"}";
  (void)snprintf(req, sizeof(req),
                 "POST /api/auth/login HTTP/1.1\r\nHost: l\r\nX-CSRF-Token: %s\r\n"
                 "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                 csrf, strlen(bad_body), bad_body);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 401);

  /* Login as bob: cookie comes back. */
  const char *body = "{\"login\":\"bob\",\"password\":\"bobpass1\"}";
  (void)snprintf(req, sizeof(req),
                 "POST /api/auth/login HTTP/1.1\r\nHost: l\r\nX-CSRF-Token: %s\r\n"
                 "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                 csrf, strlen(body), body);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 200);
  const char *set_cookie = strstr(resp, "Set-Cookie: " HTTP_AUTH_COOKIE_NAME "=");
  CHECK(set_cookie != NULL);
  char token[HTTP_AUTH_TOKEN_HEX + 1U];
  memcpy(token, set_cookie + strlen("Set-Cookie: " HTTP_AUTH_COOKIE_NAME "="), HTTP_AUTH_TOKEN_HEX);
  token[HTTP_AUTH_TOKEN_HEX] = '\0';
  CHECK(strstr(resp, "\"role\":\"user\"") != NULL);

  /* Allowed folder listing works; the root does not. */
  (void)snprintf(req, sizeof(req), "GET /api/list?path=%s HTTP/1.1\r\nHost: l\r\nCookie: " HTTP_AUTH_COOKIE_NAME "=%s\r\n\r\n", pub, token);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 200);
  CHECK(strstr(resp, "data.bin") != NULL);
  (void)snprintf(req, sizeof(req), "GET /api/list?path=%s HTTP/1.1\r\nHost: l\r\nCookie: " HTTP_AUTH_COOKIE_NAME "=%s\r\n\r\n", root, token);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 403);

  /* File download goes through the background worker: the cookie must
   * travel with it. */
  (void)snprintf(req, sizeof(req), "GET /api/file/get?path=%s HTTP/1.1\r\nHost: l\r\nCookie: " HTTP_AUTH_COOKIE_NAME "=%s\r\nRange: bytes=2-4\r\n\r\n", file, token);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 206);
  CHECK(strstr(resp, "\r\n\r\n234") != NULL);

  /* Users cannot manage shares or reach admin routes. */
  (void)snprintf(req, sizeof(req), "GET /api/shares HTTP/1.1\r\nHost: l\r\nCookie: " HTTP_AUTH_COOKIE_NAME "=%s\r\n\r\n", token);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 403);
  (void)snprintf(req, sizeof(req), "GET /api/processes HTTP/1.1\r\nHost: l\r\nCookie: " HTTP_AUTH_COOKIE_NAME "=%s\r\n\r\n", token);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 403);

  /* Upload: allowed folder ok, other folder refused by the gate. */
  (void)snprintf(req, sizeof(req),
                 "POST /api/upload?path=%s&name=up.txt HTTP/1.1\r\nHost: l\r\nX-CSRF-Token: %s\r\n"
                 "Cookie: " HTTP_AUTH_COOKIE_NAME "=%s\r\nContent-Length: 3\r\n\r\nabc", pub, csrf, token);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 200);
  (void)snprintf(req, sizeof(req),
                 "POST /api/upload?path=%s&name=up2.txt HTTP/1.1\r\nHost: l\r\nX-CSRF-Token: %s\r\n"
                 "Cookie: " HTTP_AUTH_COOKIE_NAME "=%s\r\nContent-Length: 3\r\n\r\nabc", root, csrf, token);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 403);
  (void)snprintf(req, sizeof(req),
                 "POST /api/upload?path=%s&name=up3.txt HTTP/1.1\r\nHost: l\r\nX-CSRF-Token: %s\r\n"
                 "Content-Length: 3\r\n\r\nabc", pub, csrf);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 401);

  /* Admin login, share creation, public download of the share. */
  body = "{\"login\":\"admin\",\"password\":\"adminpw1\"}";
  (void)snprintf(req, sizeof(req),
                 "POST /api/auth/login HTTP/1.1\r\nHost: l\r\nX-CSRF-Token: %s\r\n"
                 "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                 csrf, strlen(body), body);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 200);
  set_cookie = strstr(resp, "Set-Cookie: " HTTP_AUTH_COOKIE_NAME "=");
  CHECK(set_cookie != NULL);
  char admin[HTTP_AUTH_TOKEN_HEX + 1U];
  memcpy(admin, set_cookie + strlen("Set-Cookie: " HTTP_AUTH_COOKIE_NAME "="), HTTP_AUTH_TOKEN_HEX);
  admin[HTTP_AUTH_TOKEN_HEX] = '\0';

  char share_body[1024];
  (void)snprintf(share_body, sizeof(share_body), "{\"path\":\"%s\",\"expires\":0}", file);
  (void)snprintf(req, sizeof(req),
                 "POST /api/shares/create HTTP/1.1\r\nHost: l\r\nX-CSRF-Token: %s\r\n"
                 "Cookie: " HTTP_AUTH_COOKIE_NAME "=%s\r\nContent-Type: application/json\r\n"
                 "Content-Length: %zu\r\n\r\n%s", csrf, admin, strlen(share_body), share_body);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 200);
  const char *id = strstr(resp, "\"id\":\"");
  CHECK(id != NULL);
  char share_id[HTTP_SHARE_ID_HEX + 1U];
  memcpy(share_id, id + 6, HTTP_SHARE_ID_HEX);
  share_id[HTTP_SHARE_ID_HEX] = '\0';

  (void)snprintf(req, sizeof(req), "GET /s/%s HTTP/1.1\r\nHost: l\r\n\r\n", share_id);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 200);
  CHECK(strstr(resp, "attachment; filename=\"data.bin\"") != NULL);
  CHECK(strstr(resp, "\r\n\r\n0123456789") != NULL);

  /* Admin sees the share list; logout clears the cookie. */
  (void)snprintf(req, sizeof(req), "GET /api/shares HTTP/1.1\r\nHost: l\r\nAuthorization: Bearer %s\r\n\r\n", admin);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 200);
  CHECK(strstr(resp, share_id) != NULL);
  (void)snprintf(req, sizeof(req),
                 "POST /api/auth/logout HTTP/1.1\r\nHost: l\r\nX-CSRF-Token: %s\r\n"
                 "Cookie: " HTTP_AUTH_COOKIE_NAME "=%s\r\nContent-Length: 0\r\n\r\n", csrf, admin);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 200);
  CHECK(strstr(resp, "Max-Age=0") != NULL);
  (void)snprintf(req, sizeof(req), "GET /api/shares HTTP/1.1\r\nHost: l\r\nCookie: " HTTP_AUTH_COOKIE_NAME "=%s\r\n\r\n", admin);
  CHECK(exchange(port, req, resp, sizeof(resp)) > 0 && status_of(resp) == 401);

  for (unsigned i = 0U; i < 200U && atomic_load(&server->connection_count) != 0; i++)
    usleep(1000U);
  event_loop_stop(loop);
  CHECK(pthread_join(thread, NULL) == 0);
  http_server_destroy(server);
  event_loop_destroy(loop);

  char path[800];
  (void)snprintf(path, sizeof(path), "%s/pub/up.txt", root); unlink(path);
  unlink(file); rmdir(pub); rmdir(root);
  (void)snprintf(path, sizeof(path), "%s/zhttp_shares.db", state); unlink(path);
  (void)snprintf(path, sizeof(path), "%s/zhttp_users.db", state); unlink(path);
  (void)snprintf(path, sizeof(path), "%s/zhttp_access.db", state); unlink(path);
  rmdir(state);
  unsetenv("ZFTPD_ADMIN_PASSWORD");
  puts("test_http_auth_server: ok");
  return 0;
}
