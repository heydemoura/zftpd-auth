/*
MIT License

Copyright (c) 2026 Seregon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/**
 * @file http_api_auth.c
 * @brief Login, account management, folder rules and share management.
 *
 *   POST /api/auth/login            {login,password}        -> session cookie
 *   POST /api/auth/logout
 *   GET  /api/auth/me
 *   POST /api/auth/password         {current,password}
 *   GET  /api/auth/users                                     (admin)
 *   POST /api/auth/users            {login,password,role}    (admin)
 *   POST /api/auth/users/update     {login,password?,role?}  (admin)
 *   POST /api/auth/users/delete     {login}                  (admin)
 *   GET  /api/auth/access                                    (admin)
 *   POST /api/auth/access           {folders:[...]}          (admin)
 *   GET  /api/shares                                         (admin)
 *   POST /api/shares/create         {path,expires|ttl}       (admin)
 *   POST /api/shares/delete         {id}                     (admin)
 *   POST /api/shares/purge                                   (admin)
 *
 * The route gate (http_auth_gate) already restricts the admin routes; the
 * checks here are the second line of defence.
 */

#include "http_api.h"
#include "http_api_internal.h"
#include "http_auth.h"
#include "http_json.h"
#include "http_share.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOGIN_FAILURE_DELAY_US 300000U

static int is_admin(const http_request_t *request) {
  return !http_auth_enabled() || request->auth_role == (int)HTTP_ROLE_ADMIN;
}

static http_response_t *json_response(http_status_t status, http_strbuf_t *b) {
  size_t len = 0U;
  char *body = http_strbuf_take(b, &len);
  if (body == NULL)
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  http_response_t *resp = http_response_create(status);
  if (resp == NULL) {
    free(body);
    return NULL;
  }
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  if (http_response_set_body_owned(resp, body, len) != 0) {
    free(body);
    http_response_destroy(resp);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }
  return resp;
}

static http_response_t *ok_json(void) {
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  static const char body[] = "{\"ok\":true}";
  http_response_set_body(resp, body, sizeof(body) - 1U);
  return resp;
}

static http_response_t *require_post(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  return NULL;
}

static http_response_t *require_admin(const http_request_t *request) {
  if (!is_admin(request))
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN,
                               "Administrator access required");
  return NULL;
}

/*===========================================================================*
 * Sessions
 *===========================================================================*/

static void append_identity(http_strbuf_t *b, const char *login,
                            http_role_t role) {
  http_strbuf_append_cstr(b, "\"user\":\"");
  http_strbuf_append_json(b, login);
  http_strbuf_append_cstr(b, "\",\"role\":\"");
  http_strbuf_append_cstr(b, http_auth_role_name(role));
  http_strbuf_append_cstr(b, "\"");
}

static void append_folders(http_strbuf_t *b) {
  http_strbuf_append_cstr(b, "\"folders\":[");
  size_t n = http_auth_folder_count();
  for (size_t i = 0U; i < n; i++) {
    const char *folder = http_auth_folder_get(i);
    if (folder == NULL) break;
    if (i > 0U) http_strbuf_append_cstr(b, ",");
    http_strbuf_append_cstr(b, "\"");
    http_strbuf_append_json(b, folder);
    http_strbuf_append_cstr(b, "\"");
  }
  http_strbuf_append_cstr(b, "]");
}

static http_response_t *api_login(const http_request_t *request) {
  http_response_t *err = require_post(request);
  if (err != NULL) return err;
  if (!http_auth_enabled()) {
    http_strbuf_t b;
    http_strbuf_init(&b);
    http_strbuf_append_cstr(&b, "{\"ok\":true,\"enabled\":false,");
    append_identity(&b, "", HTTP_ROLE_ADMIN);
    http_strbuf_append_cstr(&b, "}");
    return json_response(HTTP_STATUS_200_OK, &b);
  }

  char login[HTTP_AUTH_LOGIN_MAX + 2U];
  char password[HTTP_AUTH_PASSWORD_MAX + 2U];
  if (request->body == NULL ||
      !http_json_body_string(request->body, "login", login, sizeof(login)) ||
      !http_json_body_string(request->body, "password", password,
                             sizeof(password)))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "Missing login or password");

  http_role_t role = HTTP_ROLE_NONE;
  if (http_auth_verify(login, password, &role) != 0) {
    memset(password, 0, sizeof(password));
    usleep(LOGIN_FAILURE_DELAY_US); /* blunt brute-force throttle */
    return http_api_error_json(HTTP_STATUS_401_UNAUTHORIZED,
                               "Invalid login or password");
  }
  memset(password, 0, sizeof(password));

  char token[HTTP_AUTH_TOKEN_HEX + 1U];
  if (http_auth_session_create(login, token, sizeof(token)) != 0)
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                               "Could not create a session");

  char body[512];
  size_t pos = 0U;
  char cookie[200];
  if (http_buf_append_cstr(body, sizeof(body), &pos,
                           "{\"ok\":true,\"enabled\":true,\"user\":\"") != 0 ||
      http_json_escape_append(body, sizeof(body), &pos, login) != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos, "\",\"role\":\"") != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos,
                           http_auth_role_name(role)) != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos, "\",\"token\":\"") != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos, token) != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos, "\",\"expires_in\":") != 0 ||
      http_buf_append_u64(body, sizeof(body), &pos,
                          (uint64_t)http_auth_session_ttl()) != 0 ||
      http_buf_append_cstr(body, sizeof(body), &pos, "}") != 0 ||
      http_auth_cookie_value(token, http_auth_session_ttl(), cookie,
                             sizeof(cookie)) != 0)
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                               "Response too large");

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_add_header(resp, "Set-Cookie", cookie);
  http_response_set_body(resp, body, pos);
  return resp;
}

static http_response_t *api_logout(const http_request_t *request) {
  http_response_t *err = require_post(request);
  if (err != NULL) return err;
  char token[HTTP_AUTH_TOKEN_HEX + 1U];
  if (http_auth_request_token(request, token, sizeof(token)) == 0)
    http_auth_session_destroy(token);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  char cookie[200];
  if (http_auth_cookie_value(NULL, 0L, cookie, sizeof(cookie)) == 0)
    http_response_add_header(resp, "Set-Cookie", cookie);
  static const char body[] = "{\"ok\":true}";
  http_response_set_body(resp, body, sizeof(body) - 1U);
  return resp;
}

static http_response_t *api_me(const http_request_t *request) {
  http_auth_identity_t id;
  http_auth_identify(request, &id);
  int enabled = http_auth_enabled();
  http_strbuf_t b;
  http_strbuf_init(&b);
  http_strbuf_append_cstr(&b, "{\"ok\":true,\"enabled\":");
  http_strbuf_append_cstr(&b, enabled ? "true" : "false");
  http_strbuf_append_cstr(&b, ",\"authenticated\":");
  http_strbuf_append_cstr(&b, id.authenticated ? "true" : "false");
  http_strbuf_append_cstr(&b, ",");
  append_identity(&b, id.login, enabled ? id.role : HTTP_ROLE_ADMIN);
  if (!enabled || id.authenticated) {
    http_strbuf_append_cstr(&b, ",");
    append_folders(&b);
  }
  http_strbuf_append_cstr(&b, "}");
  return json_response(HTTP_STATUS_200_OK, &b);
}

static http_response_t *api_password(const http_request_t *request) {
  http_response_t *err = require_post(request);
  if (err != NULL) return err;
  if (!http_auth_enabled())
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "The login gate is disabled");
  if (request->auth_login[0] == '\0')
    return http_api_error_json(HTTP_STATUS_401_UNAUTHORIZED, "Login required");

  char current[HTTP_AUTH_PASSWORD_MAX + 2U];
  char password[HTTP_AUTH_PASSWORD_MAX + 2U];
  if (request->body == NULL ||
      !http_json_body_string(request->body, "current", current, sizeof(current)) ||
      !http_json_body_string(request->body, "password", password,
                             sizeof(password)))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "Missing current or new password");
  if (http_auth_verify(request->auth_login, current, NULL) != 0) {
    usleep(LOGIN_FAILURE_DELAY_US);
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN,
                               "The current password is wrong");
  }
  char reason[128];
  if (http_auth_user_update(request->auth_login, password, HTTP_ROLE_NONE,
                            reason, sizeof(reason)) != 0)
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, reason);
  return ok_json();
}

/*===========================================================================*
 * Users (admin)
 *===========================================================================*/

static http_response_t *api_users_list(void) {
  http_strbuf_t b;
  http_strbuf_init(&b);
  http_strbuf_append_cstr(&b, "{\"ok\":true,\"users\":[");
  size_t n = http_auth_user_count();
  for (size_t i = 0U; i < n; i++) {
    http_auth_user_t user;
    if (http_auth_user_get(i, &user) != 0) break;
    if (i > 0U) http_strbuf_append_cstr(&b, ",");
    http_strbuf_append_cstr(&b, "{\"login\":\"");
    http_strbuf_append_json(&b, user.login);
    http_strbuf_append_cstr(&b, "\",\"role\":\"");
    http_strbuf_append_cstr(&b, http_auth_role_name(user.role));
    http_strbuf_append_cstr(&b, "\"}");
  }
  http_strbuf_append_cstr(&b, "]}");
  return json_response(HTTP_STATUS_200_OK, &b);
}

static http_response_t *api_users(const http_request_t *request) {
  http_response_t *err = require_admin(request);
  if (err != NULL) return err;
  if (request->method == HTTP_METHOD_GET) return api_users_list();
  if ((err = require_post(request)) != NULL) return err;

  char login[HTTP_AUTH_LOGIN_MAX + 2U];
  char password[HTTP_AUTH_PASSWORD_MAX + 2U];
  char role_name[16];
  if (request->body == NULL ||
      !http_json_body_string(request->body, "login", login, sizeof(login)) ||
      !http_json_body_string(request->body, "password", password,
                             sizeof(password)))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "Missing login or password");
  if (!http_json_body_string(request->body, "role", role_name, sizeof(role_name)))
    (void)snprintf(role_name, sizeof(role_name), "user");

  char reason[128];
  if (http_auth_user_add(login, password, http_auth_role_parse(role_name),
                         reason, sizeof(reason)) != 0)
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, reason);
  return api_users_list();
}

static http_response_t *api_users_update(const http_request_t *request) {
  http_response_t *err = require_admin(request);
  if (err != NULL) return err;
  if ((err = require_post(request)) != NULL) return err;

  char login[HTTP_AUTH_LOGIN_MAX + 2U];
  char password[HTTP_AUTH_PASSWORD_MAX + 2U];
  char role_name[16];
  if (request->body == NULL ||
      !http_json_body_string(request->body, "login", login, sizeof(login)))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing login");
  int has_password = http_json_body_string(request->body, "password", password,
                                           sizeof(password)) && password[0] != '\0';
  http_role_t role = HTTP_ROLE_NONE;
  if (http_json_body_string(request->body, "role", role_name, sizeof(role_name)) &&
      role_name[0] != '\0') {
    role = http_auth_role_parse(role_name);
    if (role == HTTP_ROLE_NONE)
      return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                                 "Role must be admin or user");
  }
  if (role != HTTP_ROLE_NONE && http_auth_enabled() &&
      strcmp(login, request->auth_login) == 0)
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "Change your own role from another "
                               "administrator account");

  char reason[128];
  if (http_auth_user_update(login, has_password ? password : NULL, role,
                            reason, sizeof(reason)) != 0)
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, reason);
  if (has_password && strcmp(login, request->auth_login) != 0)
    http_auth_sessions_drop_user(login);
  return api_users_list();
}

static http_response_t *api_users_delete(const http_request_t *request) {
  http_response_t *err = require_admin(request);
  if (err != NULL) return err;
  if ((err = require_post(request)) != NULL) return err;

  char login[HTTP_AUTH_LOGIN_MAX + 2U];
  if (request->body == NULL ||
      !http_json_body_string(request->body, "login", login, sizeof(login)))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing login");
  if (http_auth_enabled() && strcmp(login, request->auth_login) == 0)
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "You cannot remove your own account");
  char reason[128];
  if (http_auth_user_delete(login, reason, sizeof(reason)) != 0)
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, reason);
  return api_users_list();
}

/*===========================================================================*
 * Folder rules (admin)
 *===========================================================================*/

static http_response_t *api_access_list(void) {
  http_strbuf_t b;
  http_strbuf_init(&b);
  http_strbuf_append_cstr(&b, "{\"ok\":true,");
  append_folders(&b);
  http_strbuf_append_cstr(&b, "}");
  return json_response(HTTP_STATUS_200_OK, &b);
}

static http_response_t *api_access(const http_request_t *request) {
  http_response_t *err = require_admin(request);
  if (err != NULL) return err;
  if (request->method == HTTP_METHOD_GET) return api_access_list();
  if ((err = require_post(request)) != NULL) return err;
  if (request->body == NULL)
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing JSON body");

  char *storage = calloc(HTTP_AUTH_MAX_FOLDERS + 1U, FTP_PATH_MAX);
  if (storage == NULL)
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  const char *folders[HTTP_AUTH_MAX_FOLDERS + 1U];
  size_t count = 0U;
  const char *cursor = NULL;
  while (count <= HTTP_AUTH_MAX_FOLDERS &&
         http_json_body_array_next(request->body, "folders", &cursor,
                                   storage + count * FTP_PATH_MAX,
                                   FTP_PATH_MAX)) {
    folders[count] = storage + count * FTP_PATH_MAX;
    count++;
  }
  char reason[128];
  int rc = http_auth_folders_set(folders, count, reason, sizeof(reason));
  free(storage);
  if (rc != 0) return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, reason);
  return api_access_list();
}

/*===========================================================================*
 * Shares (admin)
 *===========================================================================*/

static void append_share(http_strbuf_t *b, const http_share_t *s, time_t now) {
  const char *name = strrchr(s->path, '/');
  name = name != NULL && name[1] != '\0' ? name + 1 : s->path;
  http_strbuf_append_cstr(b, "{\"id\":\"");
  http_strbuf_append_cstr(b, s->id);
  http_strbuf_append_cstr(b, "\",\"path\":\"");
  http_strbuf_append_json(b, s->path);
  http_strbuf_append_cstr(b, "\",\"name\":\"");
  http_strbuf_append_json(b, name);
  http_strbuf_append_cstr(b, "\",\"type\":\"");
  http_strbuf_append_cstr(b, s->is_dir ? "directory" : "file");
  http_strbuf_append_cstr(b, "\",\"url\":\"" HTTP_SHARE_URL_PREFIX);
  http_strbuf_append_cstr(b, s->id);
  http_strbuf_append_cstr(b, "\",\"created\":");
  http_strbuf_append_i64(b, (int64_t)s->created);
  http_strbuf_append_cstr(b, ",\"expires\":");
  http_strbuf_append_i64(b, (int64_t)s->expires);
  http_strbuf_append_cstr(b, ",\"expired\":");
  http_strbuf_append_cstr(b, http_share_is_expired(s, now) ? "true" : "false");
  http_strbuf_append_cstr(b, ",\"owner\":\"");
  http_strbuf_append_json(b, s->owner);
  http_strbuf_append_cstr(b, "\"}");
}

static http_response_t *api_shares_list(void) {
  time_t now = time(NULL);
  http_strbuf_t b;
  http_strbuf_init(&b);
  http_strbuf_append_cstr(&b, "{\"ok\":true,\"now\":");
  http_strbuf_append_i64(&b, (int64_t)now);
  http_strbuf_append_cstr(&b, ",\"shares\":[");
  size_t n = http_share_count();
  for (size_t i = 0U; i < n; i++) {
    http_share_t s;
    if (http_share_get(i, &s) != 0) break;
    if (i > 0U) http_strbuf_append_cstr(&b, ",");
    append_share(&b, &s, now);
  }
  http_strbuf_append_cstr(&b, "]}");
  return json_response(HTTP_STATUS_200_OK, &b);
}

static http_response_t *api_shares(const http_request_t *request) {
  http_response_t *err = require_admin(request);
  if (err != NULL) return err;
  if (request->method != HTTP_METHOD_GET)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use GET");
  return api_shares_list();
}

static http_response_t *api_shares_create(const http_request_t *request) {
  http_response_t *err = require_admin(request);
  if (err != NULL) return err;
  if ((err = require_post(request)) != NULL) return err;

  char path[FTP_PATH_MAX];
  if (request->body == NULL ||
      !http_json_body_string(request->body, "path", path, sizeof(path)))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing path");

  int64_t expires = 0;
  int64_t ttl = 0;
  if (http_json_body_i64(request->body, "ttl", &ttl) && ttl > 0)
    expires = (int64_t)time(NULL) + ttl;
  else if (!http_json_body_i64(request->body, "expires", &expires))
    expires = 0;
  if (expires < 0) expires = 0;

  http_share_t share;
  char reason[128];
  if (http_share_create(path, (time_t)expires, request->auth_login, &share,
                        reason, sizeof(reason)) != 0)
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, reason);

  http_strbuf_t b;
  http_strbuf_init(&b);
  http_strbuf_append_cstr(&b, "{\"ok\":true,\"share\":");
  append_share(&b, &share, time(NULL));
  http_strbuf_append_cstr(&b, "}");
  return json_response(HTTP_STATUS_200_OK, &b);
}

static http_response_t *api_shares_delete(const http_request_t *request) {
  http_response_t *err = require_admin(request);
  if (err != NULL) return err;
  if ((err = require_post(request)) != NULL) return err;
  char id[HTTP_SHARE_ID_HEX + 2U];
  if (request->body == NULL ||
      !http_json_body_string(request->body, "id", id, sizeof(id)))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing id");
  if (http_share_delete(id) != 0)
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Share not found");
  return ok_json();
}

static http_response_t *api_shares_purge(const http_request_t *request) {
  http_response_t *err = require_admin(request);
  if (err != NULL) return err;
  if ((err = require_post(request)) != NULL) return err;
  size_t removed = http_share_purge_expired();
  http_strbuf_t b;
  http_strbuf_init(&b);
  http_strbuf_append_cstr(&b, "{\"ok\":true,\"removed\":");
  http_strbuf_append_u64(&b, (uint64_t)removed);
  http_strbuf_append_cstr(&b, "}");
  return json_response(HTTP_STATUS_200_OK, &b);
}

/*===========================================================================*
 * Router
 *===========================================================================*/

http_response_t *http_api_auth_handle(const http_request_t *request) {
  if (request == NULL) return NULL;
  const char *uri = request->uri;
  if (http_api_route_is(uri, "/api/auth/login")) return api_login(request);
  if (http_api_route_is(uri, "/api/auth/logout")) return api_logout(request);
  if (http_api_route_is(uri, "/api/auth/me")) return api_me(request);
  if (http_api_route_is(uri, "/api/auth/password")) return api_password(request);
  if (http_api_route_is(uri, "/api/auth/users/update")) return api_users_update(request);
  if (http_api_route_is(uri, "/api/auth/users/delete")) return api_users_delete(request);
  if (http_api_route_is(uri, "/api/auth/users")) return api_users(request);
  if (http_api_route_is(uri, "/api/auth/access")) return api_access(request);
  if (http_api_route_is(uri, "/api/shares/create")) return api_shares_create(request);
  if (http_api_route_is(uri, "/api/shares/delete")) return api_shares_delete(request);
  if (http_api_route_is(uri, "/api/shares/purge")) return api_shares_purge(request);
  if (http_api_route_is(uri, "/api/shares")) return api_shares(request);
  return NULL;
}
