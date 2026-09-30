#include "transfer_internal.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(ZFTPD_EMBEDDED_CA_BUNDLE) && ZFTPD_EMBEDDED_CA_BUNDLE
#include "zftpd_ca_bundle.h"
#endif

#define CURL_BUFFER_BYTES (512L * 1024L)
#define CURL_RANGE_STREAMS 4U
#define CURL_RANGE_CHUNK_BYTES (8U * 1024U * 1024U)
#define CURL_RANGE_MIN_BYTES (128U * 1024U * 1024U)
#define CURL_PARALLEL_THRESHOLD_BYTES_PER_SEC 16000000.0

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static CURLcode g_curl_init_result = CURLE_FAILED_INIT;

static void curl_global_init_once(void) {
  g_curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
}

typedef struct {
  transfer_job_t *job;
  int fd;
} curl_write_ctx_t;

typedef struct {
  transfer_job_t *job;
  uint64_t from;
} curl_progress_ctx_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *opaque) {
  curl_write_ctx_t *ctx = (curl_write_ctx_t *)opaque;
  if (ctx == NULL || ctx->job == NULL || ptr == NULL) return 0U;
  if (size != 0U && nmemb > SIZE_MAX / size) return 0U;
  if (transfer_job_wait_if_paused(ctx->job) != 0) return 0U;
  size_t bytes = size * nmemb;
  return transfer_write_all(ctx->fd, ptr, bytes) == 0 ? bytes : 0U;
}

static int curl_progress_cb(void *opaque, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow) {
  curl_progress_ctx_t *ctx = (curl_progress_ctx_t *)opaque;
  transfer_job_t *job = ctx->job;
  (void)ultotal;
  (void)ulnow;
  if (job == NULL || atomic_load(&job->cancel_requested) != 0) return 1;
  if (dltotal > 0) {
    atomic_store(&job->total_size, ctx->from + (uint64_t)dltotal);
  }
  if (dlnow >= 0) {
    atomic_store(&job->downloaded, ctx->from + (uint64_t)dlnow);
  }
  return 0;
}

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
/* Ask for a larger TCP receive window before connect(), when window scaling
 * is negotiated. The console socket default can cap WAN download throughput;
 * CURLOPT_BUFFERSIZE only changes libcurl's userspace buffer. Try smaller
 * values when a firmware's socket buffer limit rejects the larger ones. */
static int curl_sockopt_cb(void *opaque, curl_socket_t fd,
                           curlsocktype purpose) {
  (void)opaque;
  if (purpose == CURLSOCKTYPE_IPCXN) {
    int current = 0;
    socklen_t current_len = sizeof(current);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &current, &current_len) != 0)
      current = 0;
    const int targets[] = {4 * 1024 * 1024, 2 * 1024 * 1024,
                           1024 * 1024, 512 * 1024, 256 * 1024};
    for (size_t i = 0U; i < sizeof(targets) / sizeof(targets[0]); i++) {
      if (current >= targets[i] ||
          setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &targets[i],
                     sizeof(targets[i])) == 0)
        break;
    }
  }
  return CURL_SOCKOPT_OK;
}
#endif

static void curl_set_ca_path(CURL *curl) {
#if defined(ZFTPD_EMBEDDED_CA_BUNDLE) && ZFTPD_EMBEDDED_CA_BUNDLE
  struct curl_blob ca_blob;
  ca_blob.data = zftpd_ca_bundle;
  ca_blob.len = zftpd_ca_bundle_len;
  ca_blob.flags = CURL_BLOB_NOCOPY;
  (void)curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca_blob);
#elif defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  static const char *const candidates[] = {
      "/user/homebrew/etc/ca-bundle.crt",
      "/data/zftpd/cacert.pem",
      "/data/cacert.pem",
      NULL};
  for (size_t i = 0U; candidates[i] != NULL; i++) {
    if (access(candidates[i], R_OK) == 0) {
      (void)curl_easy_setopt(curl, CURLOPT_CAINFO, candidates[i]);
      break;
    }
  }
#else
  (void)curl;
#endif
}

static void curl_configure(CURL *curl, transfer_job_t *job,
                           curl_write_ctx_t *wctx,
                           curl_progress_ctx_t *pctx, char *error_buffer,
                           uint64_t resume_from) {
  (void)curl_easy_setopt(curl, CURLOPT_URL, job->url);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, wctx);
  (void)curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
  (void)curl_easy_setopt(curl, CURLOPT_XFERINFODATA, pctx);
  (void)curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  (void)curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  (void)curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
  (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "zftpd/1.5");
  (void)curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, CURL_BUFFER_BYTES);
  (void)curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
#if LIBCURL_VERSION_NUM >= 0x074B00
  /* Dual-stack hosts: try the other address family quickly instead of burning
   * the whole connect timeout on an address the console cannot route. */
  (void)curl_easy_setopt(curl, CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS, 200L);
#endif
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  (void)curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, curl_sockopt_cb);
#endif
#if LIBCURL_VERSION_NUM >= 0x075500
  (void)curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https,ftp,ftps");
  (void)curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https,ftp,ftps");
#else
  (void)curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                         (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS |
                                CURLPROTO_FTP | CURLPROTO_FTPS));
  (void)curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                         (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS |
                                CURLPROTO_FTP | CURLPROTO_FTPS));
#endif
  if (resume_from > 0U) {
    (void)curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                           (curl_off_t)resume_from);
  }
  curl_set_ca_path(curl);
}

/**
 * Explain a failing HTTP status in terms the UI can act on.
 *
 * A bare "curl: The requested URL returned error: 403" tells the user nothing
 * about whether zftpd, the link or the remote host is at fault — which is
 * exactly what happens when a host answers with a bot-protection challenge.
 */
static const char *http_status_reason(long status) {
  switch (status) {
  case 401L:
  case 407L:
    return "authentication required";
  case 403L:
    return "access denied — the host may block automated downloads";
  case 404L:
  case 410L:
    return "not found — check the link";
  case 429L:
    return "too many requests — the host is rate limiting";
  default:
    break;
  }
  if (status >= 500L) {
    return "the remote server reported an error";
  }
  return "request rejected";
}

/** True for failures that mean "this host cannot be reached at all". */
static int curl_code_is_connect_failure(CURLcode code) {
  return code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT ||
         code == CURLE_OPERATION_TIMEDOUT;
}

/** Host portion of an absolute URL ("" when it cannot be isolated). */
static void curl_url_host(const char *url, char *out, size_t out_size) {
  if (url == NULL || out == NULL || out_size < 2U) return;
  out[0] = '\0';

  const char *p = strstr(url, "://");
  p = (p != NULL) ? p + 3 : url;

  size_t len = 0U;
  while (p[len] != '\0' && p[len] != '/' && p[len] != '?' &&
         p[len] != '#' && p[len] != ':') {
    len++;
  }
  for (size_t i = 0U; i < len; i++) {
    if (p[i] == '@') { /* drop any userinfo */
      len -= i + 1U;
      p += i + 1U;
      break;
    }
  }
  if (len >= out_size) len = out_size - 1U;
  memcpy(out, p, len);
  out[len] = '\0';
}

/**
 * Addresses the console resolves a host to, comma separated.
 *
 * Reported when a connection fails: it separates "DNS is broken" from "the
 * address is unreachable", which the raw curl message never does.
 */
static void curl_resolve_addrs(const char *host, char *out, size_t out_size) {
  if (host == NULL || out == NULL || out_size < 2U) return;
  out[0] = '\0';
  if (host[0] == '\0') return;

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *list = NULL;
  if (getaddrinfo(host, NULL, &hints, &list) != 0 || list == NULL) return;

  size_t pos = 0U;
  unsigned seen = 0U;
  for (const struct addrinfo *ai = list; ai != NULL && seen < 4U;
       ai = ai->ai_next) {
    char addr[INET6_ADDRSTRLEN] = {0};
    unsigned char raw[sizeof(struct in6_addr)];
    memset(raw, 0, sizeof(raw));

    /* Copy the address out of the socket address instead of casting it:
     * the structure is only guaranteed to be aligned as sockaddr. */
    if (ai->ai_family == AF_INET) {
      struct sockaddr_in sa;
      memset(&sa, 0, sizeof(sa));
      memcpy(&sa, ai->ai_addr,
             ai->ai_addrlen < sizeof(sa) ? (size_t)ai->ai_addrlen : sizeof(sa));
      memcpy(raw, &sa.sin_addr, sizeof(sa.sin_addr));
    } else if (ai->ai_family == AF_INET6) {
      struct sockaddr_in6 sa6;
      memset(&sa6, 0, sizeof(sa6));
      memcpy(&sa6, ai->ai_addr,
             ai->ai_addrlen < sizeof(sa6) ? (size_t)ai->ai_addrlen
                                          : sizeof(sa6));
      memcpy(raw, &sa6.sin6_addr, sizeof(sa6.sin6_addr));
    } else {
      continue;
    }


    if (inet_ntop(ai->ai_family, raw, addr, sizeof(addr)) == NULL) continue;
    if (strstr(out, addr) != NULL) continue; /* same address, other socket type */
    int n = snprintf(out + pos, out_size - pos, "%s%s", pos > 0U ? ", " : "",
                     addr);
    if (n < 0 || (size_t)n >= out_size - pos) break;
    pos += (size_t)n;
    seen++;
  }
  freeaddrinfo(list);
}

/**
 * Report a failed transfer.
 *
 * Connection failures get the host and what the console resolved it to, so a
 * user can tell an unreachable server from broken DNS; everything else keeps
 * the raw curl detail.
 */
static void curl_set_transfer_error(transfer_job_t *job, CURLcode code,
                                    const char *error_buffer, long status) {
  if (job == NULL) return;

  if (atomic_load(&job->cancel_requested) != 0) {
    transfer_job_set_error(job, "Cancelled by user");
    return;
  }
  if (status >= 400L) {
    /* The transport error code varies (CURLE_HTTP_RETURNED_ERROR, or
     * CURLE_RECV_ERROR when the peer drops the connection after the error
     * response): the HTTP status is what identifies the cause. */
    transfer_job_set_error(job, "Remote server returned HTTP %ld: %s", status,
                           http_status_reason(status));
    return;
  }

  if (curl_code_is_connect_failure(code)) {
    char host[256];
    char addrs[192];
    curl_url_host(job->url, host, sizeof(host));
    curl_resolve_addrs(host, addrs, sizeof(addrs));
    if (host[0] == '\0') {
      transfer_job_set_error(job, "curl: %s",
                             error_buffer[0] != '\0' ? error_buffer
                                                     : curl_easy_strerror(code));
      return;
    }
    const char *reason =
        (code == CURLE_COULDNT_RESOLVE_HOST)
            ? "the console could not resolve the host"
            : ((code == CURLE_COULDNT_CONNECT)
                   ? "the connection was refused or unreachable"
                   : "the connection timed out");
    if (addrs[0] != '\0') {
      transfer_job_set_error(job, "Cannot reach %s (%s): %s", host, addrs,
                             reason);
    } else {
      transfer_job_set_error(job, "Cannot reach %s: %s", host, reason);
    }
    return;
  }

  if (error_buffer != NULL && error_buffer[0] != '\0') {
    transfer_job_set_error(job, "curl: %s", error_buffer);
  } else {
    transfer_job_set_error(job, "curl: %s", curl_easy_strerror(code));
  }
}

typedef struct {
  long status;
  uint64_t first;
  uint64_t last;
  uint64_t total;
  int have_range;
  int identity_encoding;
  char etag[128];
} range_headers_t;

typedef struct {
  transfer_job_t *job;
  CURL *curl; /* non-NULL when the parallel worker reuses a connection */
  atomic_int *abort_wave;
  unsigned char *buffer;
  size_t capacity;
  size_t received;
  uint64_t first;
  uint64_t last;
  uint64_t total; /* zero during the size probe */
  const char *validator;
  int ok;
  int invalid_response;
  CURLcode code; /**< transport result, so callers can classify the failure */
  range_headers_t headers;
  char range[64];
  char error[160];
} range_ctx_t;

static int range_parse_number(const char **cursor, uint64_t *value) {
  const char *p = *cursor;
  if (*p < '0' || *p > '9') return -1;
  errno = 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(p, &end, 10);
  if (errno != 0 || end == p) return -1;
  *value = (uint64_t)parsed;
  *cursor = end;
  return 0;
}

static int range_parse_content_range(const char *value, range_headers_t *out) {
  while (*value == ' ' || *value == '\t') value++;
  if (strncasecmp(value, "bytes ", 6U) != 0) return -1;
  const char *p = value + 6U;
  if (range_parse_number(&p, &out->first) != 0 || *p++ != '-' ||
      range_parse_number(&p, &out->last) != 0 || *p++ != '/' ||
      range_parse_number(&p, &out->total) != 0 || *p != '\0' ||
      out->first > out->last || out->last >= out->total)
    return -1;
  return 0;
}

static int range_copy_header(char *dst, size_t capacity, const char *value) {
  while (*value == ' ' || *value == '\t') value++;
  size_t len = strlen(value);
  while (len > 0U && (value[len - 1U] == ' ' || value[len - 1U] == '\t')) len--;
  if (len == 0U || len >= capacity) return -1;
  for (size_t i = 0U; i < len; i++) {
    if ((unsigned char)value[i] < 32U || (unsigned char)value[i] == 127U)
      return -1;
  }
  memcpy(dst, value, len);
  dst[len] = '\0';
  return 0;
}

static size_t range_header_cb(char *ptr, size_t size, size_t nmemb,
                              void *opaque) {
  range_ctx_t *ctx = (range_ctx_t *)opaque;
  if (size != 0U && nmemb > SIZE_MAX / size) return 0U;
  size_t bytes = size * nmemb;
  char line[512];
  if (bytes >= sizeof(line)) return 0U;
  memcpy(line, ptr, bytes);
  line[bytes] = '\0';
  while (bytes > 0U && (line[bytes - 1U] == '\r' || line[bytes - 1U] == '\n'))
    line[--bytes] = '\0';

  range_headers_t *headers = &ctx->headers;
  if (strncasecmp(line, "HTTP/", 5U) == 0) {
    memset(headers, 0, sizeof(*headers));
    headers->identity_encoding = 1;
    const char *code = strchr(line, ' ');
    if (code != NULL) headers->status = strtol(code + 1, NULL, 10);
  } else if (strncasecmp(line, "Content-Range:", 14U) == 0) {
    headers->have_range =
        range_parse_content_range(line + 14U, headers) == 0;
  } else if (strncasecmp(line, "ETag:", 5U) == 0) {
    (void)range_copy_header(headers->etag, sizeof(headers->etag), line + 5U);
  } else if (strncasecmp(line, "Content-Encoding:", 17U) == 0) {
    char encoding[32];
    headers->identity_encoding =
        range_copy_header(encoding, sizeof(encoding), line + 17U) == 0 &&
        strcasecmp(encoding, "identity") == 0;
  }
  return size * nmemb;
}

static int range_headers_match(const range_ctx_t *ctx) {
  const range_headers_t *h = &ctx->headers;
  if (h->status != 206L || h->have_range == 0 || h->identity_encoding == 0 ||
      h->first != ctx->first || h->last != ctx->last ||
      (ctx->total != 0U && h->total != ctx->total))
    return 0;
  if (ctx->validator != NULL) {
    if (strcmp(h->etag, ctx->validator) != 0) return 0;
  }
  return 1;
}

static int range_wait_if_paused(range_ctx_t *ctx) {
  while (atomic_load(&ctx->job->paused) != 0 &&
         atomic_load(&ctx->job->cancel_requested) == 0 &&
         (ctx->abort_wave == NULL || atomic_load(ctx->abort_wave) == 0))
    usleep(100000U);
  return atomic_load(&ctx->job->cancel_requested) != 0 ||
         (ctx->abort_wave != NULL && atomic_load(ctx->abort_wave) != 0);
}

static size_t range_write_cb(void *ptr, size_t size, size_t nmemb,
                             void *opaque) {
  range_ctx_t *ctx = (range_ctx_t *)opaque;
  if (ctx == NULL || ptr == NULL) return 0U;
  if (size != 0U && nmemb > SIZE_MAX / size) return 0U;
  size_t bytes = size * nmemb;
  if (range_headers_match(ctx) == 0) {
    ctx->invalid_response = 1;
    return 0U;
  }
  if (bytes > ctx->capacity - ctx->received) {
    ctx->invalid_response = 1;
    return 0U;
  }
  if ((ctx->abort_wave != NULL && atomic_load(ctx->abort_wave) != 0) ||
      range_wait_if_paused(ctx) != 0)
    return 0U;
  memcpy(ctx->buffer + ctx->received, ptr, bytes);
  ctx->received += bytes;
  return bytes;
}

static int range_progress_cb(void *opaque, curl_off_t dltotal,
                             curl_off_t dlnow, curl_off_t ultotal,
                             curl_off_t ulnow) {
  range_ctx_t *ctx = (range_ctx_t *)opaque;
  (void)dltotal;
  (void)dlnow;
  (void)ultotal;
  (void)ulnow;
  return atomic_load(&ctx->job->cancel_requested) != 0 ||
         (ctx->abort_wave != NULL && atomic_load(ctx->abort_wave) != 0);
}

static void *range_worker(void *opaque) {
  range_ctx_t *ctx = (range_ctx_t *)opaque;
  CURL *curl = ctx->curl != NULL ? ctx->curl : curl_easy_init();
  int owns_curl = ctx->curl == NULL;
  if (curl == NULL) {
    (void)snprintf(ctx->error, sizeof(ctx->error), "curl_easy_init failed");
    return NULL;
  }
  char error_buffer[CURL_ERROR_SIZE] = {0};
  curl_configure(curl, ctx->job, NULL, NULL, error_buffer, 0U);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, range_write_cb);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);
  (void)curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, range_header_cb);
  (void)curl_easy_setopt(curl, CURLOPT_HEADERDATA, ctx);
  (void)curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, range_progress_cb);
  (void)curl_easy_setopt(curl, CURLOPT_XFERINFODATA, ctx);
  (void)curl_easy_setopt(curl, CURLOPT_RANGE, ctx->range);

  struct curl_slist *headers = NULL;
  if (ctx->validator != NULL) {
    char if_range[sizeof(ctx->headers.etag) + 16U];
    int n = snprintf(if_range, sizeof(if_range), "If-Range: %s",
                     ctx->validator);
    if (n < 0 || (size_t)n >= sizeof(if_range) ||
        (headers = curl_slist_append(NULL, if_range)) == NULL) {
      (void)snprintf(ctx->error, sizeof(ctx->error), "Cannot set If-Range");
      if (owns_curl != 0) curl_easy_cleanup(curl);
      return NULL;
    }
    (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }

  CURLcode code = curl_easy_perform(curl);
  long status = 0L;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  ctx->code = code;
  ctx->ok = code == CURLE_OK && status == 206L &&
            range_headers_match(ctx) != 0 &&
            ctx->received == ctx->capacity;
  if (ctx->ok == 0) {
    if (code == CURLE_OK || (status >= 200L && status < 300L))
      ctx->invalid_response = 1;
    (void)snprintf(ctx->error, sizeof(ctx->error), "%s",
                   error_buffer[0] != '\0' ? error_buffer :
                   curl_easy_strerror(code));
  }
  (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, NULL);
  curl_slist_free_all(headers);
  if (owns_curl != 0) curl_easy_cleanup(curl);
  return NULL;
}

/* A one-byte GET proves that byte ranges work and gives us a strong ETag.
 * Without that validator, use the original single stream so separate
 * requests cannot silently assemble different representations. */
static int range_probe(transfer_job_t *job, uint64_t *total,
                       char *validator, size_t validator_size,
                       CURLcode *out_code, char *out_error,
                       size_t out_error_size) {
  unsigned char byte = 0U;
  range_ctx_t probe = {0};
  probe.job = job;
  probe.buffer = &byte;
  probe.capacity = 1U;
  probe.range[0] = '0';
  probe.range[1] = '-';
  probe.range[2] = '0';
  probe.range[3] = '\0';
  (void)range_worker(&probe);
  if (out_code != NULL) *out_code = probe.code;
  if (out_error != NULL && out_error_size > 0U)
    (void)snprintf(out_error, out_error_size, "%s", probe.error);
  if (probe.ok == 0 || probe.headers.total < CURL_RANGE_MIN_BYTES ||
      probe.headers.total > (uint64_t)INT64_MAX) return -1;
  size_t etag_len = strlen(probe.headers.etag);
  if (etag_len < 2U || probe.headers.etag[0] != '"' ||
      probe.headers.etag[etag_len - 1U] != '"' ||
      etag_len >= validator_size) return -1;
  (void)snprintf(validator, validator_size, "%s", probe.headers.etag);
  *total = probe.headers.total;
  return 0;
}

/* Older .part files have no saved ETag. Before fetching their suffix in
 * parallel, compare small regions of the existing prefix with the current
 * representation. A mismatch must leave the user's partial untouched. */
static int range_verify_prefix(transfer_job_t *job, int fd, uint64_t prefix,
                               uint64_t total, const char *validator,
                               unsigned char *buffer) {
  const size_t sample_size = prefix < 65536U ? (size_t)prefix : 65536U;
  if (sample_size == 0U) return 0;
  const uint64_t last = prefix - sample_size;
  const uint64_t offsets[] = {0U, last / 2U, last};
  for (size_t i = 0U; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
    uint64_t offset = offsets[i];
    if (i > 0U && offset == offsets[i - 1U]) continue;
    size_t read_bytes = 0U;
    while (read_bytes < sample_size) {
      ssize_t n = pread(fd, buffer + 65536U + read_bytes,
                        sample_size - read_bytes, (off_t)(offset + read_bytes));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return 0; /* e.g. write-only partial: use legacy resume */
      read_bytes += (size_t)n;
    }

    range_ctx_t sample = {0};
    sample.job = job;
    sample.buffer = buffer;
    sample.capacity = sample_size;
    sample.first = offset;
    sample.last = offset + sample_size - 1U;
    sample.total = total;
    sample.validator = validator;
    (void)snprintf(sample.range, sizeof(sample.range), "%" PRIu64 "-%" PRIu64,
                   sample.first, sample.last);
    (void)range_worker(&sample);
    if (sample.invalid_response != 0) {
      transfer_job_set_error(job, "Remote file changed during resume");
      return -1;
    }
    if (sample.ok == 0) return 0; /* retain single-stream compatibility */
    if (memcmp(buffer, buffer + 65536U, sample_size) != 0) {
      transfer_job_set_error(job, "Existing partial differs from remote file");
      return -1;
    }
  }
  return 1;
}

/* Each connection fills a bounded memory chunk. Only after every range in a
 * wave has passed its Content-Range and validator checks do we append the
 * chunks in order. The on-disk .part file is therefore always a contiguous
 * prefix and the existing resume logic remains valid after interruption. */
static int curl_perform_parallel(transfer_job_t *job, int fd, uint64_t total,
                                 const char *validator, unsigned char *buffer,
                                 uint64_t committed) {
  CURL *handles[CURL_RANGE_STREAMS] = {NULL};
  int result = -1;
  for (size_t i = 0U; i < CURL_RANGE_STREAMS; i++) {
    handles[i] = curl_easy_init();
    if (handles[i] == NULL) {
      transfer_job_set_error(job, "curl_easy_init failed");
      goto cleanup;
    }
  }
  atomic_store(&job->total_size, total);
  while (committed < total) {
    range_ctx_t ranges[CURL_RANGE_STREAMS];
    pthread_t threads[CURL_RANGE_STREAMS];
    atomic_int abort_wave = ATOMIC_VAR_INIT(0);
    size_t count = 0U;
    uint64_t wave_end = committed;
    for (; count < CURL_RANGE_STREAMS && wave_end < total; count++) {
      range_ctx_t *ctx = &ranges[count];
      memset(ctx, 0, sizeof(*ctx));
      ctx->job = job;
      ctx->curl = handles[count];
      ctx->abort_wave = &abort_wave;
      ctx->buffer = buffer + count * CURL_RANGE_CHUNK_BYTES;
      ctx->capacity = (size_t)((total - wave_end) < CURL_RANGE_CHUNK_BYTES
                                   ? (total - wave_end) : CURL_RANGE_CHUNK_BYTES);
      ctx->first = wave_end;
      ctx->last = wave_end + ctx->capacity - 1U;
      ctx->total = total;
      ctx->validator = validator;
      (void)snprintf(ctx->range, sizeof(ctx->range), "%" PRIu64 "-%" PRIu64,
                     ctx->first, ctx->last);
      wave_end = ctx->last + 1U;
    }

    size_t started = 0U;
    for (; started < count; started++) {
      if (pthread_create(&threads[started], NULL, range_worker,
                         &ranges[started]) != 0) {
        atomic_store(&abort_wave, 1);
        break;
      }
    }
    for (size_t i = 0U; i < started; i++)
      (void)pthread_join(threads[i], NULL);
    if (started != count) {
      transfer_job_set_error(job, "Cannot start parallel download threads");
      goto cleanup;
    }
    size_t failed = count;
    int invalid = 0;
    for (size_t i = 0U; i < count; i++) {
      if (ranges[i].ok == 0 && failed == count) failed = i;
      if (ranges[i].invalid_response != 0) invalid = 1;
    }
    if (failed != count) {
      if (atomic_load(&job->cancel_requested) != 0) {
        transfer_job_set_error(job, "Cancelled by user");
        goto cleanup;
      }
      if (invalid != 0) {
        if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) {
          transfer_job_set_error(job, "Cannot discard changed download: %s",
                                 strerror(errno));
          goto cleanup;
        }
        atomic_store(&job->downloaded, 0U);
      }
      transfer_job_set_error(job, "Range download failed: %s",
                             invalid != 0 ? "remote file changed or range invalid" :
                             (ranges[failed].error[0] != '\0' ?
                              ranges[failed].error : "network error"));
      goto cleanup;
    }
    for (size_t i = 0U; i < count; i++) {
      if (transfer_job_wait_if_paused(job) != 0) {
        transfer_job_set_error(job, "Cancelled by user");
        goto cleanup;
      }
      if (transfer_write_all(fd, ranges[i].buffer, ranges[i].capacity) != 0) {
        transfer_job_set_error(job, "Cannot write download: %s", strerror(errno));
        goto cleanup;
      }
      committed += ranges[i].capacity;
      atomic_store(&job->downloaded, committed);
    }
  }
  result = 0;
cleanup:
  for (size_t i = 0U; i < CURL_RANGE_STREAMS; i++)
    if (handles[i] != NULL) curl_easy_cleanup(handles[i]);
  return result;
}

static int curl_perform_once(transfer_job_t *job, int fd, uint64_t from,
                             CURLcode *out_code, long *out_status) {
  CURL *curl = curl_easy_init();
  if (curl == NULL) {
    transfer_job_set_error(job, "curl_easy_init failed");
    return -1;
  }

  char error_buffer[CURL_ERROR_SIZE];
  error_buffer[0] = '\0';
  curl_write_ctx_t wctx = {job, fd};
  curl_progress_ctx_t pctx = {job, from};
  curl_configure(curl, job, &wctx, &pctx, error_buffer, from);

  CURLcode code = curl_easy_perform(curl);
  long status = 0L;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

#if LIBCURL_VERSION_NUM >= 0x073700
  curl_off_t total = 0;
  if (curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total) ==
          CURLE_OK &&
      total > 0) {
    atomic_store(&job->total_size, from + (uint64_t)total);
  }
#endif

  curl_easy_cleanup(curl);
  if (out_code != NULL) *out_code = code;
  if (out_status != NULL) *out_status = status;

  if (code == CURLE_OK) return 0;
  curl_set_transfer_error(job, code, error_buffer, status);
  return -1;
}

typedef struct {
  range_ctx_t range;
  curl_write_ctx_t file;
} validated_write_ctx_t;

static size_t validated_write_cb(void *ptr, size_t size, size_t nmemb,
                                  void *opaque) {
  validated_write_ctx_t *ctx = (validated_write_ctx_t *)opaque;
  if (range_headers_match(&ctx->range) == 0) {
    ctx->range.invalid_response = 1;
    return 0U;
  }
  return curl_write_cb(ptr, size, nmemb, &ctx->file);
}

/* Continue a fast single connection from the sampled prefix. Validate the
 * same ETag and exact remaining range before allowing any bytes onto disk. */
static int curl_perform_validated_remainder(transfer_job_t *job, int fd,
                                            uint64_t from, uint64_t total,
                                            const char *validator) {
  CURL *curl = curl_easy_init();
  if (curl == NULL) {
    transfer_job_set_error(job, "curl_easy_init failed");
    return -1;
  }
  char error_buffer[CURL_ERROR_SIZE] = {0};
  curl_progress_ctx_t pctx = {job, from};
  validated_write_ctx_t wctx = {0};
  wctx.range.job = job;
  wctx.range.first = from;
  wctx.range.last = total - 1U;
  wctx.range.total = total;
  wctx.range.validator = validator;
  wctx.file.job = job;
  wctx.file.fd = fd;
  curl_configure(curl, job, &wctx.file, &pctx, error_buffer, 0U);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, validated_write_cb);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);
  (void)curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, range_header_cb);
  (void)curl_easy_setopt(curl, CURLOPT_HEADERDATA, &wctx.range);
  char range[64];
  (void)snprintf(range, sizeof(range), "%" PRIu64 "-%" PRIu64,
                 from, total - 1U);
  (void)curl_easy_setopt(curl, CURLOPT_RANGE, range);
  char if_range[160];
  (void)snprintf(if_range, sizeof(if_range), "If-Range: %s", validator);
  struct curl_slist *headers = curl_slist_append(NULL, if_range);
  if (headers == NULL) {
    curl_easy_cleanup(curl);
    transfer_job_set_error(job, "Cannot set If-Range");
    return -1;
  }
  (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  CURLcode code = curl_easy_perform(curl);
  long status = 0L;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (atomic_load(&job->cancel_requested) != 0) {
    transfer_job_set_error(job, "Cancelled by user");
    return -1;
  }
  if (wctx.range.invalid_response != 0 ||
      (status >= 200L && status < 300L &&
       range_headers_match(&wctx.range) == 0)) {
    if (ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) >= 0)
      atomic_store(&job->downloaded, 0U);
    transfer_job_set_error(job, "Remote file changed during download");
    return -1;
  }
  if (code != CURLE_OK) {
    curl_set_transfer_error(job, code, error_buffer, status);
    return -1;
  }
  struct stat st;
  if (status != 206L || fstat(fd, &st) != 0 ||
      st.st_size < 0 || (uint64_t)st.st_size != total) {
    transfer_job_set_error(job, "Incomplete range download");
    return -1;
  }
  atomic_store(&job->downloaded, total);
  atomic_store(&job->total_size, total);
  return 0;
}

int transfer_backend_curl_run(transfer_job_t *job, int fd) {
  if (job == NULL || fd < 0) return -1;
  (void)pthread_once(&g_curl_once, curl_global_init_once);
  if (g_curl_init_result != CURLE_OK) {
    transfer_job_set_error(job, "curl_global_init failed: %s",
                           curl_easy_strerror(g_curl_init_result));
    return -1;
  }

  int is_http = strncasecmp(job->url, "http://", 7U) == 0 ||
                strncasecmp(job->url, "https://", 8U) == 0;
  if (job->resume_from > 0U && is_http != 0) {
    uint64_t total = 0U;
    char validator[128];
    CURLcode probe_code = CURLE_OK;
    char probe_error[160] = {0};
    int probed = range_probe(job, &total, validator, sizeof(validator),
                             &probe_code, probe_error,
                             sizeof(probe_error)) == 0;
    if (probed == 0 && curl_code_is_connect_failure(probe_code) != 0) {
      /* The host is unreachable: the single-stream attempt would spend the
       * whole connect timeout again only to fail the same way. */
      curl_set_transfer_error(job, probe_code, probe_error, 0L);
      return -1;
    }
    if (probed != 0 && job->resume_from < total) {
      unsigned char *buffer =
          malloc(CURL_RANGE_STREAMS * CURL_RANGE_CHUNK_BYTES);
      if (buffer != NULL) {
        int verified = range_verify_prefix(job, fd, job->resume_from, total,
                                           validator, buffer);
        if (verified < 0) {
          free(buffer);
          return -1;
        }
        if (verified > 0) {
          int rc = curl_perform_parallel(job, fd, total, validator, buffer,
                                         job->resume_from);
          free(buffer);
          return rc;
        }
        free(buffer);
      }
    }
    if (atomic_load(&job->cancel_requested) != 0) {
      transfer_job_set_error(job, "Cancelled by user");
      return -1;
    }
  }

  if (job->resume_from == 0U && is_http != 0) {
    uint64_t total = 0U;
    char validator[128];
    CURLcode probe_code = CURLE_OK;
    char probe_error[160] = {0};
    int probed = range_probe(job, &total, validator, sizeof(validator),
                             &probe_code, probe_error,
                             sizeof(probe_error)) == 0;
    if (probed == 0 && curl_code_is_connect_failure(probe_code) != 0) {
      curl_set_transfer_error(job, probe_code, probe_error, 0L);
      return -1;
    }
    if (probed != 0) {
      unsigned char *buffer =
          malloc(CURL_RANGE_STREAMS * CURL_RANGE_CHUNK_BYTES);
      if (buffer != NULL) {
        range_ctx_t sample = {0};
        sample.job = job;
        sample.buffer = buffer;
        sample.capacity = CURL_RANGE_STREAMS * CURL_RANGE_CHUNK_BYTES;
        sample.first = 0U;
        sample.last = sample.capacity - 1U;
        sample.total = total;
        sample.validator = validator;
        (void)snprintf(sample.range, sizeof(sample.range), "0-%zu",
                       sample.capacity - 1U);
        struct timespec start, end;
        int timed = clock_gettime(CLOCK_MONOTONIC, &start) == 0;
        (void)range_worker(&sample);
        if (timed != 0) timed = clock_gettime(CLOCK_MONOTONIC, &end) == 0;
        if (sample.ok == 0) {
          free(buffer);
          if (atomic_load(&job->cancel_requested) != 0) {
            transfer_job_set_error(job, "Cancelled by user");
            return -1;
          }
          if (sample.invalid_response != 0) {
            transfer_job_set_error(job, "Remote file changed during download");
            return -1;
          }
        } else {
          if (transfer_job_wait_if_paused(job) != 0) {
            transfer_job_set_error(job, "Cancelled by user");
            free(buffer);
            return -1;
          }
          if (transfer_write_all(fd, buffer, sample.capacity) != 0) {
            transfer_job_set_error(job, "Cannot write download: %s",
                                   strerror(errno));
            free(buffer);
            return -1;
          }
          atomic_store(&job->downloaded, sample.capacity);
          atomic_store(&job->total_size, total);
          double seconds = timed != 0
                               ? (double)(end.tv_sec - start.tv_sec) +
                                     (double)(end.tv_nsec - start.tv_nsec) / 1e9
                               : 0.0;
          int rc = seconds > 0.0 &&
                           (double)sample.capacity / seconds >=
                               CURL_PARALLEL_THRESHOLD_BYTES_PER_SEC
                       ? curl_perform_validated_remainder(
                             job, fd, sample.capacity, total, validator)
                       : curl_perform_parallel(job, fd, total, validator,
                                               buffer, sample.capacity);
          free(buffer);
          return rc;
        }
      }
    }
    if (atomic_load(&job->cancel_requested) != 0) {
      transfer_job_set_error(job, "Cancelled by user");
      return -1;
    }
  }

  CURLcode code = CURLE_OK;
  long status = 0L;
  if (curl_perform_once(job, fd, job->resume_from, &code, &status) == 0) return 0;

  if (job->resume_from > 0U && atomic_load(&job->cancel_requested) == 0 &&
      (code == CURLE_RANGE_ERROR || status == 416L)) {
    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) {
      transfer_job_set_error(job, "Cannot restart non-resumable download: %s",
                             strerror(errno));
      return -1;
    }
    job->resume_from = 0U;
    job->error_msg[0] = '\0';
    atomic_store(&job->error, 0);
    atomic_store(&job->downloaded, 0U);
    atomic_store(&job->total_size, 0U);
    return curl_perform_once(job, fd, 0U, &code, &status);
  }
  return -1;
}
