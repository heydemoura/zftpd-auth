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
 * @file http_parser.c
 * @brief Minimal HTTP/1.1 request parser (no strtok, no strncpy)
 *
 * Rewrites the original parser to avoid:
 *   - strtok() : hidden global state, not reentrant
 *   - strncpy(): no guaranteed NUL termination
 *
 * Uses bounded pointer arithmetic with explicit length checks.
 * The input buffer is mutated in-place (NUL inserted at delimiters)
 * so header name/value pointers remain valid for the buffer's lifetime.
 */

#include "http_parser.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/*===========================================================================*
 *  HELPERS
 *
 *   find_char()  — bounded strchr (stays within [p, end))
 *   find_crlf()  — bounded strstr for "\r\n"
 *===========================================================================*/

/**
 * @brief Find first occurrence of c in [p, end)
 * @return Pointer to c, or NULL if not found
 */
static char *find_char(char *p, const char *end, char c) {
  while (p < end) {
    if (*p == c) {
      return p;
    }
    p++;
  }
  return NULL;
}

/**
 * @brief Find "\r\n" in [p, end)
 * @return Pointer to the '\r', or NULL if not found
 */
static char *find_crlf(char *p, const char *end) {
  while ((p + 1) < end) {
    if (p[0] == '\r' && p[1] == '\n') {
      return p;
    }
    p++;
  }
  return NULL;
}

static const char *find_crlf_const(const char *p, const char *end) {
  while ((p + 1) < end) {
    if (p[0] == '\r' && p[1] == '\n') return p;
    p++;
  }
  return NULL;
}

static http_method_t method_from_span(const char *s, size_t len) {
  if (len == 3U && memcmp(s, "GET", 3U) == 0) return HTTP_METHOD_GET;
  if (len == 4U && memcmp(s, "POST", 4U) == 0) return HTTP_METHOD_POST;
  if (len == 4U && memcmp(s, "HEAD", 4U) == 0) return HTTP_METHOD_HEAD;
  return HTTP_METHOD_UNKNOWN;
}

static int parse_http_version_span(const char *s, size_t len, int *major,
                                   int *minor) {
  if (s == NULL || major == NULL || minor == NULL || len != 8U ||
      memcmp(s, "HTTP/", 5U) != 0 || s[5] < '0' || s[5] > '9' ||
      s[6] != '.' || s[7] < '0' || s[7] > '9')
    return -1;
  *major = s[5] - '0';
  *minor = s[7] - '0';
  return 0;
}

static int parse_size_decimal(const char *p, const char *end, size_t *out) {
  while (p < end && (*p == ' ' || *p == '\t')) p++;
  if (p == end || *p < '0' || *p > '9') return -1;

  size_t value = 0U;
  const size_t max = (size_t)-1;
  while (p < end && *p >= '0' && *p <= '9') {
    unsigned digit = (unsigned)(*p - '0');
    if (value > (max - digit) / 10U) return -1;
    value = value * 10U + digit;
    p++;
  }
  while (p < end && (*p == ' ' || *p == '\t')) p++;
  if (p != end) return -1;
  *out = value;
  return 0;
}

/*===========================================================================*
 *  PARSE REQUEST LINE
 *
 *   "GET /index.html HTTP/1.1\r\n"
 *    ^method  ^uri       ^version
 *
 *   Splits on spaces with bounded search, no strtok.
 *===========================================================================*/

/**
 * @brief Parse the request line: METHOD SP URI SP HTTP/x.y
 *
 * @param[in,out] line     Start of request line (NUL-terminated by caller)
 * @param[in]     line_len Length of the line (excluding NUL)
 * @param[out]    request  Populated with method, uri, version
 *
 * @return 0 on success, -1 on malformed request
 */
static int parse_request_line(char *line, size_t line_len,
                              http_request_t *request) {
  const char *end = line + line_len;

  /*-- METHOD --*/
  char *sp1 = find_char(line, end, ' ');
  if (sp1 == NULL) {
    return -1;
  }
  *sp1 = '\0';

  size_t method_len = (size_t)(sp1 - line);
  if (memchr(line, '\0', method_len) != NULL) return -1;
  request->method = method_from_span(line, method_len);

  /*-- URI --*/
  char *uri_start = sp1 + 1;
  if (uri_start >= end) {
    return -1;
  }
  char *sp2 = find_char(uri_start, end, ' ');
  if (sp2 == NULL) {
    return -1;
  }
  *sp2 = '\0';

  /* Bounded copy into fixed-size uri field */
  size_t uri_len = (size_t)(sp2 - uri_start);
  if (uri_len == 0U || uri_len >= HTTP_URI_MAX_LENGTH ||
      memchr(uri_start, '\0', uri_len) != NULL)
    return -1;
  memcpy(request->uri, uri_start, uri_len);
  request->uri[uri_len] = '\0';

  /*-- VERSION  "HTTP/x.y" --*/
  char *ver_start = sp2 + 1;
  if (ver_start >= end) {
    return -1;
  }
  if (parse_http_version_span(ver_start, (size_t)(end - ver_start),
                              &request->version_major,
                              &request->version_minor) != 0)
    return -1;

  return 0;
}

/*===========================================================================*
 *  PARSE HEADERS
 *
 *   "Content-Type: application/json\r\n"
 *    ^name        ^colon  ^value
 *
 *   Each header line is NUL-terminated at the \r\n boundary.
 *   Colon is replaced with NUL to split name/value in-place.
 *   Leading whitespace on value is skipped.
 *===========================================================================*/

/**
 * @brief Parse a single header line into request->headers[]
 *
 * @param[in,out] line    Header line, already NUL-terminated
 * @param[out]    request Target request struct
 */
static int parse_header_line(char *line, size_t line_len,
                             http_request_t *request) {
  if (request->num_headers >= HTTP_HEADER_MAX_COUNT ||
      memchr(line, '\0', line_len) != NULL)
    return -1;
  char *colon = memchr(line, ':', line_len);
  if (colon == NULL || colon == line) return -1;

  size_t name_len = (size_t)(colon - line);
  if (name_len == 17U && strncasecmp(line, "Transfer-Encoding", 17U) == 0)
    return -1;

  *colon = '\0';
  char *value = colon + 1;
  while (*value == ' ' || *value == '\t') value++;
  request->headers[request->num_headers].name = line;
  request->headers[request->num_headers].value = value;
  request->num_headers++;
  return 0;
}

/*===========================================================================*
 *  PUBLIC API
 *===========================================================================*/

/**
 * @brief Parse a complete HTTP/1.1 request from a mutable buffer
 *
 * The buffer is mutated in-place: NUL bytes are inserted at
 * line boundaries and colon separators so that request->uri,
 * header name/value, and body pointers reference the buffer
 * directly with zero-copy semantics.
 *
 * @param[in,out] buffer  Raw HTTP request data (mutable)
 * @param[in]     length  Number of valid bytes in buffer
 * @param[out]    request Parsed result
 *
 * @return 0 on success, negative on parse error
 *   -1: NULL input or malformed request line
 *   -2: missing CRLF (incomplete request)
 */
int http_peek_request_head(const char *buffer, size_t length,
                           http_request_head_t *head) {
  if (buffer == NULL || head == NULL || length == 0U) return -1;
  memset(head, 0, sizeof(*head));

  const char *end = buffer + length;
  const char *line_end = find_crlf_const(buffer, end);
  if (line_end == NULL) return -2;
  if (memchr(buffer, '\0', (size_t)(line_end - buffer)) != NULL) return -1;

  const char *sp1 = memchr(buffer, ' ', (size_t)(line_end - buffer));
  if (sp1 == NULL) return -1;
  const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - (sp1 + 1)));
  if (sp2 == NULL) return -1;

  head->method = method_from_span(buffer, (size_t)(sp1 - buffer));
  size_t uri_len = (size_t)(sp2 - (sp1 + 1));
  if (uri_len == 0U || uri_len >= sizeof(head->uri)) return -1;
  memcpy(head->uri, sp1 + 1, uri_len);
  head->uri[uri_len] = '\0';
  int version_major = 0, version_minor = 0;
  if (parse_http_version_span(sp2 + 1, (size_t)(line_end - (sp2 + 1)),
                              &version_major, &version_minor) != 0)
    return -1;

  const char *p = line_end + 2;
  int saw_content_length = 0;
  for (;;) {
    if (p >= end) return -2;
    const char *eol = find_crlf_const(p, end);
    if (eol == NULL) return -2;
    if (eol == p) {
      head->header_length = (size_t)((eol + 2) - buffer);
      return 0;
    }
    if (memchr(p, '\0', (size_t)(eol - p)) != NULL) return -1;

    const char *colon = memchr(p, ':', (size_t)(eol - p));
    if (colon != NULL) {
      size_t name_len = (size_t)(colon - p);
      if (name_len == 17U && strncasecmp(p, "Transfer-Encoding", 17U) == 0)
        return -1;
      if (name_len == 14U && strncasecmp(p, "Content-Length", 14U) == 0) {
        if (saw_content_length != 0 ||
            parse_size_decimal(colon + 1, eol, &head->content_length) != 0)
          return -1;
        saw_content_length = 1;
      }
    }
    p = eol + 2;
  }
}

int http_parse_request(char *buffer, size_t length, http_request_t *request) {
  if ((buffer == NULL) || (request == NULL) || (length == 0U)) {
    return -1;
  }

  http_request_head_t head;
  int head_rc = http_peek_request_head(buffer, length, &head);
  if (head_rc != 0) return head_rc;

  memset(request, 0, sizeof(*request));
  const char *buf_end = buffer + length;

  /*-- Request line --*/
  char *crlf = find_crlf(buffer, buf_end);
  if (crlf == NULL) {
    return -2;
  }
  *crlf = '\0'; /* NUL-terminate request line */

  size_t line_len = (size_t)(crlf - buffer);
  if (parse_request_line(buffer, line_len, request) != 0) {
    return -1;
  }

  /*-- Headers --*/
  char *line = crlf + 2; /* skip past \r\n */
  while (line < buf_end) {
    crlf = find_crlf(line, buf_end);
    if (crlf == NULL) {
      break; /* truncated headers, stop */
    }
    size_t header_line_len = (size_t)(crlf - line);
    *crlf = '\0';

    /* Empty line = end of headers, body follows */
    if (line[0] == '\0') {
      char *body_start = crlf + 2;
      if (body_start < buf_end) {
        request->body = body_start;
        request->body_length = (size_t)(buf_end - body_start);
      }
      break;
    }

    if (parse_header_line(line, header_line_len, request) != 0) return -1;
    line = crlf + 2;
  }

  return 0;
}

/**
 * @brief Look up a header value by name (case-insensitive)
 *
 * @param request Parsed request
 * @param name    Header name to search for
 *
 * @return Header value string, or NULL if not found
 */
const char *http_get_header(const http_request_t *request, const char *name) {
  if ((request == NULL) || (name == NULL)) {
    return NULL;
  }

  for (size_t i = 0; i < request->num_headers; i++) {
    if (strcasecmp(request->headers[i].name, name) == 0) {
      return request->headers[i].value;
    }
  }

  return NULL;
}
