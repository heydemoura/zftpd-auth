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
 * @file http_auth.c
 * @brief Web interface login gate: user store, sessions and folder rules.
 *
 * Storage (text, one record per line, atomic rename on save):
 *
 *   <state>/zhttp_users.db    zftpd-users 1
 *                             login \t role \t iterations \t salt \t hash
 *   <state>/zhttp_access.db   zftpd-access 1
 *                             /allowed/folder
 *
 * Passwords are PBKDF2-HMAC-SHA256 with a per-user random salt.  Sessions
 * live in memory only.
 */

#include "http_auth.h"
#include "ftp_config.h"
#include "ftp_log.h"
#include "ftp_path.h"
#include "http_api.h"
#include "http_api_internal.h"
#include "http_json.h"
#include "http_sha256.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUTH_SALT_BYTES 16U
#define AUTH_PBKDF2_ITERATIONS 20000U
#define AUTH_USERS_MAGIC "zftpd-users 1"
#define AUTH_ACCESS_MAGIC "zftpd-access 1"
#define AUTH_USERS_FILE "zhttp_users.db"
#define AUTH_ACCESS_FILE "zhttp_access.db"

typedef struct {
  char login[HTTP_AUTH_LOGIN_MAX + 1U];
  http_role_t role;
  uint32_t iterations;
  uint8_t salt[AUTH_SALT_BYTES];
  uint8_t hash[HTTP_SHA256_DIGEST_SIZE];
} auth_user_t;

typedef struct {
  char token[HTTP_AUTH_TOKEN_HEX + 1U];
  char login[HTTP_AUTH_LOGIN_MAX + 1U];
  time_t expires;
} auth_session_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static auth_user_t g_users[HTTP_AUTH_MAX_USERS];
static size_t g_user_count = 0U;
static auth_session_t g_sessions[HTTP_AUTH_MAX_SESSIONS];
static char g_folders[HTTP_AUTH_MAX_FOLDERS][FTP_PATH_MAX];
static size_t g_folder_count = 0U;
static int g_enabled = 0;
static long g_session_ttl = HTTP_AUTH_DEFAULT_SESSION_TTL;
static const char *g_state_dir_override = NULL;

/* Directories probed for the stores, in order (same as the transfer state). */
static const char *const k_state_dirs[] = {"/data/zftpd", "/tmp/zftpd", NULL};

/*===========================================================================*
 * Small helpers
 *===========================================================================*/

static int random_bytes(uint8_t *out, size_t len) {
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) return -1;
  size_t got = 0U;
  while (got < len) {
    ssize_t n = read(fd, out + got, len - got);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      close(fd);
      return -1;
    }
    got += (size_t)n;
  }
  close(fd);
  return 0;
}

static void hex_encode(const uint8_t *in, size_t len, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0U; i < len; i++) {
    out[i * 2U] = digits[in[i] >> 4];
    out[i * 2U + 1U] = digits[in[i] & 0x0fU];
  }
  out[len * 2U] = '\0';
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int hex_decode(const char *in, uint8_t *out, size_t len) {
  if (strlen(in) != len * 2U) return -1;
  for (size_t i = 0U; i < len; i++) {
    int hi = hex_value(in[i * 2U]);
    int lo = hex_value(in[i * 2U + 1U]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 0;
}

static void set_error(char *err, size_t err_size, const char *message) {
  if (err == NULL || err_size == 0U) return;
  (void)snprintf(err, err_size, "%s", message);
}

static int env_is_true(const char *value) {
  return value != NULL &&
         (strcmp(value, "1") == 0 || strcasecmp(value, "on") == 0 ||
          strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0);
}

static int env_is_false(const char *value) {
  return value != NULL &&
         (strcmp(value, "0") == 0 || strcasecmp(value, "off") == 0 ||
          strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0);
}

const char *http_auth_role_name(http_role_t role) {
  switch (role) {
  case HTTP_ROLE_ADMIN: return "admin";
  case HTTP_ROLE_USER: return "user";
  default: return "none";
  }
}

http_role_t http_auth_role_parse(const char *name) {
  if (name == NULL) return HTTP_ROLE_NONE;
  if (strcasecmp(name, "admin") == 0) return HTTP_ROLE_ADMIN;
  if (strcasecmp(name, "user") == 0 || strcasecmp(name, "normal") == 0)
    return HTTP_ROLE_USER;
  return HTTP_ROLE_NONE;
}

int http_auth_login_valid(const char *login) {
  if (login == NULL) return 0;
  size_t len = strlen(login);
  if (len == 0U || len > HTTP_AUTH_LOGIN_MAX) return 0;
  for (const char *p = login; *p != '\0'; p++) {
    unsigned char c = (unsigned char)*p;
    if (!(isalnum(c) || c == '.' || c == '_' || c == '-' || c == '@')) return 0;
  }
  return 1;
}

static int password_valid(const char *password, char *err, size_t err_size) {
  size_t len = password != NULL ? strlen(password) : 0U;
  if (len < HTTP_AUTH_PASSWORD_MIN) {
    set_error(err, err_size, "Password is too short (6 characters minimum)");
    return 0;
  }
  if (len > HTTP_AUTH_PASSWORD_MAX) {
    set_error(err, err_size, "Password is too long");
    return 0;
  }
  return 1;
}

/*===========================================================================*
 * State directory
 *===========================================================================*/

void http_auth_set_state_dir(const char *dir) { g_state_dir_override = dir; }

int http_auth_state_path(const char *file, char *out, size_t out_size) {
  if (file == NULL || out == NULL || out_size < 2U) return -1;

  const char *dir = g_state_dir_override;
  if (dir == NULL) dir = getenv("ZFTPD_STATE_DIR");
  if (dir != NULL && dir[0] != '\0') {
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return -1;
    int n = snprintf(out, out_size, "%s/%s", dir, file);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
  }

  for (size_t i = 0U; k_state_dirs[i] != NULL; i++) {
    if (mkdir(k_state_dirs[i], 0755) != 0 && errno != EEXIST) continue;
    if (access(k_state_dirs[i], W_OK) != 0) continue;
    int n = snprintf(out, out_size, "%s/%s", k_state_dirs[i], file);
    if (n > 0 && (size_t)n < out_size) return 0;
  }
  return -1;
}

/* Writes the file through a temporary sibling so a crash never leaves a
 * half-written store behind. */
static FILE *store_open_temp(const char *path, char *temp, size_t temp_size) {
  int n = snprintf(temp, temp_size, "%s.tmp", path);
  if (n <= 0 || (size_t)n >= temp_size) return NULL;
  int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return NULL;
  (void)fchmod(fd, 0600);
  FILE *fp = fdopen(fd, "w");
  if (fp == NULL) {
    close(fd);
    (void)unlink(temp);
  }
  return fp;
}

static int store_commit(FILE *fp, const char *temp, const char *path) {
  int rc = fflush(fp);
  if (rc == 0) rc = fsync(fileno(fp));
  if (fclose(fp) != 0) rc = -1;
  if (rc == 0) rc = rename(temp, path);
  if (rc != 0) (void)unlink(temp);
  return rc;
}

/*===========================================================================*
 * Users: persistence
 *===========================================================================*/

/* Caller holds g_lock. */
static int users_save_locked(void) {
  char path[FTP_PATH_MAX];
  char temp[FTP_PATH_MAX];
  if (http_auth_state_path(AUTH_USERS_FILE, path, sizeof(path)) != 0) return -1;
  FILE *fp = store_open_temp(path, temp, sizeof(temp));
  if (fp == NULL) return -1;

  fputs(AUTH_USERS_MAGIC "\n", fp);
  for (size_t i = 0U; i < g_user_count; i++) {
    char salt_hex[AUTH_SALT_BYTES * 2U + 1U];
    char hash_hex[HTTP_SHA256_DIGEST_SIZE * 2U + 1U];
    hex_encode(g_users[i].salt, sizeof(g_users[i].salt), salt_hex);
    hex_encode(g_users[i].hash, sizeof(g_users[i].hash), hash_hex);
    (void)fprintf(fp, "%s\t%s\t%u\t%s\t%s\n", g_users[i].login,
                  http_auth_role_name(g_users[i].role),
                  (unsigned)g_users[i].iterations, salt_hex, hash_hex);
  }
  return store_commit(fp, temp, path);
}

static int split_tabs(char *line, char **out, size_t count) {
  size_t field = 0U;
  char *p = line;
  while (field < count) {
    out[field++] = p;
    char *tab = strchr(p, '\t');
    if (tab == NULL) break;
    *tab = '\0';
    p = tab + 1;
  }
  return field == count ? 0 : -1;
}

/* Caller holds g_lock. */
static void users_load_locked(void) {
  g_user_count = 0U;
  char path[FTP_PATH_MAX];
  if (http_auth_state_path(AUTH_USERS_FILE, path, sizeof(path)) != 0) return;
  FILE *fp = fopen(path, "r");
  if (fp == NULL) return;

  char line[1024];
  if (fgets(line, sizeof(line), fp) == NULL) {
    fclose(fp);
    return;
  }
  line[strcspn(line, "\r\n")] = '\0';
  if (strcmp(line, AUTH_USERS_MAGIC) != 0) {
    ftp_log_line(FTP_LOG_WARN, "zhttp auth: unrecognised user store, ignored");
    fclose(fp);
    return;
  }

  while (g_user_count < HTTP_AUTH_MAX_USERS &&
         fgets(line, sizeof(line), fp) != NULL) {
    line[strcspn(line, "\r\n")] = '\0';
    char *fields[5];
    if (split_tabs(line, fields, 5U) != 0) continue;
    auth_user_t *user = &g_users[g_user_count];
    memset(user, 0, sizeof(*user));
    if (!http_auth_login_valid(fields[0])) continue;
    http_role_t role = http_auth_role_parse(fields[1]);
    if (role == HTTP_ROLE_NONE) continue;
    char *end = NULL;
    unsigned long iterations = strtoul(fields[2], &end, 10);
    if (end == fields[2] || *end != '\0' || iterations == 0UL ||
        iterations > 10000000UL)
      continue;
    if (hex_decode(fields[3], user->salt, sizeof(user->salt)) != 0) continue;
    if (hex_decode(fields[4], user->hash, sizeof(user->hash)) != 0) continue;
    (void)snprintf(user->login, sizeof(user->login), "%s", fields[0]);
    user->role = role;
    user->iterations = (uint32_t)iterations;
    g_user_count++;
  }
  fclose(fp);
}

/*===========================================================================*
 * Users: table
 *===========================================================================*/

/* Caller holds g_lock. */
static auth_user_t *user_lookup_locked(const char *login) {
  if (login == NULL) return NULL;
  for (size_t i = 0U; i < g_user_count; i++) {
    if (strcmp(g_users[i].login, login) == 0) return &g_users[i];
  }
  return NULL;
}

static size_t admin_count_locked(void) {
  size_t n = 0U;
  for (size_t i = 0U; i < g_user_count; i++) {
    if (g_users[i].role == HTTP_ROLE_ADMIN) n++;
  }
  return n;
}

static int user_set_password(auth_user_t *user, const char *password) {
  if (random_bytes(user->salt, sizeof(user->salt)) != 0) return -1;
  user->iterations = AUTH_PBKDF2_ITERATIONS;
  http_pbkdf2_sha256(password, strlen(password), user->salt,
                     sizeof(user->salt), user->iterations, user->hash);
  return 0;
}

static int user_check_password(const auth_user_t *user, const char *password) {
  uint8_t hash[HTTP_SHA256_DIGEST_SIZE];
  http_pbkdf2_sha256(password, strlen(password), user->salt,
                     sizeof(user->salt), user->iterations, hash);
  return http_crypto_equal(hash, user->hash, sizeof(hash));
}

size_t http_auth_user_count(void) {
  pthread_mutex_lock(&g_lock);
  size_t n = g_user_count;
  pthread_mutex_unlock(&g_lock);
  return n;
}

int http_auth_user_get(size_t index, http_auth_user_t *out) {
  if (out == NULL) return -1;
  pthread_mutex_lock(&g_lock);
  int rc = -1;
  if (index < g_user_count) {
    (void)snprintf(out->login, sizeof(out->login), "%s", g_users[index].login);
    out->role = g_users[index].role;
    rc = 0;
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

int http_auth_user_find(const char *login, http_auth_user_t *out) {
  pthread_mutex_lock(&g_lock);
  const auth_user_t *user = user_lookup_locked(login);
  if (user != NULL && out != NULL) {
    (void)snprintf(out->login, sizeof(out->login), "%s", user->login);
    out->role = user->role;
  }
  pthread_mutex_unlock(&g_lock);
  return user != NULL ? 0 : -1;
}

int http_auth_user_add(const char *login, const char *password,
                       http_role_t role, char *err, size_t err_size) {
  if (!http_auth_login_valid(login)) {
    set_error(err, err_size,
              "Login must be 1-32 characters: letters, digits, . _ - @");
    return -1;
  }
  if (role != HTTP_ROLE_ADMIN && role != HTTP_ROLE_USER) {
    set_error(err, err_size, "Role must be admin or user");
    return -1;
  }
  if (!password_valid(password, err, err_size)) return -1;

  pthread_mutex_lock(&g_lock);
  int rc = -1;
  if (user_lookup_locked(login) != NULL) {
    set_error(err, err_size, "A user with this login already exists");
  } else if (g_user_count >= HTTP_AUTH_MAX_USERS) {
    set_error(err, err_size, "Too many users");
  } else {
    auth_user_t *user = &g_users[g_user_count];
    memset(user, 0, sizeof(*user));
    (void)snprintf(user->login, sizeof(user->login), "%s", login);
    user->role = role;
    if (user_set_password(user, password) != 0) {
      set_error(err, err_size, "No entropy available");
    } else {
      g_user_count++;
      rc = users_save_locked();
      if (rc != 0) {
        g_user_count--;
        set_error(err, err_size, "Could not write the user store");
      }
    }
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

int http_auth_user_update(const char *login, const char *password,
                          http_role_t role, char *err, size_t err_size) {
  if (password != NULL && !password_valid(password, err, err_size)) return -1;
  if (role != HTTP_ROLE_NONE && role != HTTP_ROLE_ADMIN &&
      role != HTTP_ROLE_USER) {
    set_error(err, err_size, "Role must be admin or user");
    return -1;
  }

  pthread_mutex_lock(&g_lock);
  int rc = -1;
  auth_user_t *user = user_lookup_locked(login);
  if (user == NULL) {
    set_error(err, err_size, "User not found");
  } else if (role == HTTP_ROLE_USER && user->role == HTTP_ROLE_ADMIN &&
             admin_count_locked() == 1U) {
    set_error(err, err_size, "The last administrator cannot be demoted");
  } else {
    auth_user_t backup = *user;
    int ok = 1;
    if (password != NULL && user_set_password(user, password) != 0) {
      set_error(err, err_size, "No entropy available");
      ok = 0;
    }
    if (ok) {
      if (role != HTTP_ROLE_NONE) user->role = role;
      rc = users_save_locked();
      if (rc != 0) set_error(err, err_size, "Could not write the user store");
    }
    if (rc != 0) *user = backup;
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

int http_auth_user_delete(const char *login, char *err, size_t err_size) {
  pthread_mutex_lock(&g_lock);
  int rc = -1;
  auth_user_t *user = user_lookup_locked(login);
  if (user == NULL) {
    set_error(err, err_size, "User not found");
  } else if (user->role == HTTP_ROLE_ADMIN && admin_count_locked() == 1U) {
    set_error(err, err_size, "The last administrator cannot be removed");
  } else {
    auth_user_t removed = *user;
    size_t index = (size_t)(user - g_users);
    memmove(&g_users[index], &g_users[index + 1U],
            (g_user_count - index - 1U) * sizeof(g_users[0]));
    g_user_count--;
    rc = users_save_locked();
    if (rc != 0) {
      memmove(&g_users[index + 1U], &g_users[index],
              (g_user_count - index) * sizeof(g_users[0]));
      g_users[index] = removed;
      g_user_count++;
      set_error(err, err_size, "Could not write the user store");
    }
  }
  pthread_mutex_unlock(&g_lock);
  if (rc == 0) http_auth_sessions_drop_user(login);
  return rc;
}

int http_auth_verify(const char *login, const char *password,
                     http_role_t *role) {
  if (login == NULL || password == NULL) return -1;
  pthread_mutex_lock(&g_lock);
  const auth_user_t *user = user_lookup_locked(login);
  auth_user_t copy;
  if (user != NULL) {
    copy = *user;
  } else {
    /* Unknown login: still run the derivation so timing says nothing. */
    memset(&copy, 0, sizeof(copy));
    copy.iterations = AUTH_PBKDF2_ITERATIONS;
  }
  pthread_mutex_unlock(&g_lock);

  int ok = user_check_password(&copy, password);
  if (user == NULL || !ok) return -1;
  if (role != NULL) *role = copy.role;
  return 0;
}

int http_auth_configure_admin(const char *login, const char *password) {
  if (!http_auth_login_valid(login)) return -1;
  char err[128];
  if (!password_valid(password, err, sizeof(err))) {
    ftp_log_line(FTP_LOG_ERROR, "zhttp auth: admin password rejected");
    return -1;
  }

  pthread_mutex_lock(&g_lock);
  auth_user_t *user = user_lookup_locked(login);
  int unchanged = user != NULL && user->role == HTTP_ROLE_ADMIN &&
                  user_check_password(user, password);
  pthread_mutex_unlock(&g_lock);
  if (unchanged) return 0;

  if (user == NULL)
    return http_auth_user_add(login, password, HTTP_ROLE_ADMIN, err,
                              sizeof(err));
  return http_auth_user_update(login, password, HTTP_ROLE_ADMIN, err,
                               sizeof(err));
}

/*===========================================================================*
 * Sessions
 *===========================================================================*/

/* Caller holds g_lock. */
static void sessions_expire_locked(time_t now) {
  for (size_t i = 0U; i < HTTP_AUTH_MAX_SESSIONS; i++) {
    if (g_sessions[i].token[0] != '\0' && g_sessions[i].expires <= now)
      memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
  }
}

int http_auth_session_create(const char *login, char *token,
                             size_t token_size) {
  if (login == NULL || token == NULL || token_size <= HTTP_AUTH_TOKEN_HEX)
    return -1;
  uint8_t raw[HTTP_AUTH_TOKEN_HEX / 2U];
  if (random_bytes(raw, sizeof(raw)) != 0) return -1;

  time_t now = time(NULL);
  pthread_mutex_lock(&g_lock);
  sessions_expire_locked(now);
  auth_session_t *slot = NULL;
  auth_session_t *oldest = &g_sessions[0];
  for (size_t i = 0U; i < HTTP_AUTH_MAX_SESSIONS; i++) {
    if (g_sessions[i].token[0] == '\0') {
      slot = &g_sessions[i];
      break;
    }
    if (g_sessions[i].expires < oldest->expires) oldest = &g_sessions[i];
  }
  if (slot == NULL) slot = oldest; /* table full: evict the oldest */
  hex_encode(raw, sizeof(raw), slot->token);
  (void)snprintf(slot->login, sizeof(slot->login), "%s", login);
  slot->expires = now + (time_t)g_session_ttl;
  (void)snprintf(token, token_size, "%s", slot->token);
  pthread_mutex_unlock(&g_lock);
  return 0;
}

void http_auth_session_destroy(const char *token) {
  if (token == NULL) return;
  pthread_mutex_lock(&g_lock);
  for (size_t i = 0U; i < HTTP_AUTH_MAX_SESSIONS; i++) {
    if (g_sessions[i].token[0] != '\0' &&
        strcmp(g_sessions[i].token, token) == 0)
      memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
  }
  pthread_mutex_unlock(&g_lock);
}

void http_auth_sessions_drop_user(const char *login) {
  if (login == NULL) return;
  pthread_mutex_lock(&g_lock);
  for (size_t i = 0U; i < HTTP_AUTH_MAX_SESSIONS; i++) {
    if (g_sessions[i].token[0] != '\0' &&
        strcmp(g_sessions[i].login, login) == 0)
      memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
  }
  pthread_mutex_unlock(&g_lock);
}

size_t http_auth_session_count(void) {
  pthread_mutex_lock(&g_lock);
  sessions_expire_locked(time(NULL));
  size_t n = 0U;
  for (size_t i = 0U; i < HTTP_AUTH_MAX_SESSIONS; i++) {
    if (g_sessions[i].token[0] != '\0') n++;
  }
  pthread_mutex_unlock(&g_lock);
  return n;
}

static int token_valid(const char *token, size_t len) {
  if (len != HTTP_AUTH_TOKEN_HEX) return 0;
  for (size_t i = 0U; i < len; i++) {
    if (hex_value(token[i]) < 0) return 0;
  }
  return 1;
}

int http_auth_request_token(const http_request_t *request, char *out,
                            size_t out_size) {
  if (request == NULL || out == NULL || out_size <= HTTP_AUTH_TOKEN_HEX)
    return -1;
  out[0] = '\0';

  const char *bearer = http_get_header(request, "Authorization");
  if (bearer != NULL && strncasecmp(bearer, "Bearer ", 7) == 0) {
    const char *value = bearer + 7;
    while (*value == ' ') value++;
    size_t len = strcspn(value, " \t");
    if (token_valid(value, len)) {
      memcpy(out, value, len);
      out[len] = '\0';
      return 0;
    }
  }

  const char *cookie = http_get_header(request, "Cookie");
  if (cookie == NULL) return -1;
  static const char name[] = HTTP_AUTH_COOKIE_NAME "=";
  const char *p = cookie;
  while ((p = strstr(p, name)) != NULL) {
    int at_start = p == cookie || p[-1] == ' ' || p[-1] == ';';
    const char *value = p + sizeof(name) - 1U;
    p = value;
    if (!at_start) continue;
    size_t len = strcspn(value, ";");
    while (len > 0U && (value[len - 1U] == ' ' || value[len - 1U] == '\t')) len--;
    if (token_valid(value, len)) {
      memcpy(out, value, len);
      out[len] = '\0';
      return 0;
    }
  }
  return -1;
}

void http_auth_identify(const http_request_t *request,
                        http_auth_identity_t *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  if (!http_auth_enabled()) {
    out->role = HTTP_ROLE_ADMIN;
    return;
  }

  char token[HTTP_AUTH_TOKEN_HEX + 1U];
  if (http_auth_request_token(request, token, sizeof(token)) != 0) return;

  time_t now = time(NULL);
  pthread_mutex_lock(&g_lock);
  sessions_expire_locked(now);
  for (size_t i = 0U; i < HTTP_AUTH_MAX_SESSIONS; i++) {
    if (g_sessions[i].token[0] == '\0' ||
        strcmp(g_sessions[i].token, token) != 0)
      continue;
    const auth_user_t *user = user_lookup_locked(g_sessions[i].login);
    if (user == NULL) {
      memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
      break;
    }
    out->authenticated = 1;
    out->role = user->role;
    (void)snprintf(out->login, sizeof(out->login), "%s", user->login);
    break;
  }
  pthread_mutex_unlock(&g_lock);
}

int http_auth_cookie_value(const char *token, long ttl, char *out,
                           size_t out_size) {
  int n;
  if (ttl > 0L && token != NULL) {
    n = snprintf(out, out_size,
                 HTTP_AUTH_COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Lax; "
                 "Max-Age=%ld",
                 token, ttl);
  } else {
    n = snprintf(out, out_size,
                 HTTP_AUTH_COOKIE_NAME "=; Path=/; HttpOnly; SameSite=Lax; "
                 "Max-Age=0");
  }
  return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

/*===========================================================================*
 * Folder allow-list
 *===========================================================================*/

/* Caller holds g_lock. */
static int folders_save_locked(void) {
  char path[FTP_PATH_MAX];
  char temp[FTP_PATH_MAX];
  if (http_auth_state_path(AUTH_ACCESS_FILE, path, sizeof(path)) != 0) return -1;
  FILE *fp = store_open_temp(path, temp, sizeof(temp));
  if (fp == NULL) return -1;
  fputs(AUTH_ACCESS_MAGIC "\n", fp);
  for (size_t i = 0U; i < g_folder_count; i++) {
    fputs(g_folders[i], fp);
    fputc('\n', fp);
  }
  return store_commit(fp, temp, path);
}

/* Caller holds g_lock. */
static void folders_load_locked(void) {
  g_folder_count = 0U;
  char path[FTP_PATH_MAX];
  if (http_auth_state_path(AUTH_ACCESS_FILE, path, sizeof(path)) != 0) return;
  FILE *fp = fopen(path, "r");
  if (fp == NULL) return;
  char line[FTP_PATH_MAX + 16U];
  if (fgets(line, sizeof(line), fp) == NULL) {
    fclose(fp);
    return;
  }
  line[strcspn(line, "\r\n")] = '\0';
  if (strcmp(line, AUTH_ACCESS_MAGIC) != 0) {
    fclose(fp);
    return;
  }
  while (g_folder_count < HTTP_AUTH_MAX_FOLDERS &&
         fgets(line, sizeof(line), fp) != NULL) {
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] != '/' || strlen(line) >= FTP_PATH_MAX) continue;
    (void)snprintf(g_folders[g_folder_count], FTP_PATH_MAX, "%s", line);
    g_folder_count++;
  }
  fclose(fp);
}

size_t http_auth_folder_count(void) {
  pthread_mutex_lock(&g_lock);
  size_t n = g_folder_count;
  pthread_mutex_unlock(&g_lock);
  return n;
}

const char *http_auth_folder_get(size_t index) {
  /* Entries are only replaced wholesale under the lock; readers copy the
   * pointer target before the next replacement in practice (single UI). */
  return index < g_folder_count ? g_folders[index] : NULL;
}

static int path_has_control(const char *path) {
  for (const char *p = path; *p != '\0'; p++) {
    if ((unsigned char)*p < 0x20U) return 1;
  }
  return 0;
}

int http_auth_folders_set(const char *const *folders, size_t count, char *err,
                          size_t err_size) {
  if (count > HTTP_AUTH_MAX_FOLDERS) {
    set_error(err, err_size, "Too many folders (32 maximum)");
    return -1;
  }
  char canonical[HTTP_AUTH_MAX_FOLDERS][FTP_PATH_MAX];
  size_t kept = 0U;
  for (size_t i = 0U; i < count; i++) {
    const char *raw = folders[i];
    if (raw == NULL || raw[0] != '/' || path_has_control(raw)) {
      set_error(err, err_size, "Folders must be absolute paths");
      return -1;
    }
    char safe[FTP_PATH_MAX];
    if (!http_api_validate_path(raw, safe, sizeof(safe))) {
      set_error(err, err_size, "Folder is outside the served root");
      return -1;
    }
    struct stat st;
    if (stat(safe, &st) != 0 || !S_ISDIR(st.st_mode)) {
      set_error(err, err_size, "Folder not found");
      return -1;
    }
    int duplicate = 0;
    for (size_t j = 0U; j < kept; j++) {
      if (strcmp(canonical[j], safe) == 0) duplicate = 1;
    }
    if (duplicate) continue;
    (void)snprintf(canonical[kept], FTP_PATH_MAX, "%s", safe);
    kept++;
  }

  pthread_mutex_lock(&g_lock);
  char backup[HTTP_AUTH_MAX_FOLDERS][FTP_PATH_MAX];
  size_t backup_count = g_folder_count;
  memcpy(backup, g_folders, sizeof(backup));
  memcpy(g_folders, canonical, sizeof(g_folders));
  g_folder_count = kept;
  int rc = folders_save_locked();
  if (rc != 0) {
    memcpy(g_folders, backup, sizeof(g_folders));
    g_folder_count = backup_count;
    set_error(err, err_size, "Could not write the access rules");
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

static int path_within(const char *path, const char *folder) {
  if (strcmp(folder, "/") == 0) return 1;
  size_t n = strlen(folder);
  return strncmp(path, folder, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

int http_auth_path_allowed(const http_auth_identity_t *identity,
                           const char *path) {
  if (!http_auth_enabled()) return 1;
  if (identity == NULL || !identity->authenticated) return 0;
  if (identity->role == HTTP_ROLE_ADMIN) return 1;
  if (path == NULL) return 0;

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) return 0;

  int allowed = 0;
  pthread_mutex_lock(&g_lock);
  for (size_t i = 0U; i < g_folder_count && !allowed; i++) {
    if (path_within(safe, g_folders[i])) allowed = 1;
  }
  pthread_mutex_unlock(&g_lock);
  return allowed;
}

/*===========================================================================*
 * Request gate
 *===========================================================================*/

/* Routes that never need a login. */
static const char *const k_public_routes[] = {
    "/api/status", "/api/auth/login", "/api/auth/logout", "/api/auth/me",
    NULL};

/* Routes a "user" may call; every path they carry is checked against the
 * allow-list.  Everything else under /api/ needs an administrator. */
static const char *const k_user_routes[] = {
    "/api/list",           "/api/dirsize",      "/api/file/get",
    "/api/download",       "/api/create_file",  "/api/mkdir",
    "/api/delete",         "/api/rename",       "/api/copy",
    "/api/copy_progress",  "/api/copy_cancel",  "/api/copy_pause",
    "/api/upload",         "/api/extract",      "/api/extract_progress",
    "/api/extract_cancel", "/api/archive/zip",  "/api/stats",
    "/api/stats/ram",      "/api/stats/system", "/api/disk/info",
    "/api/mounts",         "/api/auth/password", "/api/game/meta",
    "/api/game/icon",      NULL};

/* Listing routes default to "/" when no path is given. */
static const char *const k_default_root_routes[] = {"/api/list", "/api/dirsize",
                                                    "/api/stats", NULL};

static int route_in(const char *uri, const char *const *routes) {
  for (size_t i = 0U; routes[i] != NULL; i++) {
    if (http_api_route_is(uri, routes[i])) return 1;
  }
  return 0;
}

static http_response_t *gate_error(http_status_t status, const char *message,
                                   const char *hint) {
  http_response_t *resp = http_response_create(status);
  if (resp == NULL) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  char body[256];
  int n = snprintf(body, sizeof(body), "{\"error\":\"%s\",\"auth\":\"%s\"}",
                   message, hint);
  if (n < 0 || (size_t)n >= sizeof(body)) n = 0;
  http_response_set_body(resp, body, (size_t)n);
  return resp;
}

/* Every path-like parameter of a user-role request must be allowed. */
static int request_paths_allowed(const http_request_t *request,
                                 const http_auth_identity_t *identity) {
  const char *query = strchr(request->uri, '?');
  char value[FTP_PATH_MAX];
  static const char *const query_keys[] = {"path", "dst", NULL};
  int saw_path = 0;

  for (size_t i = 0U; query_keys[i] != NULL; i++) {
    if (query == NULL || !http_api_query_has_param(query, query_keys[i])) continue;
    if (strcmp(query_keys[i], "path") == 0) saw_path = 1;
    if (http_api_parse_query_param(query, query_keys[i], value,
                                   sizeof(value)) != 0) {
      /* An empty "path" means the root. */
      if (strcmp(query_keys[i], "path") != 0) return 0;
      (void)snprintf(value, sizeof(value), "/");
    }
    if (!http_auth_path_allowed(identity, value)) return 0;
  }

  if (!saw_path && route_in(request->uri, k_default_root_routes) &&
      !http_auth_path_allowed(identity, "/"))
    return 0;

  if (request->body != NULL && request->body_length > 0U) {
    static const char *const body_keys[] = {"path", "dst", "dest", NULL};
    for (size_t i = 0U; body_keys[i] != NULL; i++) {
      if (http_json_body_string(request->body, body_keys[i], value,
                                sizeof(value)) &&
          !http_auth_path_allowed(identity, value))
        return 0;
    }
    const char *cursor = NULL;
    while (http_json_body_array_next(request->body, "paths", &cursor, value,
                                     sizeof(value))) {
      if (!http_auth_path_allowed(identity, value)) return 0;
    }
  }
  return 1;
}

http_response_t *http_auth_gate(http_request_t *request,
                                http_auth_identity_t *identity) {
  http_auth_identity_t local;
  if (identity == NULL) identity = &local;
  http_auth_identify(request, identity);
  request->auth_role = (int)identity->role;
  (void)snprintf(request->auth_login, sizeof(request->auth_login), "%s",
                 identity->login);

  if (!http_auth_enabled()) return NULL;
  if (strncmp(request->uri, "/api/", 5) != 0) return NULL; /* UI, shares */
  if (route_in(request->uri, k_public_routes)) return NULL;

  if (!identity->authenticated)
    return gate_error(HTTP_STATUS_401_UNAUTHORIZED, "Login required",
                      "required");
  if (identity->role == HTTP_ROLE_ADMIN) return NULL;

  if (!route_in(request->uri, k_user_routes))
    return gate_error(HTTP_STATUS_403_FORBIDDEN,
                      "Administrator access required", "admin");
  if (!request_paths_allowed(request, identity))
    return gate_error(HTTP_STATUS_403_FORBIDDEN,
                      "This folder is not available to your account",
                      "folder");
  return NULL;
}

int http_auth_status_json(const http_request_t *request, char *buf,
                          size_t cap, size_t *pos) {
  http_auth_identity_t identity;
  http_auth_identify(request, &identity);
  int enabled = http_auth_enabled();
  if (http_buf_append_cstr(buf, cap, pos, "\"auth\":{\"enabled\":") != 0 ||
      http_buf_append_cstr(buf, cap, pos, enabled ? "true" : "false") != 0 ||
      http_buf_append_cstr(buf, cap, pos, ",\"authenticated\":") != 0 ||
      http_buf_append_cstr(buf, cap, pos,
                           identity.authenticated ? "true" : "false") != 0 ||
      http_buf_append_cstr(buf, cap, pos, ",\"user\":\"") != 0 ||
      http_json_escape_append(buf, cap, pos, identity.login) != 0 ||
      http_buf_append_cstr(buf, cap, pos, "\",\"role\":\"") != 0 ||
      http_buf_append_cstr(buf, cap, pos,
                           enabled ? http_auth_role_name(identity.role)
                                   : "admin") != 0 ||
      http_buf_append_cstr(buf, cap, pos, "\"}") != 0)
    return -1;
  return 0;
}

/*===========================================================================*
 * Lifecycle
 *===========================================================================*/

int http_auth_enabled(void) {
  pthread_mutex_lock(&g_lock);
  int enabled = g_enabled;
  pthread_mutex_unlock(&g_lock);
  return enabled;
}

void http_auth_set_enabled(int enabled) {
  pthread_mutex_lock(&g_lock);
  g_enabled = enabled ? 1 : 0;
  pthread_mutex_unlock(&g_lock);
}

long http_auth_session_ttl(void) { return g_session_ttl; }

void http_auth_set_session_ttl(long seconds) {
  if (seconds < 60L) seconds = 60L;
  g_session_ttl = seconds;
}

int http_auth_init(void) {
  pthread_mutex_lock(&g_lock);
  memset(g_sessions, 0, sizeof(g_sessions));
  users_load_locked();
  folders_load_locked();
  g_enabled = 0;
  pthread_mutex_unlock(&g_lock);

  const char *ttl = getenv("ZFTPD_HTTP_SESSION_TTL");
  if (ttl != NULL && ttl[0] != '\0') {
    char *end = NULL;
    long seconds = strtol(ttl, &end, 10);
    if (end != ttl && *end == '\0' && seconds > 0L)
      http_auth_set_session_ttl(seconds);
  }

  const char *admin_login = getenv("ZFTPD_ADMIN_USER");
  if (admin_login == NULL || admin_login[0] == '\0') admin_login = "admin";
  const char *admin_password = getenv("ZFTPD_ADMIN_PASSWORD");
  int configured = 0;
  if (admin_password != NULL && admin_password[0] != '\0') {
    if (http_auth_configure_admin(admin_login, admin_password) == 0) {
      configured = 1;
    } else {
      ftp_log_line(FTP_LOG_ERROR,
                   "zhttp auth: could not store the admin account");
    }
  }

  const char *mode = getenv("ZFTPD_HTTP_AUTH");
  int have_admin = 0;
  pthread_mutex_lock(&g_lock);
  have_admin = admin_count_locked() > 0U;
  pthread_mutex_unlock(&g_lock);

  int enabled;
  if (env_is_false(mode)) {
    enabled = 0;
  } else if (env_is_true(mode)) {
    enabled = 1;
    if (!have_admin)
      ftp_log_line(FTP_LOG_WARN,
                   "zhttp auth: enabled without any administrator; set "
                   "ZFTPD_ADMIN_PASSWORD to create one");
  } else {
    enabled = configured || have_admin;
  }
  http_auth_set_enabled(enabled);

  char line[160];
  (void)snprintf(line, sizeof(line),
                 "zhttp auth: %s (%zu users, %zu allowed folders)",
                 enabled ? "login required" : "disabled",
                 http_auth_user_count(), http_auth_folder_count());
  ftp_log_line(FTP_LOG_INFO, line);
  return 0;
}

void http_auth_shutdown(void) {
  pthread_mutex_lock(&g_lock);
  memset(g_sessions, 0, sizeof(g_sessions));
  g_user_count = 0U;
  g_folder_count = 0U;
  g_enabled = 0;
  pthread_mutex_unlock(&g_lock);
}
