#ifndef ZFTPD_HTTP_JSON_H
#define ZFTPD_HTTP_JSON_H

#include <stddef.h>
#include <stdint.h>

int http_buf_append_bytes(char *buf, size_t cap, size_t *pos,
                          const char *data, size_t len);
int http_buf_append_cstr(char *buf, size_t cap, size_t *pos, const char *str);
int http_buf_append_u64(char *buf, size_t cap, size_t *pos, uint64_t value);
int http_buf_append_u32(char *buf, size_t cap, size_t *pos, uint32_t value);
int http_buf_append_i32(char *buf, size_t cap, size_t *pos, int32_t value);
int http_json_escape_append(char *buf, size_t cap, size_t *pos,
                            const char *str);

/*
 * Minimal readers for the flat JSON objects the web interface posts.  They
 * locate "key" at any depth (first occurrence) and decode the usual escapes;
 * nested objects, unicode escapes and duplicate keys are out of scope.
 */

/** 1 when "key" holds a string (copied, truncated to out_size - 1). */
int http_json_body_string(const char *body, const char *key, char *out,
                          size_t out_size);
/** 1 when "key" holds an integer. */
int http_json_body_i64(const char *body, const char *key, int64_t *out);
/** 1 when "key" holds true/false; *out set accordingly. */
int http_json_body_bool(const char *body, const char *key, int *out);
/**
 * Iterate the strings of "key": [ ... ].  Start with *cursor == NULL; each
 * call that returns 1 fills @p out and advances the cursor.
 */
int http_json_body_array_next(const char *body, const char *key,
                              const char **cursor, char *out, size_t out_size);

/*
 * Growable text buffer for responses that do not fit a fixed stack buffer
 * (user lists, share tables, directory pages).  A failed allocation sticks:
 * check http_strbuf_failed() once at the end instead of every append.
 */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
  int failed;
} http_strbuf_t;

void http_strbuf_init(http_strbuf_t *b);
void http_strbuf_free(http_strbuf_t *b);
int http_strbuf_failed(const http_strbuf_t *b);
int http_strbuf_append(http_strbuf_t *b, const char *data, size_t len);
int http_strbuf_append_cstr(http_strbuf_t *b, const char *str);
int http_strbuf_append_u64(http_strbuf_t *b, uint64_t value);
int http_strbuf_append_i64(http_strbuf_t *b, int64_t value);
/** Appends a JSON-escaped string body (no surrounding quotes). */
int http_strbuf_append_json(http_strbuf_t *b, const char *str);
/** Appends HTML text with & < > " ' escaped. */
int http_strbuf_append_html(http_strbuf_t *b, const char *str);
/** Appends a percent-encoded URL path segment (keeps unreserved and '/'). */
int http_strbuf_append_url(http_strbuf_t *b, const char *str);
/** Hands the NUL-terminated buffer to the caller (free() it). */
char *http_strbuf_take(http_strbuf_t *b, size_t *len);

#endif
