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

#endif
