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
 * @file http_auth.h
 * @brief Optional login gate for the web interface: users, roles, sessions
 *        and the folder allow-list applied to the "user" role.
 *
 * The gate is off unless configured.  Configuration comes from the
 * environment at startup (see http_auth_init()) and from the persisted user
 * store, which the admin manages through the web interface:
 *
 *   ZFTPD_ADMIN_PASSWORD   enables the gate and (re)sets the admin password
 *   ZFTPD_ADMIN_USER       admin login name (default "admin")
 *   ZFTPD_HTTP_AUTH        "0" forces the gate off, "1" forces it on
 *   ZFTPD_HTTP_SESSION_TTL session lifetime in seconds (default 7 days)
 *   ZFTPD_STATE_DIR        where users, access rules and shares are stored
 *
 * Without ZFTPD_ADMIN_PASSWORD the gate stays enabled as long as the stored
 * user list contains an admin, so a restart never silently opens the
 * interface again.
 */

#ifndef HTTP_AUTH_H
#define HTTP_AUTH_H

#include "http_parser.h"
#include "http_response.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef enum {
  HTTP_ROLE_NONE = 0, /**< not authenticated */
  HTTP_ROLE_USER = 1, /**< restricted to the allowed folders */
  HTTP_ROLE_ADMIN = 2 /**< full access, manages users and shares */
} http_role_t;

#define HTTP_AUTH_LOGIN_MAX 32U
#define HTTP_AUTH_PASSWORD_MIN 6U
#define HTTP_AUTH_PASSWORD_MAX 128U
#define HTTP_AUTH_MAX_USERS 64U
#define HTTP_AUTH_MAX_SESSIONS 64U
#define HTTP_AUTH_MAX_FOLDERS 32U
#define HTTP_AUTH_TOKEN_HEX 64U
#define HTTP_AUTH_COOKIE_NAME "zftpd_session"
#define HTTP_AUTH_DEFAULT_SESSION_TTL (7L * 24L * 3600L)

typedef struct {
  char login[HTTP_AUTH_LOGIN_MAX + 1U];
  http_role_t role;
} http_auth_user_t;

typedef struct {
  int authenticated;                   /**< 1 when a valid session was sent */
  http_role_t role;                    /**< effective role for this request */
  char login[HTTP_AUTH_LOGIN_MAX + 1U]; /**< empty when not authenticated   */
} http_auth_identity_t;

/*---------------------------------------------------------------------------*
 * Lifecycle and configuration
 *---------------------------------------------------------------------------*/

/** Directory that holds the persisted stores; NULL restores the default. */
void http_auth_set_state_dir(const char *dir);
int http_auth_state_path(const char *file, char *out, size_t out_size);

/**
 * @brief Load the stores and read the environment.  Safe to call when the
 *        gate ends up disabled.  Returns 0 on success.
 */
int http_auth_init(void);
void http_auth_shutdown(void);

int http_auth_enabled(void);
void http_auth_set_enabled(int enabled);
long http_auth_session_ttl(void);
void http_auth_set_session_ttl(long seconds);

/** Create or update the admin account with this password (persisted). */
int http_auth_configure_admin(const char *login, const char *password);

/*---------------------------------------------------------------------------*
 * Users
 *---------------------------------------------------------------------------*/

size_t http_auth_user_count(void);
int http_auth_user_get(size_t index, http_auth_user_t *out);
int http_auth_user_find(const char *login, http_auth_user_t *out);
int http_auth_user_add(const char *login, const char *password,
                       http_role_t role, char *err, size_t err_size);
/** password NULL keeps the current one; role HTTP_ROLE_NONE keeps the role. */
int http_auth_user_update(const char *login, const char *password,
                          http_role_t role, char *err, size_t err_size);
int http_auth_user_delete(const char *login, char *err, size_t err_size);
int http_auth_verify(const char *login, const char *password,
                     http_role_t *role);
int http_auth_login_valid(const char *login);
const char *http_auth_role_name(http_role_t role);
http_role_t http_auth_role_parse(const char *name);

/*---------------------------------------------------------------------------*
 * Sessions (in memory; a restart logs everyone out)
 *---------------------------------------------------------------------------*/

int http_auth_session_create(const char *login, char *token,
                             size_t token_size);
void http_auth_session_destroy(const char *token);
void http_auth_sessions_drop_user(const char *login);
size_t http_auth_session_count(void);

/** Session token carried by the request (cookie or bearer), if any. */
int http_auth_request_token(const http_request_t *request, char *out,
                            size_t out_size);
/** Resolve the request to an identity; with the gate off the identity is an
 *  unauthenticated admin so every handler keeps working as before. */
void http_auth_identify(const http_request_t *request,
                        http_auth_identity_t *out);
/** Set-Cookie value that installs (ttl > 0) or clears (ttl == 0) a session. */
int http_auth_cookie_value(const char *token, long ttl, char *out,
                           size_t out_size);

/*---------------------------------------------------------------------------*
 * Folder allow-list for the user role
 *---------------------------------------------------------------------------*/

size_t http_auth_folder_count(void);
const char *http_auth_folder_get(size_t index);
int http_auth_folders_set(const char *const *folders, size_t count, char *err,
                          size_t err_size);
/** 1 when the (raw, request-supplied) path is inside an allowed folder. */
int http_auth_path_allowed(const http_auth_identity_t *identity,
                           const char *path);

/*---------------------------------------------------------------------------*
 * Request gate
 *---------------------------------------------------------------------------*/

/**
 * @brief Authorise a request.  Fills the identity and the request's auth
 *        fields.  Returns NULL when the request may proceed, otherwise the
 *        401/403 response to send instead.
 */
http_response_t *http_auth_gate(http_request_t *request,
                                http_auth_identity_t *identity);

/** JSON fragment describing the gate state for /api/status. */
int http_auth_status_json(const http_request_t *request, char *buf,
                          size_t cap, size_t *pos);

#endif /* HTTP_AUTH_H */
