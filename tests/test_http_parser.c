#include "http_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { \
  if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return -1; \
  } \
} while (0)

static int test_basic_head(void) {
  const char req[] = "GET /api/status?x=1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
  http_request_head_t head;
  CHECK(http_peek_request_head(req, sizeof(req) - 1U, &head) == 0);
  CHECK(head.method == HTTP_METHOD_GET);
  CHECK(strcmp(head.uri, "/api/status?x=1") == 0);
  CHECK(head.content_length == 0U);
  CHECK(head.header_length == sizeof(req) - 1U);
  return 0;
}
static int test_content_length(void) {
  const char req[] =
      "POST /api/upload?path=/&name=a.bin HTTP/1.1\r\n"
      "content-length:\t123 \t\r\nX-Test: ok\r\n\r\nabc";
  http_request_head_t head;
  CHECK(http_peek_request_head(req, sizeof(req) - 1U, &head) == 0);
  CHECK(head.method == HTTP_METHOD_POST);
  CHECK(head.content_length == 123U);
  CHECK(head.header_length < sizeof(req) - 1U);
  return 0;
}

static int test_incomplete(void) {
  const char req[] = "GET / HTTP/1.1\r\nHost: local";
  http_request_head_t head;
  CHECK(http_peek_request_head(req, sizeof(req) - 1U, &head) == -2);
  return 0;
}

static int test_bad_content_length(void) {
  http_request_head_t head;
  const char garbage[] =
      "POST /x HTTP/1.1\r\nContent-Length: 12x\r\n\r\n";
  CHECK(http_peek_request_head(garbage, sizeof(garbage) - 1U, &head) == -1);

  const char overflow[] =
      "POST /x HTTP/1.1\r\nContent-Length: 999999999999999999999999999\r\n\r\n";
  CHECK(http_peek_request_head(overflow, sizeof(overflow) - 1U, &head) == -1);

  const char duplicate[] =
      "POST /x HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n";
  CHECK(http_peek_request_head(duplicate, sizeof(duplicate) - 1U, &head) == -1);
  char mutable_duplicate[sizeof(duplicate)];
  memcpy(mutable_duplicate, duplicate, sizeof(duplicate));
  http_request_t parsed_duplicate;
  CHECK(http_parse_request(mutable_duplicate, sizeof(duplicate) - 1U,
                           &parsed_duplicate) == -1);
  return 0;
}

static int test_overlong_uri(void) {
  size_t uri_len = HTTP_URI_MAX_LENGTH;
  size_t cap = uri_len + 64U;
  char *req = (char *)malloc(cap);
  CHECK(req != NULL);
  size_t pos = 0U;
  memcpy(req + pos, "GET /", 5U);
  pos += 5U;
  memset(req + pos, 'a', uri_len - 1U);
  pos += uri_len - 1U;
  memcpy(req + pos, " HTTP/1.1\r\n\r\n", 13U);
  pos += 13U;

  http_request_head_t head;
  CHECK(http_peek_request_head(req, pos, &head) == -1);

  char *mutable = (char *)malloc(pos + 1U);
  CHECK(mutable != NULL);
  memcpy(mutable, req, pos);
  mutable[pos] = '\0';
  http_request_t parsed;
  CHECK(http_parse_request(mutable, pos, &parsed) == -1);
  free(mutable);
  free(req);
  return 0;
}

static int test_rejects_ambiguous_framing(void) {
  http_request_head_t head;
  const char transfer[] =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
  CHECK(http_peek_request_head(transfer, sizeof(transfer) - 1U, &head) == -1);
  char mutable_transfer[sizeof(transfer)];
  memcpy(mutable_transfer, transfer, sizeof(transfer));
  http_request_t parsed_transfer;
  CHECK(http_parse_request(mutable_transfer, sizeof(transfer) - 1U,
                           &parsed_transfer) == -1);

  const char bad_version[] = "GET / HTTP/1.1junk\r\n\r\n";
  CHECK(http_peek_request_head(bad_version, sizeof(bad_version) - 1U, &head) == -1);
  char mutable_version[sizeof(bad_version)];
  memcpy(mutable_version, bad_version, sizeof(bad_version));
  http_request_t parsed;
  CHECK(http_parse_request(mutable_version, sizeof(bad_version) - 1U, &parsed) == -1);

  char embedded[] = "GET / HTTP/1.1\r\nX-Test: ok\r\n\r\n";
  embedded[22] = '\0';
  CHECK(http_peek_request_head(embedded, sizeof(embedded) - 1U, &head) == -1);
  return 0;
}
int main(void) {
  CHECK(test_basic_head() == 0);
  CHECK(test_content_length() == 0);
  CHECK(test_incomplete() == 0);
  CHECK(test_bad_content_length() == 0);
  CHECK(test_overlong_uri() == 0);
  CHECK(test_rejects_ambiguous_framing() == 0);
  puts("test_http_parser: ok");
  return 0;
}
