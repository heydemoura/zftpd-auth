#include "http_json.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int http_buf_append_bytes(char *buf, size_t cap, size_t *pos,
                            const char *data, size_t len) {
  if ((buf == NULL) || (pos == NULL) || (data == NULL)) {
    return -1;
  }
  if (*pos > cap) {
    return -1;
  }
  if (len > (cap - *pos)) {
    return -1;
  }
  if (len > 0U) {
    memcpy(buf + *pos, data, len);
    *pos += len;
  }
  return 0;
}

int http_buf_append_cstr(char *buf, size_t cap, size_t *pos,
                           const char *str) {
  if (str == NULL) {
    return -1;
  }
  return http_buf_append_bytes(buf, cap, pos, str, strlen(str));
}

int http_buf_append_u64(char *buf, size_t cap, size_t *pos, uint64_t v) {
  char tmp[32];
  int n = snprintf(tmp, sizeof(tmp), "%" PRIu64, v);
  if ((n < 0) || ((size_t)n >= sizeof(tmp))) {
    return -1;
  }
  return http_buf_append_bytes(buf, cap, pos, tmp, (size_t)n);
}

int http_buf_append_u32(char *buf, size_t cap, size_t *pos, uint32_t v) {
  char tmp[16];
  int n = snprintf(tmp, sizeof(tmp), "%" PRIu32, v);
  if ((n < 0) || ((size_t)n >= sizeof(tmp))) {
    return -1;
  }
  return http_buf_append_bytes(buf, cap, pos, tmp, (size_t)n);
}

int http_buf_append_i32(char *buf, size_t cap, size_t *pos, int32_t v) {
  char tmp[16];
  int n = snprintf(tmp, sizeof(tmp), "%" PRId32, v);
  if ((n < 0) || ((size_t)n >= sizeof(tmp))) {
    return -1;
  }
  return http_buf_append_bytes(buf, cap, pos, tmp, (size_t)n);
}

/*===========================================================================*
 * JSON HELPERS
 *===========================================================================*/

/**
 * @brief Append a JSON-escaped string to buffer
 *
 * Escapes: " \ / \b \f \n \r \t and control chars
 */
int http_json_escape_append(char *buf, size_t cap, size_t *pos,
                              const char *str) {
  size_t p = *pos;

  for (const char *s = str; *s != '\0'; s++) {
    unsigned char c = (unsigned char)*s;

    if (p + 6 >= cap) {
      return -1; /* would overflow */
    }

    switch (c) {
    case '"':
      buf[p++] = '\\';
      buf[p++] = '"';
      break;
    case '\\':
      buf[p++] = '\\';
      buf[p++] = '\\';
      break;
    case '\b':
      buf[p++] = '\\';
      buf[p++] = 'b';
      break;
    case '\f':
      buf[p++] = '\\';
      buf[p++] = 'f';
      break;
    case '\n':
      buf[p++] = '\\';
      buf[p++] = 'n';
      break;
    case '\r':
      buf[p++] = '\\';
      buf[p++] = 'r';
      break;
    case '\t':
      buf[p++] = '\\';
      buf[p++] = 't';
      break;
    default:
      if (c < 0x20) {
        p += (size_t)snprintf(buf + p, cap - p, "\\u%04x", c);
      } else {
        buf[p++] = (char)c;
      }
      break;
    }
  }

  *pos = p;
  return 0;
}


/*===========================================================================*
 * JSON BODY READERS
 *===========================================================================*/

/* Returns a pointer to the first non-blank character after "key": */
static const char *json_value_start(const char *body, const char *key) {
  if (body == NULL || key == NULL) return NULL;
  char needle[72];
  int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
  if (n <= 0 || (size_t)n >= sizeof(needle)) return NULL;
  const char *p = body;
  while ((p = strstr(p, needle)) != NULL) {
    const char *q = p + (size_t)n;
    while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
    if (*q == ':') {
      q++;
      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
      return q;
    }
    p = q;
  }
  return NULL;
}

/* Decodes the string starting after the opening quote; returns the position
 * after the closing quote, or NULL when unterminated. */
static const char *json_read_string(const char *p, char *out, size_t out_size) {
  size_t pos = 0U;
  while (*p != '\0' && *p != '"') {
    char c = *p++;
    if (c == '\\') {
      if (*p == '\0') return NULL;
      c = *p++;
      if (c == 'n') c = '\n';
      else if (c == 'r') c = '\r';
      else if (c == 't') c = '\t';
      else if (c == 'b') c = '\b';
      else if (c == 'f') c = '\f';
      else if (c == 'u') {
        /* \uXXXX: keep ASCII, replace anything else with '?'. */
        unsigned code = 0U;
        for (int i = 0; i < 4; i++) {
          char h = *p;
          unsigned v;
          if (h >= '0' && h <= '9') v = (unsigned)(h - '0');
          else if (h >= 'a' && h <= 'f') v = (unsigned)(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F') v = (unsigned)(h - 'A' + 10);
          else return NULL;
          code = code * 16U + v;
          p++;
        }
        c = code < 0x80U ? (char)code : '?';
      }
    }
    if (out != NULL && pos + 1U < out_size) out[pos++] = c;
  }
  if (out != NULL && out_size > 0U) out[pos < out_size ? pos : out_size - 1U] = '\0';
  return *p == '"' ? p + 1 : NULL;
}

int http_json_body_string(const char *body, const char *key, char *out,
                          size_t out_size) {
  if (out == NULL || out_size == 0U) return 0;
  out[0] = '\0';
  const char *p = json_value_start(body, key);
  if (p == NULL || *p != '"') return 0;
  return json_read_string(p + 1, out, out_size) != NULL;
}

int http_json_body_i64(const char *body, const char *key, int64_t *out) {
  const char *p = json_value_start(body, key);
  if (p == NULL || out == NULL) return 0;
  if (*p == '"') p++; /* tolerate quoted numbers */
  if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return 0;
  errno = 0;
  char *end = NULL;
  long long value = strtoll(p, &end, 10);
  if (errno != 0 || end == p) return 0;
  *out = (int64_t)value;
  return 1;
}

int http_json_body_bool(const char *body, const char *key, int *out) {
  const char *p = json_value_start(body, key);
  if (p == NULL || out == NULL) return 0;
  if (strncmp(p, "true", 4) == 0) { *out = 1; return 1; }
  if (strncmp(p, "false", 5) == 0) { *out = 0; return 1; }
  if (*p == '1' || *p == '0') { *out = *p == '1'; return 1; }
  return 0;
}

int http_json_body_array_next(const char *body, const char *key,
                              const char **cursor, char *out, size_t out_size) {
  if (cursor == NULL || out == NULL || out_size == 0U) return 0;
  const char *p = *cursor;
  if (p == NULL) {
    p = json_value_start(body, key);
    if (p == NULL || *p != '[') return 0;
    p++;
  }
  for (;;) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
    if (*p == '"') {
      const char *next = json_read_string(p + 1, out, out_size);
      if (next == NULL) {
        *cursor = p + strlen(p);
        return 0;
      }
      *cursor = next;
      return 1;
    }
    if (*p == ']' || *p == '\0') {
      *cursor = p;
      return 0;
    }
    /* Non-string element: skip to the next separator. */
    while (*p != '\0' && *p != ',' && *p != ']') p++;
  }
}

/*===========================================================================*
 * GROWABLE BUFFER
 *===========================================================================*/

void http_strbuf_init(http_strbuf_t *b) {
  b->data = NULL;
  b->len = 0U;
  b->cap = 0U;
  b->failed = 0;
}

void http_strbuf_free(http_strbuf_t *b) {
  free(b->data);
  http_strbuf_init(b);
}

int http_strbuf_failed(const http_strbuf_t *b) { return b->failed; }

static int strbuf_reserve(http_strbuf_t *b, size_t extra) {
  if (b->failed) return -1;
  if (b->len + extra + 1U <= b->cap) return 0;
  size_t cap = b->cap == 0U ? 1024U : b->cap;
  while (cap < b->len + extra + 1U) {
    if (cap > ((size_t)1 << 30)) {
      b->failed = 1;
      return -1;
    }
    cap *= 2U;
  }
  char *grown = realloc(b->data, cap);
  if (grown == NULL) {
    b->failed = 1;
    return -1;
  }
  b->data = grown;
  b->cap = cap;
  return 0;
}

int http_strbuf_append(http_strbuf_t *b, const char *data, size_t len) {
  if (strbuf_reserve(b, len) != 0) return -1;
  memcpy(b->data + b->len, data, len);
  b->len += len;
  b->data[b->len] = '\0';
  return 0;
}

int http_strbuf_append_cstr(http_strbuf_t *b, const char *str) {
  return http_strbuf_append(b, str, strlen(str));
}

int http_strbuf_append_u64(http_strbuf_t *b, uint64_t value) {
  char tmp[32];
  int n = snprintf(tmp, sizeof(tmp), "%" PRIu64, value);
  return n > 0 ? http_strbuf_append(b, tmp, (size_t)n) : -1;
}

int http_strbuf_append_i64(http_strbuf_t *b, int64_t value) {
  char tmp[32];
  int n = snprintf(tmp, sizeof(tmp), "%" PRId64, value);
  return n > 0 ? http_strbuf_append(b, tmp, (size_t)n) : -1;
}

int http_strbuf_append_json(http_strbuf_t *b, const char *str) {
  for (const char *s = str; *s != '\0'; s++) {
    unsigned char c = (unsigned char)*s;
    char tmp[8];
    const char *piece = tmp;
    size_t len = 1U;
    switch (c) {
    case '"': piece = "\\\""; len = 2U; break;
    case '\\': piece = "\\\\"; len = 2U; break;
    case '\b': piece = "\\b"; len = 2U; break;
    case '\f': piece = "\\f"; len = 2U; break;
    case '\n': piece = "\\n"; len = 2U; break;
    case '\r': piece = "\\r"; len = 2U; break;
    case '\t': piece = "\\t"; len = 2U; break;
    default:
      if (c < 0x20U) {
        len = (size_t)snprintf(tmp, sizeof(tmp), "\\u%04x", c);
      } else {
        tmp[0] = (char)c;
      }
      break;
    }
    if (http_strbuf_append(b, piece, len) != 0) return -1;
  }
  return 0;
}

int http_strbuf_append_html(http_strbuf_t *b, const char *str) {
  for (const char *s = str; *s != '\0'; s++) {
    const char *piece;
    switch (*s) {
    case '&': piece = "&amp;"; break;
    case '<': piece = "&lt;"; break;
    case '>': piece = "&gt;"; break;
    case '"': piece = "&quot;"; break;
    case '\'': piece = "&#39;"; break;
    default:
      if (http_strbuf_append(b, s, 1U) != 0) return -1;
      continue;
    }
    if (http_strbuf_append_cstr(b, piece) != 0) return -1;
  }
  return 0;
}

int http_strbuf_append_url(http_strbuf_t *b, const char *str) {
  static const char hex[] = "0123456789ABCDEF";
  for (const char *s = str; *s != '\0'; s++) {
    unsigned char c = (unsigned char)*s;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~' || c == '/') {
      if (http_strbuf_append(b, s, 1U) != 0) return -1;
      continue;
    }
    char enc[3] = {'%', hex[c >> 4], hex[c & 0x0fU]};
    if (http_strbuf_append(b, enc, 3U) != 0) return -1;
  }
  return 0;
}

char *http_strbuf_take(http_strbuf_t *b, size_t *len) {
  if (b->failed) {
    http_strbuf_free(b);
    if (len != NULL) *len = 0U;
    return NULL;
  }
  if (b->data == NULL && strbuf_reserve(b, 0U) != 0) return NULL;
  char *out = b->data;
  if (len != NULL) *len = b->len;
  http_strbuf_init(b);
  return out;
}
