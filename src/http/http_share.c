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
 * @file http_share.c
 * @brief Share store and the public /s/<id> handler.
 *
 * Store: <state>/zhttp_shares.db
 *   zftpd-shares 1
 *   id \t D|F \t created \t expires \t owner \t path
 */

#include "http_share.h"
#include "ftp_path.h"
#include "http_api.h"
#include "http_api_internal.h"
#include "http_auth.h"
#include "http_json.h"
#include "zip_writer.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SHARE_FILE "zhttp_shares.db"
#define SHARE_MAGIC "zftpd-shares 1"
#define SHARE_LIST_MAX_ENTRIES 4000U

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static http_share_t g_shares[HTTP_SHARE_MAX];
static size_t g_count = 0U;

/*===========================================================================*
 * Persistence
 *===========================================================================*/

static void write_escaped(FILE *fp, const char *value) {
  for (const char *p = value; *p != '\0'; p++) {
    switch (*p) {
    case '\\': fputs("\\\\", fp); break;
    case '\t': fputs("\\t", fp); break;
    case '\n': fputs("\\n", fp); break;
    case '\r': fputs("\\r", fp); break;
    default: fputc(*p, fp); break;
    }
  }
}

static void read_escaped(char *dst, size_t dst_size, const char *src) {
  size_t pos = 0U;
  if (dst_size == 0U) return;
  while (*src != '\0' && pos + 1U < dst_size) {
    if (*src != '\\') {
      dst[pos++] = *src++;
      continue;
    }
    src++;
    switch (*src) {
    case '\\': dst[pos++] = '\\'; break;
    case 't': dst[pos++] = '\t'; break;
    case 'n': dst[pos++] = '\n'; break;
    case 'r': dst[pos++] = '\r'; break;
    case '\0': dst[pos] = '\0'; return;
    default: dst[pos++] = *src; break;
    }
    src++;
  }
  dst[pos] = '\0';
}

/* Caller holds g_lock. */
static int shares_save_locked(void) {
  char path[FTP_PATH_MAX];
  char temp[FTP_PATH_MAX];
  if (http_auth_state_path(SHARE_FILE, path, sizeof(path)) != 0) return -1;
  int n = snprintf(temp, sizeof(temp), "%s.tmp", path);
  if (n <= 0 || (size_t)n >= sizeof(temp)) return -1;
  int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  FILE *fp = fdopen(fd, "w");
  if (fp == NULL) {
    close(fd);
    (void)unlink(temp);
    return -1;
  }
  fputs(SHARE_MAGIC "\n", fp);
  for (size_t i = 0U; i < g_count; i++) {
    const http_share_t *s = &g_shares[i];
    (void)fprintf(fp, "%s\t%c\t%lld\t%lld\t%s\t", s->id, s->is_dir ? 'D' : 'F',
                  (long long)s->created, (long long)s->expires, s->owner);
    write_escaped(fp, s->path);
    fputc('\n', fp);
  }
  int rc = fflush(fp);
  if (rc == 0) rc = fsync(fd);
  if (fclose(fp) != 0) rc = -1;
  if (rc == 0) rc = rename(temp, path);
  if (rc != 0) (void)unlink(temp);
  return rc;
}

static int split_tabs(char *line, char **out, size_t count) {
  size_t field = 0U;
  char *p = line;
  while (field < count) {
    out[field++] = p;
    if (field == count) break;
    char *tab = strchr(p, '\t');
    if (tab == NULL) break;
    *tab = '\0';
    p = tab + 1;
  }
  return field == count ? 0 : -1;
}

static int id_valid(const char *id) {
  if (id == NULL || strlen(id) != HTTP_SHARE_ID_HEX) return 0;
  for (const char *p = id; *p != '\0'; p++) {
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 0;
  }
  return 1;
}

/* Caller holds g_lock. */
static void shares_load_locked(void) {
  g_count = 0U;
  char path[FTP_PATH_MAX];
  if (http_auth_state_path(SHARE_FILE, path, sizeof(path)) != 0) return;
  FILE *fp = fopen(path, "r");
  if (fp == NULL) return;
  char line[FTP_PATH_MAX * 2U + 256U];
  if (fgets(line, sizeof(line), fp) == NULL) {
    fclose(fp);
    return;
  }
  line[strcspn(line, "\r\n")] = '\0';
  if (strcmp(line, SHARE_MAGIC) != 0) {
    fclose(fp);
    return;
  }
  while (g_count < HTTP_SHARE_MAX && fgets(line, sizeof(line), fp) != NULL) {
    line[strcspn(line, "\r\n")] = '\0';
    char *fields[6];
    if (split_tabs(line, fields, 6U) != 0) continue;
    if (!id_valid(fields[0])) continue;
    if (fields[1][0] != 'D' && fields[1][0] != 'F') continue;
    char *end = NULL;
    long long created = strtoll(fields[2], &end, 10);
    if (end == fields[2] || *end != '\0') continue;
    long long expires = strtoll(fields[3], &end, 10);
    if (end == fields[3] || *end != '\0' || expires < 0LL) continue;
    http_share_t *s = &g_shares[g_count];
    memset(s, 0, sizeof(*s));
    (void)snprintf(s->id, sizeof(s->id), "%s", fields[0]);
    s->is_dir = fields[1][0] == 'D';
    s->created = (time_t)created;
    s->expires = (time_t)expires;
    (void)snprintf(s->owner, sizeof(s->owner), "%s", fields[4]);
    read_escaped(s->path, sizeof(s->path), fields[5]);
    if (s->path[0] != '/') continue;
    g_count++;
  }
  fclose(fp);
}

void http_share_init(void) {
  pthread_mutex_lock(&g_lock);
  shares_load_locked();
  pthread_mutex_unlock(&g_lock);
}

void http_share_shutdown(void) {
  pthread_mutex_lock(&g_lock);
  g_count = 0U;
  pthread_mutex_unlock(&g_lock);
}

/*===========================================================================*
 * Store operations
 *===========================================================================*/

static int random_id(char *out, size_t out_size) {
  if (out_size <= HTTP_SHARE_ID_HEX) return -1;
  uint8_t raw[HTTP_SHARE_ID_HEX / 2U];
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) return -1;
  size_t got = 0U;
  while (got < sizeof(raw)) {
    ssize_t n = read(fd, raw + got, sizeof(raw) - got);
    if (n <= 0) {
      close(fd);
      return -1;
    }
    got += (size_t)n;
  }
  close(fd);
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0U; i < sizeof(raw); i++) {
    out[i * 2U] = digits[raw[i] >> 4];
    out[i * 2U + 1U] = digits[raw[i] & 0x0fU];
  }
  out[HTTP_SHARE_ID_HEX] = '\0';
  return 0;
}

static void set_error(char *err, size_t err_size, const char *message) {
  if (err != NULL && err_size > 0U) (void)snprintf(err, err_size, "%s", message);
}

int http_share_is_expired(const http_share_t *share, time_t now) {
  return share != NULL && share->expires != 0 && share->expires <= now;
}

int http_share_create(const char *path, time_t expires, const char *owner,
                      http_share_t *out, char *err, size_t err_size) {
  if (path == NULL || path[0] != '/') {
    set_error(err, err_size, "Missing or invalid path");
    return -1;
  }
  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    set_error(err, err_size, "Path is outside the served root");
    return -1;
  }
  struct stat st;
  if (stat(safe, &st) != 0 || !(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) {
    set_error(err, err_size, "File or folder not found");
    return -1;
  }
  if (expires < 0 || (expires != 0 && expires <= time(NULL))) {
    set_error(err, err_size, "The expiry date is in the past");
    return -1;
  }
  if (strcmp(safe, http_api_get_root()) == 0) {
    set_error(err, err_size, "The served root itself cannot be shared");
    return -1;
  }

  http_share_t share;
  memset(&share, 0, sizeof(share));
  if (random_id(share.id, sizeof(share.id)) != 0) {
    set_error(err, err_size, "No entropy available");
    return -1;
  }
  (void)snprintf(share.path, sizeof(share.path), "%s", safe);
  share.is_dir = S_ISDIR(st.st_mode);
  share.created = time(NULL);
  share.expires = expires;
  (void)snprintf(share.owner, sizeof(share.owner), "%s",
                 owner != NULL ? owner : "");

  pthread_mutex_lock(&g_lock);
  int rc = -1;
  if (g_count >= HTTP_SHARE_MAX) {
    set_error(err, err_size, "Too many shares; remove some first");
  } else {
    g_shares[g_count++] = share;
    rc = shares_save_locked();
    if (rc != 0) {
      g_count--;
      set_error(err, err_size, "Could not write the share store");
    }
  }
  pthread_mutex_unlock(&g_lock);
  if (rc == 0 && out != NULL) *out = share;
  return rc;
}

int http_share_delete(const char *id) {
  if (!id_valid(id)) return -1;
  pthread_mutex_lock(&g_lock);
  int rc = -1;
  for (size_t i = 0U; i < g_count; i++) {
    if (strcmp(g_shares[i].id, id) != 0) continue;
    http_share_t removed = g_shares[i];
    memmove(&g_shares[i], &g_shares[i + 1U],
            (g_count - i - 1U) * sizeof(g_shares[0]));
    g_count--;
    rc = shares_save_locked();
    if (rc != 0) {
      memmove(&g_shares[i + 1U], &g_shares[i],
              (g_count - i) * sizeof(g_shares[0]));
      g_shares[i] = removed;
      g_count++;
    }
    break;
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

size_t http_share_count(void) {
  pthread_mutex_lock(&g_lock);
  size_t n = g_count;
  pthread_mutex_unlock(&g_lock);
  return n;
}

int http_share_get(size_t index, http_share_t *out) {
  if (out == NULL) return -1;
  pthread_mutex_lock(&g_lock);
  int rc = -1;
  if (index < g_count) {
    *out = g_shares[index];
    rc = 0;
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

int http_share_find(const char *id, http_share_t *out) {
  if (!id_valid(id)) return -1;
  pthread_mutex_lock(&g_lock);
  int rc = -1;
  for (size_t i = 0U; i < g_count; i++) {
    if (strcmp(g_shares[i].id, id) == 0) {
      if (out != NULL) *out = g_shares[i];
      rc = 0;
      break;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

size_t http_share_purge_expired(void) {
  time_t now = time(NULL);
  pthread_mutex_lock(&g_lock);
  size_t kept = 0U;
  for (size_t i = 0U; i < g_count; i++) {
    if (!http_share_is_expired(&g_shares[i], now)) g_shares[kept++] = g_shares[i];
  }
  size_t removed = g_count - kept;
  if (removed > 0U) {
    g_count = kept;
    (void)shares_save_locked();
  }
  pthread_mutex_unlock(&g_lock);
  return removed;
}

/*===========================================================================*
 * Public handler
 *===========================================================================*/

int http_share_route_matches(const char *uri) {
  return uri != NULL &&
         strncmp(uri, HTTP_SHARE_URL_PREFIX, sizeof(HTTP_SHARE_URL_PREFIX) - 1U) == 0;
}

static int url_decode(const char *in, size_t len, char *out, size_t out_size) {
  size_t wi = 0U;
  for (size_t ri = 0U; ri < len; ri++) {
    unsigned char ch = (unsigned char)in[ri];
    if (ch == '%') {
      if (ri + 2U >= len) return -1;
      int hi = in[ri + 1U], lo = in[ri + 2U];
      int h = (hi >= '0' && hi <= '9') ? hi - '0' : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
      int l = (lo >= '0' && lo <= '9') ? lo - '0' : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
      if (h < 0 || l < 0) return -1;
      ch = (unsigned char)((h << 4) | l);
      ri += 2U;
    }
    if (ch == '\0' || ch < 0x20U) return -1;
    if (wi + 1U >= out_size) return -1;
    out[wi++] = (char)ch;
  }
  out[wi] = '\0';
  return 0;
}

static http_response_t *html_page(http_status_t status, const char *title,
                                  const char *message) {
  http_strbuf_t b;
  http_strbuf_init(&b);
  http_strbuf_append_cstr(&b,
      "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
      "<title>");
  http_strbuf_append_html(&b, title);
  http_strbuf_append_cstr(&b,
      "</title><style>body{font:16px/1.5 system-ui,sans-serif;margin:0;"
      "padding:48px 24px;background:#f5f6f8;color:#1f2328}main{max-width:520px;"
      "margin:auto;background:#fff;border-radius:12px;padding:32px;"
      "box-shadow:0 1px 3px rgba(0,0,0,.08)}h1{font-size:22px;margin:0 0 8px}"
      "p{margin:0;color:#59636e}</style></head><body><main><h1>");
  http_strbuf_append_html(&b, title);
  http_strbuf_append_cstr(&b, "</h1><p>");
  http_strbuf_append_html(&b, message);
  http_strbuf_append_cstr(&b, "</p></main></body></html>");
  size_t len = 0U;
  char *body = http_strbuf_take(&b, &len);
  if (body == NULL) return NULL;
  http_response_t *resp = http_response_create(status);
  if (resp == NULL) {
    free(body);
    return NULL;
  }
  http_response_add_header(resp, "Content-Type", "text/html; charset=utf-8");
  if (http_response_set_body_owned(resp, body, len) != 0) {
    free(body);
    http_response_destroy(resp);
    return NULL;
  }
  return resp;
}

typedef struct {
  char name[256];
  int is_dir;
  uint64_t size;
  time_t mtime;
} share_entry_t;

static int entry_compare(const void *a, const void *b) {
  const share_entry_t *x = (const share_entry_t *)a;
  const share_entry_t *y = (const share_entry_t *)b;
  if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
  return strcasecmp(x->name, y->name);
}

static void format_time(time_t t, char *out, size_t out_size) {
  struct tm tm;
  if (t <= 0 || gmtime_r(&t, &tm) == NULL) {
    (void)snprintf(out, out_size, "-");
    return;
  }
  if (strftime(out, out_size, "%Y-%m-%d %H:%M UTC", &tm) == 0U)
    (void)snprintf(out, out_size, "-");
}

static void format_size(uint64_t n, char *out, size_t out_size) {
  static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = (double)n;
  size_t u = 0U;
  while (v >= 1024.0 && u + 1U < sizeof(units) / sizeof(units[0])) {
    v /= 1024.0;
    u++;
  }
  if (u == 0U) (void)snprintf(out, out_size, "%" PRIu64 " B", n);
  else (void)snprintf(out, out_size, "%.1f %s", v, units[u]);
}

/* HTML listing of a folder inside the share.  @p rel is the decoded path
 * below the share root ("" for the root itself). */
static http_response_t *share_listing(const http_share_t *share,
                                      const char *dir, const char *rel) {
  DIR *d = opendir(dir);
  if (d == NULL) return html_page(HTTP_STATUS_404_NOT_FOUND, "Not found",
                                  "This folder is no longer available.");

  share_entry_t *entries = calloc(SHARE_LIST_MAX_ENTRIES, sizeof(*entries));
  if (entries == NULL) {
    closedir(d);
    return html_page(HTTP_STATUS_500_INTERNAL_ERROR, "Error", "Out of memory.");
  }
  size_t count = 0U;
  int truncated = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    if (count >= SHARE_LIST_MAX_ENTRIES) {
      truncated = 1;
      break;
    }
    char child[FTP_PATH_MAX];
    int n = snprintf(child, sizeof(child), "%s/%s", dir, ent->d_name);
    if (n <= 0 || (size_t)n >= sizeof(child)) continue;
    struct stat st;
    if (stat(child, &st) != 0) continue;
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;
    share_entry_t *e = &entries[count++];
    (void)snprintf(e->name, sizeof(e->name), "%s", ent->d_name);
    e->is_dir = S_ISDIR(st.st_mode);
    e->size = e->is_dir ? 0U : (uint64_t)st.st_size;
    e->mtime = st.st_mtime;
  }
  closedir(d);
  qsort(entries, count, sizeof(*entries), entry_compare);

  const char *share_name = strrchr(share->path, '/');
  share_name = share_name != NULL && share_name[1] != '\0' ? share_name + 1
                                                            : share->path;

  http_strbuf_t b;
  http_strbuf_init(&b);
  http_strbuf_append_cstr(&b,
      "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
      "<title>");
  http_strbuf_append_html(&b, share_name);
  http_strbuf_append_cstr(&b,
      " - shared folder</title><style>"
      "body{font:15px/1.5 system-ui,sans-serif;margin:0;padding:32px 16px;"
      "background:#f5f6f8;color:#1f2328}main{max-width:960px;margin:auto;"
      "background:#fff;border-radius:12px;padding:24px 28px;"
      "box-shadow:0 1px 3px rgba(0,0,0,.08)}h1{font-size:20px;margin:0 0 4px}"
      ".crumbs{color:#59636e;margin-bottom:16px;word-break:break-all}"
      ".crumbs a{color:#0969da;text-decoration:none}.crumbs a:hover{text-decoration:underline}"
      ".tools{margin:0 0 16px}.tools a{display:inline-block;padding:6px 12px;"
      "border:1px solid #d0d7de;border-radius:8px;color:#1f2328;text-decoration:none}"
      ".tools a:hover{background:#f3f4f6}table{width:100%;border-collapse:collapse}"
      "th,td{text-align:left;padding:8px 10px;border-top:1px solid #eaeef2;"
      "white-space:nowrap}th{font-weight:600;color:#59636e;border-top:0}"
      "td.n{white-space:normal;word-break:break-all;width:100%}td a{color:#0969da;"
      "text-decoration:none}td a:hover{text-decoration:underline}"
      ".dir a::before{content:'\\1F4C1  '}.file a::before{content:'\\1F4C4  '}"
      ".note{color:#59636e;font-size:13px;margin-top:16px}"
      "@media(prefers-color-scheme:dark){body{background:#0d1117;color:#e6edf3}"
      "main{background:#161b22;box-shadow:none}th,.crumbs,.note{color:#8b949e}"
      "th,td{border-color:#30363d}.tools a{border-color:#30363d;color:#e6edf3}"
      ".tools a:hover{background:#21262d}td a,.crumbs a{color:#58a6ff}}"
      "</style></head><body><main><h1>");
  http_strbuf_append_html(&b, share_name);
  http_strbuf_append_cstr(&b, "</h1><div class=\"crumbs\">");

  /* Breadcrumbs: share root, then each segment of rel. */
  char base[64];
  (void)snprintf(base, sizeof(base), HTTP_SHARE_URL_PREFIX "%s/", share->id);
  http_strbuf_append_cstr(&b, "<a href=\"");
  http_strbuf_append_cstr(&b, base);
  http_strbuf_append_cstr(&b, "\">");
  http_strbuf_append_html(&b, share_name);
  http_strbuf_append_cstr(&b, "</a>");
  {
    char walk[FTP_PATH_MAX];
    (void)snprintf(walk, sizeof(walk), "%s", rel);
    char acc[FTP_PATH_MAX] = "";
    char *save = NULL;
    for (char *seg = strtok_r(walk, "/", &save); seg != NULL;
         seg = strtok_r(NULL, "/", &save)) {
      size_t used = strlen(acc);
      (void)snprintf(acc + used, sizeof(acc) - used, "%s/", seg);
      http_strbuf_append_cstr(&b, " / <a href=\"");
      http_strbuf_append_cstr(&b, base);
      http_strbuf_append_url(&b, acc);
      http_strbuf_append_cstr(&b, "\">");
      http_strbuf_append_html(&b, seg);
      http_strbuf_append_cstr(&b, "</a>");
    }
  }
  http_strbuf_append_cstr(&b, "</div><div class=\"tools\"><a href=\"");
  http_strbuf_append_cstr(&b, base);
  http_strbuf_append_url(&b, rel);
  if (rel[0] != '\0') http_strbuf_append_cstr(&b, "/");
  http_strbuf_append_cstr(&b, "?zip=1\">Download this folder as ZIP</a></div>"
                              "<table><thead><tr><th>Name</th><th>Size</th>"
                              "<th>Modified</th></tr></thead><tbody>");

  for (size_t i = 0U; i < count; i++) {
    const share_entry_t *e = &entries[i];
    char when[40];
    char size[40];
    format_time(e->mtime, when, sizeof(when));
    format_size(e->size, size, sizeof(size));
    http_strbuf_append_cstr(&b, e->is_dir ? "<tr class=\"dir\"><td class=\"n\"><a href=\""
                                           : "<tr class=\"file\"><td class=\"n\"><a href=\"");
    http_strbuf_append_cstr(&b, base);
    if (rel[0] != '\0') {
      http_strbuf_append_url(&b, rel);
      http_strbuf_append_cstr(&b, "/");
    }
    http_strbuf_append_url(&b, e->name);
    if (e->is_dir) http_strbuf_append_cstr(&b, "/");
    http_strbuf_append_cstr(&b, "\">");
    http_strbuf_append_html(&b, e->name);
    http_strbuf_append_cstr(&b, "</a></td><td>");
    http_strbuf_append_cstr(&b, e->is_dir ? "-" : size);
    http_strbuf_append_cstr(&b, "</td><td>");
    http_strbuf_append_cstr(&b, when);
    http_strbuf_append_cstr(&b, "</td></tr>");
  }
  if (count == 0U)
    http_strbuf_append_cstr(&b, "<tr><td class=\"n\" colspan=\"3\">This folder is empty.</td></tr>");
  http_strbuf_append_cstr(&b, "</tbody></table>");
  if (truncated)
    http_strbuf_append_cstr(&b, "<p class=\"note\">Only the first 4000 entries are shown.</p>");
  if (share->expires != 0) {
    char when[40];
    format_time(share->expires, when, sizeof(when));
    http_strbuf_append_cstr(&b, "<p class=\"note\">This link expires on ");
    http_strbuf_append_cstr(&b, when);
    http_strbuf_append_cstr(&b, ".</p>");
  }
  http_strbuf_append_cstr(&b, "<p class=\"note\">Shared with zftpd.</p></main></body></html>");
  free(entries);

  size_t len = 0U;
  char *body = http_strbuf_take(&b, &len);
  if (body == NULL)
    return html_page(HTTP_STATUS_500_INTERNAL_ERROR, "Error", "Out of memory.");
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) {
    free(body);
    return NULL;
  }
  http_response_add_header(resp, "Content-Type", "text/html; charset=utf-8");
  if (http_response_set_body_owned(resp, body, len) != 0) {
    free(body);
    http_response_destroy(resp);
    return NULL;
  }
  return resp;
}

static http_response_t *share_zip(const char *dir) {
  const char *paths[1] = {dir};
  zip_writer_t *zip = zip_writer_create(paths, 1U);
  if (zip == NULL || zip_writer_entry_count(zip) == 0U) {
    zip_writer_destroy(zip);
    return html_page(HTTP_STATUS_404_NOT_FOUND, "Nothing to download",
                     "This folder has no files.");
  }
  const char *name = strrchr(dir, '/');
  name = name != NULL && name[1] != '\0' ? name + 1 : "share";
  char zip_name[300];
  (void)snprintf(zip_name, sizeof(zip_name), "%.250s.zip", name);
  return http_api_archive_zip_response(zip, zip_name);
}

http_response_t *http_share_handle(const http_request_t *request) {
  if (request == NULL || !http_share_route_matches(request->uri)) return NULL;
  if (request->method != HTTP_METHOD_GET && request->method != HTTP_METHOD_HEAD)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use GET");

  const char *p = request->uri + sizeof(HTTP_SHARE_URL_PREFIX) - 1U;
  const char *query = strchr(p, '?');
  size_t path_len = query != NULL ? (size_t)(query - p) : strlen(p);

  char id[HTTP_SHARE_ID_HEX + 1U];
  if (path_len < HTTP_SHARE_ID_HEX ||
      (path_len > HTTP_SHARE_ID_HEX && p[HTTP_SHARE_ID_HEX] != '/'))
    return html_page(HTTP_STATUS_404_NOT_FOUND, "Link not found",
                     "This share link does not exist.");
  memcpy(id, p, HTTP_SHARE_ID_HEX);
  id[HTTP_SHARE_ID_HEX] = '\0';

  http_share_t share;
  if (http_share_find(id, &share) != 0)
    return html_page(HTTP_STATUS_404_NOT_FOUND, "Link not found",
                     "This share link does not exist or was removed.");
  if (http_share_is_expired(&share, time(NULL)))
    return html_page(HTTP_STATUS_410_GONE, "Link expired",
                     "This share link has expired.");

  /* Decoded path below the share root, without leading/trailing slashes. */
  char rel[FTP_PATH_MAX] = "";
  if (path_len > HTTP_SHARE_ID_HEX + 1U) {
    const char *sub = p + HTTP_SHARE_ID_HEX + 1U;
    size_t sub_len = path_len - HTTP_SHARE_ID_HEX - 1U;
    if (url_decode(sub, sub_len, rel, sizeof(rel)) != 0)
      return html_page(HTTP_STATUS_400_BAD_REQUEST, "Bad request",
                       "The link is malformed.");
    size_t n = strlen(rel);
    while (n > 0U && rel[n - 1U] == '/') rel[--n] = '\0';
  }

  /* The share itself must still be inside the served root. */
  char root[FTP_PATH_MAX];
  if (!http_api_validate_path(share.path, root, sizeof(root)))
    return html_page(HTTP_STATUS_404_NOT_FOUND, "Not found",
                     "The shared item is no longer available.");

  if (!share.is_dir) {
    if (rel[0] != '\0')
      return html_page(HTTP_STATUS_404_NOT_FOUND, "Not found",
                       "This link points to a single file.");
    struct stat st;
    if (stat(root, &st) != 0 || !S_ISREG(st.st_mode))
      return html_page(HTTP_STATUS_404_NOT_FOUND, "Not found",
                       "The shared file is no longer available.");
    http_response_t *resp = http_api_file_response(root, NULL, request);
    return resp;
  }

  char joined[FTP_PATH_MAX];
  char normalized[FTP_PATH_MAX];
  char target[FTP_PATH_MAX];
  int n = snprintf(joined, sizeof(joined), "%s/%s", root, rel);
  if (n <= 0 || (size_t)n >= sizeof(joined) ||
      ftp_path_normalize(joined, normalized, sizeof(normalized)) != FTP_OK ||
      realpath(normalized, target) == NULL ||
      ftp_path_is_within_root(target, root) != 1 ||
      !http_api_validate_path(target, joined, sizeof(joined)))
    return html_page(HTTP_STATUS_404_NOT_FOUND, "Not found",
                     "This item is not part of the shared folder.");

  struct stat st;
  if (stat(target, &st) != 0)
    return html_page(HTTP_STATUS_404_NOT_FOUND, "Not found",
                     "This item is no longer available.");

  if (S_ISREG(st.st_mode)) return http_api_file_response(target, NULL, request);
  if (!S_ISDIR(st.st_mode))
    return html_page(HTTP_STATUS_404_NOT_FOUND, "Not found",
                     "This item cannot be downloaded.");

  int want_zip = 0;
  if (query != NULL) {
    char flag[8] = {0};
    want_zip = http_api_parse_query_param(query, "zip", flag, sizeof(flag)) == 0 &&
               flag[0] == '1';
  }
  if (want_zip) return share_zip(target);

  /* Canonical rel for links: strip the share root from the resolved path. */
  const char *canonical_rel = target + strlen(root);
  while (*canonical_rel == '/') canonical_rel++;
  return share_listing(&share, target, canonical_rel);
}
