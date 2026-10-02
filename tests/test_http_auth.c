/* Login gate: user store, sessions, folder rules and route authorisation. */
#include "http_api.h"
#include "http_auth.h"
#include "http_json.h"
#include "../src/http/http_api_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static int status_of(const http_response_t *resp) {
  if (resp == NULL || resp->used < 12U) return -1;
  return atoi(resp->data + 9);
}

static void make_request(http_request_t *req, http_method_t method,
                         const char *uri, const char *cookie, char *body) {
  memset(req, 0, sizeof(*req));
  req->method = method;
  (void)snprintf(req->uri, sizeof(req->uri), "%s", uri);
  if (cookie != NULL) {
    static char name[] = "Cookie";
    static char value[256];
    (void)snprintf(value, sizeof(value), "other=1; " HTTP_AUTH_COOKIE_NAME "=%s; x=y", cookie);
    req->headers[0].name = name;
    req->headers[0].value = value;
    req->num_headers = 1;
  }
  req->body = body;
  req->body_length = body != NULL ? strlen(body) : 0U;
}

/* Runs the gate; returns the HTTP status it would send, or 0 when allowed. */
static int gate(http_method_t method, const char *uri, const char *cookie,
                char *body, http_auth_identity_t *id) {
  http_request_t req;
  make_request(&req, method, uri, cookie, body);
  http_response_t *resp = http_auth_gate(&req, id);
  if (resp == NULL) return 0;
  int code = status_of(resp);
  http_response_destroy(resp);
  return code;
}

static int test_json_readers(void) {
  char out[64];
  int64_t n = 0;
  int flag = -1;
  const char *body = "{\"login\": \"ad\\\"min\", \"ttl\":3600, \"never\":true, "
                     "\"folders\": [\"/a\", 5, \"/b c\"], \"nested\":{\"x\":\"y\"}}";
  CHECK(http_json_body_string(body, "login", out, sizeof(out)) == 1);
  CHECK(strcmp(out, "ad\"min") == 0);
  CHECK(http_json_body_string(body, "missing", out, sizeof(out)) == 0);
  CHECK(http_json_body_string(body, "x", out, sizeof(out)) == 1 && strcmp(out, "y") == 0);
  CHECK(http_json_body_i64(body, "ttl", &n) == 1 && n == 3600);
  CHECK(http_json_body_i64(body, "login", &n) == 0);
  CHECK(http_json_body_bool(body, "never", &flag) == 1 && flag == 1);
  const char *cursor = NULL;
  CHECK(http_json_body_array_next(body, "folders", &cursor, out, sizeof(out)) == 1);
  CHECK(strcmp(out, "/a") == 0);
  CHECK(http_json_body_array_next(body, "folders", &cursor, out, sizeof(out)) == 1);
  CHECK(strcmp(out, "/b c") == 0);
  CHECK(http_json_body_array_next(body, "folders", &cursor, out, sizeof(out)) == 0);
  cursor = NULL;
  CHECK(http_json_body_array_next(body, "nope", &cursor, out, sizeof(out)) == 0);

  http_strbuf_t b;
  http_strbuf_init(&b);
  CHECK(http_strbuf_append_json(&b, "a\"b\\c\n") == 0);
  CHECK(http_strbuf_append_html(&b, "<&>") == 0);
  CHECK(http_strbuf_append_url(&b, "a b/c%") == 0);
  size_t len = 0U;
  char *s = http_strbuf_take(&b, &len);
  CHECK(s != NULL && strcmp(s, "a\\\"b\\\\c\\n&lt;&amp;&gt;a%20b/c%25") == 0);
  free(s);
  return 0;
}

int main(void) {
  char root[] = "/tmp/zftpd-auth-root-XXXXXX";
  char state[] = "/tmp/zftpd-auth-state-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  CHECK(mkdtemp(state) != NULL);
  http_api_set_root(root);
  http_auth_set_state_dir(state);

  char pub[1024], priv[1024], pub_file[1024];
  (void)snprintf(pub, sizeof(pub), "%s/public", root);
  (void)snprintf(priv, sizeof(priv), "%s/private", root);
  (void)snprintf(pub_file, sizeof(pub_file), "%s/public/readme.txt", root);
  CHECK(mkdir(pub, 0755) == 0 && mkdir(priv, 0755) == 0);
  FILE *f = fopen(pub_file, "w");
  CHECK(f != NULL);
  fputs("hello", f);
  fclose(f);

  CHECK(test_json_readers() == 0);

  /* --- Gate disabled: everything passes, identity acts as admin. --- */
  unsetenv("ZFTPD_ADMIN_PASSWORD");
  unsetenv("ZFTPD_HTTP_AUTH");
  CHECK(http_auth_init() == 0);
  CHECK(http_auth_enabled() == 0);
  http_auth_identity_t id;
  CHECK(gate(HTTP_METHOD_GET, "/api/processes", NULL, NULL, &id) == 0);
  CHECK(id.role == HTTP_ROLE_ADMIN && id.authenticated == 0);

  /* --- Enabled through the environment. --- */
  CHECK(setenv("ZFTPD_ADMIN_PASSWORD", "hunter22", 1) == 0);
  CHECK(http_auth_init() == 0);
  CHECK(http_auth_enabled() == 1);
  CHECK(http_auth_user_count() == 1U);
  http_auth_user_t u;
  CHECK(http_auth_user_get(0, &u) == 0);
  CHECK(strcmp(u.login, "admin") == 0 && u.role == HTTP_ROLE_ADMIN);
  CHECK(http_auth_verify("admin", "hunter22", NULL) == 0);
  CHECK(http_auth_verify("admin", "hunter23", NULL) != 0);
  CHECK(http_auth_verify("nobody", "hunter22", NULL) != 0);

  /* Public and gated routes without a session. */
  CHECK(gate(HTTP_METHOD_GET, "/api/status", NULL, NULL, &id) == 0);
  CHECK(gate(HTTP_METHOD_GET, "/index.html", NULL, NULL, &id) == 0);
  CHECK(gate(HTTP_METHOD_GET, "/s/0123456789abcdef0123456789abcdef", NULL, NULL, &id) == 0);
  CHECK(gate(HTTP_METHOD_GET, "/api/list?path=/", NULL, NULL, &id) == 401);
  CHECK(gate(HTTP_METHOD_POST, "/api/auth/login", NULL, NULL, &id) == 0);

  /* Users. */
  char err[128];
  CHECK(http_auth_user_add("bob", "secret1", HTTP_ROLE_USER, err, sizeof(err)) == 0);
  CHECK(http_auth_user_add("bob", "secret1", HTTP_ROLE_USER, err, sizeof(err)) != 0);
  CHECK(http_auth_user_add("bad login", "secret1", HTTP_ROLE_USER, err, sizeof(err)) != 0);
  CHECK(http_auth_user_add("eve", "short", HTTP_ROLE_USER, err, sizeof(err)) != 0);
  CHECK(http_auth_user_delete("admin", err, sizeof(err)) != 0); /* last admin */
  CHECK(http_auth_user_update("admin", NULL, HTTP_ROLE_USER, err, sizeof(err)) != 0);

  /* Sessions and identification through the cookie. */
  char admin_token[HTTP_AUTH_TOKEN_HEX + 1U];
  char bob_token[HTTP_AUTH_TOKEN_HEX + 1U];
  CHECK(http_auth_session_create("admin", admin_token, sizeof(admin_token)) == 0);
  CHECK(http_auth_session_create("bob", bob_token, sizeof(bob_token)) == 0);
  CHECK(strlen(admin_token) == HTTP_AUTH_TOKEN_HEX);
  CHECK(http_auth_session_count() == 2U);

  CHECK(gate(HTTP_METHOD_GET, "/api/processes", admin_token, NULL, &id) == 0);
  CHECK(id.authenticated == 1 && id.role == HTTP_ROLE_ADMIN && strcmp(id.login, "admin") == 0);
  CHECK(gate(HTTP_METHOD_GET, "/api/processes", bob_token, NULL, &id) == 403);
  CHECK(gate(HTTP_METHOD_GET, "/api/processes", "deadbeef", NULL, &id) == 401);

  /* Bearer tokens work for API clients. */
  {
    http_request_t req;
    make_request(&req, HTTP_METHOD_GET, "/api/processes", NULL, NULL);
    static char name[] = "Authorization";
    char value[128];
    (void)snprintf(value, sizeof(value), "Bearer %s", admin_token);
    req.headers[0].name = name;
    req.headers[0].value = value;
    req.num_headers = 1;
    http_response_t *resp = http_auth_gate(&req, &id);
    CHECK(resp == NULL && id.role == HTTP_ROLE_ADMIN);
    CHECK(req.auth_role == (int)HTTP_ROLE_ADMIN && strcmp(req.auth_login, "admin") == 0);
  }

  /* Folder rules: nothing allowed until the admin says so. */
  char uri[2048];
  (void)snprintf(uri, sizeof(uri), "/api/list?path=%s", pub);
  CHECK(gate(HTTP_METHOD_GET, uri, bob_token, NULL, &id) == 403);
  const char *folders[] = {pub};
  CHECK(http_auth_folders_set(folders, 1U, err, sizeof(err)) == 0);
  CHECK(http_auth_folder_count() == 1U);
  CHECK(gate(HTTP_METHOD_GET, uri, bob_token, NULL, &id) == 0);
  (void)snprintf(uri, sizeof(uri), "/api/list?path=%s", priv);
  CHECK(gate(HTTP_METHOD_GET, uri, bob_token, NULL, &id) == 403);
  CHECK(gate(HTTP_METHOD_GET, "/api/list", bob_token, NULL, &id) == 403); /* defaults to / */
  (void)snprintf(uri, sizeof(uri), "/api/file/get?path=%s/readme.txt", pub);
  CHECK(gate(HTTP_METHOD_GET, uri, bob_token, NULL, &id) == 0);
  (void)snprintf(uri, sizeof(uri), "/api/file/get?path=%s/../private/x", pub);
  CHECK(gate(HTTP_METHOD_GET, uri, bob_token, NULL, &id) == 403);
  (void)snprintf(uri, sizeof(uri), "/api/copy?path=%s/readme.txt&dst=%s", pub, priv);
  CHECK(gate(HTTP_METHOD_POST, uri, bob_token, NULL, &id) == 403);
  (void)snprintf(uri, sizeof(uri), "/api/copy?path=%s/readme.txt&dst=%s", pub, pub);
  CHECK(gate(HTTP_METHOD_POST, uri, bob_token, NULL, &id) == 0);
  {
    char body[2048];
    (void)snprintf(body, sizeof(body), "{\"paths\":[\"%s/readme.txt\",\"%s\"]}", pub, priv);
    CHECK(gate(HTTP_METHOD_POST, "/api/archive/zip", bob_token, body, &id) == 403);
    (void)snprintf(body, sizeof(body), "{\"paths\":[\"%s/readme.txt\"],\"name\":\"a.zip\"}", pub);
    CHECK(gate(HTTP_METHOD_POST, "/api/archive/zip", bob_token, body, &id) == 0);
  }
  /* Admin routes and share management stay closed to users. */
  CHECK(gate(HTTP_METHOD_GET, "/api/shares", bob_token, NULL, &id) == 403);
  CHECK(gate(HTTP_METHOD_GET, "/api/auth/users", bob_token, NULL, &id) == 403);
  CHECK(gate(HTTP_METHOD_GET, "/api/shares", admin_token, NULL, &id) == 0);
  CHECK(gate(HTTP_METHOD_GET, "/api/auth/me", bob_token, NULL, &id) == 0);
  /* Rules must point at real folders inside the root. */
  const char *outside[] = {"/"};
  CHECK(http_auth_folders_set(outside, 1U, err, sizeof(err)) != 0);
  const char *missing[] = {"/does/not/exist"};
  CHECK(http_auth_folders_set(missing, 1U, err, sizeof(err)) != 0);
  CHECK(http_auth_folder_count() == 1U);

  /* Role change applies to live sessions; deletion drops them. */
  CHECK(http_auth_user_update("bob", NULL, HTTP_ROLE_ADMIN, err, sizeof(err)) == 0);
  CHECK(gate(HTTP_METHOD_GET, "/api/processes", bob_token, NULL, &id) == 0);
  CHECK(http_auth_user_update("bob", "newpass1", HTTP_ROLE_USER, err, sizeof(err)) == 0);
  CHECK(http_auth_verify("bob", "newpass1", NULL) == 0);
  CHECK(http_auth_user_delete("bob", err, sizeof(err)) == 0);
  CHECK(gate(HTTP_METHOD_GET, "/api/auth/me", bob_token, NULL, &id) == 0 && id.authenticated == 0);
  CHECK(http_auth_session_count() == 1U);
  http_auth_session_destroy(admin_token);
  CHECK(http_auth_session_count() == 0U);

  /* Persistence: a restart without the env var keeps the gate and the data. */
  CHECK(http_auth_user_add("carol", "carolpw", HTTP_ROLE_USER, err, sizeof(err)) == 0);
  unsetenv("ZFTPD_ADMIN_PASSWORD");
  CHECK(http_auth_init() == 0);
  CHECK(http_auth_enabled() == 1);
  CHECK(http_auth_user_count() == 2U);
  CHECK(http_auth_verify("carol", "carolpw", NULL) == 0);
  CHECK(http_auth_verify("admin", "hunter22", NULL) == 0);
  CHECK(http_auth_folder_count() == 1U);
  CHECK(strcmp(http_auth_folder_get(0), pub) == 0);

  /* ZFTPD_HTTP_AUTH=0 forces the gate off. */
  CHECK(setenv("ZFTPD_HTTP_AUTH", "0", 1) == 0);
  CHECK(http_auth_init() == 0);
  CHECK(http_auth_enabled() == 0);
  CHECK(gate(HTTP_METHOD_GET, "/api/processes", NULL, NULL, &id) == 0);
  unsetenv("ZFTPD_HTTP_AUTH");

  /* Cookie values. */
  char cookie[200];
  CHECK(http_auth_cookie_value("abc", 60L, cookie, sizeof(cookie)) == 0);
  CHECK(strstr(cookie, HTTP_AUTH_COOKIE_NAME "=abc;") != NULL && strstr(cookie, "HttpOnly") != NULL);
  CHECK(http_auth_cookie_value(NULL, 0L, cookie, sizeof(cookie)) == 0);
  CHECK(strstr(cookie, "Max-Age=0") != NULL);

  /* Status fragment. */
  {
    http_request_t req;
    make_request(&req, HTTP_METHOD_GET, "/api/status", NULL, NULL);
    char buf[256];
    size_t pos = 0U;
    CHECK(http_auth_status_json(&req, buf, sizeof(buf), &pos) == 0);
    buf[pos] = '\0';
    CHECK(strstr(buf, "\"auth\":{\"enabled\":false") != NULL);
  }

  http_auth_shutdown();
  unlink(pub_file);
  rmdir(pub);
  rmdir(priv);
  rmdir(root);
  char path[1200];
  (void)snprintf(path, sizeof(path), "%s/zhttp_users.db", state);
  unlink(path);
  (void)snprintf(path, sizeof(path), "%s/zhttp_access.db", state);
  unlink(path);
  rmdir(state);
  puts("test_http_auth: ok");
  return 0;
}
