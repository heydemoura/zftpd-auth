/* Share links: store, expiry, public download and folder listing. */
#include "http_api.h"
#include "http_auth.h"
#include "http_share.h"
#include "../src/http/http_api_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static int status_of(const http_response_t *resp) {
  if (resp == NULL || resp->used < 12U) return -1;
  return atoi(resp->data + 9);
}

static http_response_t *get(const char *uri) {
  http_request_t req;
  memset(&req, 0, sizeof(req));
  req.method = HTTP_METHOD_GET;
  (void)snprintf(req.uri, sizeof(req.uri), "%s", uri);
  return http_api_handle(&req);
}

static int write_text(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (f == NULL) return -1;
  fputs(text, f);
  fclose(f);
  return 0;
}

int main(void) {
  char root[] = "/tmp/zftpd-share-root-XXXXXX";
  char state[] = "/tmp/zftpd-share-state-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  CHECK(mkdtemp(state) != NULL);
  http_api_set_root(root);
  http_auth_set_state_dir(state);
  unsetenv("ZFTPD_ADMIN_PASSWORD");
  unsetenv("ZFTPD_HTTP_AUTH");
  CHECK(http_auth_init() == 0);
  http_share_init();
  CHECK(http_share_count() == 0U);

  char folder[1024], sub[1024], file[1024], subfile[1024], secret[1024];
  (void)snprintf(folder, sizeof(folder), "%s/photos", root);
  (void)snprintf(sub, sizeof(sub), "%s/photos/2026 trip", root);
  (void)snprintf(file, sizeof(file), "%s/photos/a&b.txt", root);
  (void)snprintf(subfile, sizeof(subfile), "%s/photos/2026 trip/notes.txt", root);
  (void)snprintf(secret, sizeof(secret), "%s/secret.txt", root);
  CHECK(mkdir(folder, 0755) == 0 && mkdir(sub, 0755) == 0);
  CHECK(write_text(file, "hello") == 0);
  CHECK(write_text(subfile, "notes") == 0);
  CHECK(write_text(secret, "top secret") == 0);

  char err[128];
  http_share_t f, d, old;
  CHECK(http_share_create(file, 0, "admin", &f, err, sizeof(err)) == 0);
  CHECK(http_share_create(folder, time(NULL) + 3600, "admin", &d, err, sizeof(err)) == 0);
  CHECK(http_share_create("/etc/passwd", 0, "admin", NULL, err, sizeof(err)) != 0);
  CHECK(http_share_create(root, 0, "admin", NULL, err, sizeof(err)) != 0);
  CHECK(http_share_create(file, time(NULL) - 10, "admin", NULL, err, sizeof(err)) != 0);
  {
    char missing[1100];
    (void)snprintf(missing, sizeof(missing), "%s/nope", root);
    CHECK(http_share_create(missing, 0, "admin", NULL, err, sizeof(err)) != 0);
  }
  CHECK(http_share_count() == 2U);
  CHECK(f.is_dir == 0 && d.is_dir == 1 && strlen(f.id) == HTTP_SHARE_ID_HEX);

  /* File share: direct download with attachment disposition. */
  char uri[256];
  (void)snprintf(uri, sizeof(uri), "/s/%s", f.id);
  http_response_t *resp = get(uri);
  CHECK(status_of(resp) == 200);
  CHECK(resp->sendfile_fd >= 0 && resp->sendfile_count == 5U);
  CHECK(strstr(resp->data, "Content-Disposition: attachment; filename=\"a&b.txt\"") != NULL);
  http_response_destroy(resp);
  (void)snprintf(uri, sizeof(uri), "/s/%s/extra", f.id);
  resp = get(uri);
  CHECK(status_of(resp) == 404);
  http_response_destroy(resp);

  /* Folder share: HTML listing with encoded links, subfolder, file, ZIP. */
  (void)snprintf(uri, sizeof(uri), "/s/%s", d.id);
  resp = get(uri);
  CHECK(status_of(resp) == 200);
  CHECK(strstr(resp->data, "Content-Type: text/html") != NULL);
  const char *html = (const char *)resp->mem_body;
  CHECK(html != NULL);
  CHECK(strstr(html, "a&amp;b.txt") != NULL);
  CHECK(strstr(html, "2026%20trip/") != NULL);
  CHECK(strstr(html, "?zip=1") != NULL);
  CHECK(strstr(html, "This link expires on") != NULL);
  http_response_destroy(resp);

  (void)snprintf(uri, sizeof(uri), "/s/%s/2026%%20trip/", d.id);
  resp = get(uri);
  CHECK(status_of(resp) == 200);
  CHECK(strstr((const char *)resp->mem_body, "notes.txt") != NULL);
  http_response_destroy(resp);

  (void)snprintf(uri, sizeof(uri), "/s/%s/2026%%20trip/notes.txt", d.id);
  resp = get(uri);
  CHECK(status_of(resp) == 200 && resp->sendfile_count == 5U);
  http_response_destroy(resp);

  (void)snprintf(uri, sizeof(uri), "/s/%s/2026%%20trip?zip=1", d.id);
  resp = get(uri);
  CHECK(status_of(resp) == 200);
  CHECK(strstr(resp->data, "2026 trip.zip") != NULL);
  http_response_destroy(resp);

  /* Nothing outside the shared folder is reachable through it. */
  (void)snprintf(uri, sizeof(uri), "/s/%s/../secret.txt", d.id);
  resp = get(uri);
  CHECK(status_of(resp) == 404);
  http_response_destroy(resp);
  (void)snprintf(uri, sizeof(uri), "/s/%s/..%%2Fsecret.txt", d.id);
  resp = get(uri);
  CHECK(status_of(resp) == 404);
  http_response_destroy(resp);
  (void)snprintf(uri, sizeof(uri), "/s/%s/missing.txt", d.id);
  resp = get(uri);
  CHECK(status_of(resp) == 404);
  http_response_destroy(resp);

  /* Unknown and malformed ids. */
  resp = get("/s/00000000000000000000000000000000");
  CHECK(status_of(resp) == 404);
  http_response_destroy(resp);
  resp = get("/s/short");
  CHECK(status_of(resp) == 404);
  http_response_destroy(resp);

  /* Expiry: an expired link answers 410 and purge removes it. */
  CHECK(http_share_create(file, time(NULL) + 1, "admin", &old, err, sizeof(err)) == 0);
  sleep(2);
  (void)snprintf(uri, sizeof(uri), "/s/%s", old.id);
  resp = get(uri);
  CHECK(status_of(resp) == 410);
  http_response_destroy(resp);
  http_share_t found;
  CHECK(http_share_find(old.id, &found) == 0 && http_share_is_expired(&found, time(NULL)));
  CHECK(http_share_purge_expired() == 1U);
  CHECK(http_share_find(old.id, NULL) != 0);
  CHECK(http_share_count() == 2U);

  /* Share links stay public when the login gate is on. */
  CHECK(setenv("ZFTPD_ADMIN_PASSWORD", "hunter22", 1) == 0);
  CHECK(http_auth_init() == 0 && http_auth_enabled() == 1);
  (void)snprintf(uri, sizeof(uri), "/s/%s", f.id);
  resp = get(uri);
  CHECK(status_of(resp) == 200);
  http_response_destroy(resp);
  resp = get("/api/shares");
  CHECK(status_of(resp) == 401);
  http_response_destroy(resp);
  unsetenv("ZFTPD_ADMIN_PASSWORD");
  CHECK(setenv("ZFTPD_HTTP_AUTH", "0", 1) == 0);
  CHECK(http_auth_init() == 0 && http_auth_enabled() == 0);

  /* Persistence and deletion. */
  http_share_shutdown();
  http_share_init();
  CHECK(http_share_count() == 2U);
  CHECK(http_share_find(d.id, &found) == 0 && strcmp(found.path, folder) == 0 && found.is_dir == 1);
  CHECK(http_share_delete(d.id) == 0);
  CHECK(http_share_delete(d.id) != 0);
  CHECK(http_share_count() == 1U);
  (void)snprintf(uri, sizeof(uri), "/s/%s", d.id);
  resp = get(uri);
  CHECK(status_of(resp) == 404);
  http_response_destroy(resp);

  /* Management API without the gate: list, create, delete, purge. */
  resp = get("/api/shares");
  CHECK(status_of(resp) == 200);
  CHECK(strstr((const char *)resp->mem_body, "\"shares\":[{\"id\":\"") != NULL);
  http_response_destroy(resp);
  {
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = HTTP_METHOD_POST;
    (void)snprintf(req.uri, sizeof(req.uri), "/api/shares/create");
    char body[1200];
    (void)snprintf(body, sizeof(body), "{\"path\":\"%s\",\"ttl\":60}", folder);
    req.body = body;
    req.body_length = strlen(body);
    resp = http_api_auth_handle(&req);
    CHECK(status_of(resp) == 200);
    const char *id = strstr((const char *)resp->mem_body, "\"id\":\"");
    CHECK(id != NULL);
    char new_id[HTTP_SHARE_ID_HEX + 1U];
    memcpy(new_id, id + 6, HTTP_SHARE_ID_HEX);
    new_id[HTTP_SHARE_ID_HEX] = '\0';
    http_response_destroy(resp);
    CHECK(http_share_find(new_id, &found) == 0 && found.expires > time(NULL));

    (void)snprintf(req.uri, sizeof(req.uri), "/api/shares/delete");
    (void)snprintf(body, sizeof(body), "{\"id\":\"%s\"}", new_id);
    req.body_length = strlen(body);
    resp = http_api_auth_handle(&req);
    CHECK(status_of(resp) == 200);
    http_response_destroy(resp);
    CHECK(http_share_find(new_id, NULL) != 0);
  }

  http_share_shutdown();
  http_auth_shutdown();
  unlink(subfile); unlink(file); unlink(secret);
  rmdir(sub); rmdir(folder); rmdir(root);
  char path[1200];
  (void)snprintf(path, sizeof(path), "%s/zhttp_shares.db", state); unlink(path);
  (void)snprintf(path, sizeof(path), "%s/zhttp_users.db", state); unlink(path);
  (void)snprintf(path, sizeof(path), "%s/zhttp_access.db", state); unlink(path);
  rmdir(state);
  unsetenv("ZFTPD_HTTP_AUTH");
  puts("test_http_share: ok");
  return 0;
}
