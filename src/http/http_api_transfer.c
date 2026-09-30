/* URL transfer REST endpoints; protocol work lives in src/transfer/. */
#include "http_api_internal.h"
#include "transfer/transfer_manager.h"
#include "ftp_config.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*===========================================================================*
 * DOWNLOAD / TRANSFER API
 *
 * Transport logic lives in src/transfer/.  The HTTP API is intentionally
 * limited to request validation, destination confinement and JSON mapping.
 *===========================================================================*/

static int dl_is_directory_path(const char *path) {
  struct stat st;
  return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int dl_try_destination(const char *candidate, char *safe,
                              size_t safe_size) {
  if (candidate == NULL || candidate[0] == '\0') return 0;
  if (!http_api_validate_path(candidate, safe, safe_size)) return 0;
  return dl_is_directory_path(safe);
}

static int dl_normalize_destination(const char *requested, char *safe,
                                    size_t safe_size) {
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (requested == NULL || requested[0] == '\0' || strcmp(requested, "/") == 0) {
    static const char *const defaults[] = {"/data", "/mnt/ext1", "/mnt/usb0",
                                            "/mnt/usb1", NULL};
    for (size_t i = 0U; defaults[i] != NULL; i++) {
      if (dl_try_destination(defaults[i], safe, safe_size)) return 1;
    }
  }
#endif
  if (dl_try_destination(requested, safe, safe_size)) return 1;
  if (http_api_get_root()[0] != '\0' && strcmp(http_api_get_root(), "/") != 0 &&
      dl_try_destination(http_api_get_root(), safe, safe_size)) return 1;
  return 0;
}

static int json_body_string(const http_request_t *request, const char *key,
                            char *out, size_t out_size) {
  if (request == NULL || key == NULL || out == NULL || out_size == 0U ||
      request->body == NULL || request->body_length == 0U) return 0;
  out[0] = '\0';
  char needle[64];
  int nn = snprintf(needle, sizeof(needle), "\"%s\"", key);
  if (nn <= 0 || (size_t)nn >= sizeof(needle)) return 0;
  const char *p = strstr(request->body, needle);
  if (p == NULL) return 0;
  p = strchr(p + (size_t)nn, ':');
  if (p == NULL) return 0;
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p++ != '"') return 0;
  size_t pos = 0U;
  while (*p != '\0' && *p != '"' && pos + 1U < out_size) {
    if (*p == '\\' && p[1] != '\0') {
      p++;
      if (*p == 'n') out[pos++] = '\n';
      else if (*p == 'r') out[pos++] = '\r';
      else if (*p == 't') out[pos++] = '\t';
      else out[pos++] = *p;
      p++;
      continue;
    }
    out[pos++] = *p++;
  }
  out[pos] = '\0';
  return *p == '"';
}

static int json_body_int(const http_request_t *request, const char *key,
                         int *out) {
  if (request == NULL || key == NULL || out == NULL || request->body == NULL)
    return 0;
  char needle[64];
  int nn = snprintf(needle, sizeof(needle), "\"%s\"", key);
  if (nn <= 0 || (size_t)nn >= sizeof(needle)) return 0;
  const char *p = strstr(request->body, needle);
  if (p == NULL || (p = strchr(p + (size_t)nn, ':')) == NULL) return 0;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  char *endptr = NULL;
  long value = strtol(p, &endptr, 10);
  if (endptr == p || value <= 0L || value > INT_MAX) return 0;
  *out = (int)value;
  return 1;
}

http_response_t *http_api_transfer_start(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");

  char url[TRANSFER_URL_MAX];
  char dst[TRANSFER_PATH_MAX];
  if (!json_body_string(request, "url", url, sizeof(url)) || url[0] == '\0')
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing url parameter");
  if (!json_body_string(request, "dst", dst, sizeof(dst))) dst[0] = '\0';

  char reason[TRANSFER_ERROR_MAX];
  if (!transfer_url_supported(url, reason, sizeof(reason)))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, reason);

  char safe_dst[FTP_PATH_MAX];
  if (!dl_normalize_destination(dst, safe_dst, sizeof(safe_dst)))
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN,
                      "Invalid or read-only destination path");

  int id = 0;
  int queued = 0;
  char name[TRANSFER_NAME_MAX];
  char error[TRANSFER_ERROR_MAX];
  if (transfer_start(url, safe_dst, &id, &queued, name, sizeof(name), error,
                     sizeof(error)) != 0)
    return http_api_error_json(HTTP_STATUS_409_CONFLICT,
                      error[0] != '\0' ? error : "Failed to start transfer");

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  char esc_name[TRANSFER_NAME_MAX * 2U];
  size_t esc_pos = 0U;
  esc_name[0] = '\0';
  (void)http_json_escape_append(esc_name, sizeof(esc_name), &esc_pos, name);
  esc_name[(esc_pos < sizeof(esc_name)) ? esc_pos : sizeof(esc_name) - 1U] = '\0';
  char body[768];
  int len = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"id\":%d,\"name\":\"%s\",\"size\":0,"
                     "\"queued\":%s}",
                     id, esc_name, queued ? "true" : "false");
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

http_response_t *http_api_transfer_status(const http_request_t *request) {
  (void)request;
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");

  /* Jobs carry full URLs, paths and error strings: with TRANSFER_MAX_ACTIVE
   * entries the snapshot array and the JSON body are tens of KB, so they are
   * heap allocated instead of sitting on the HTTP thread stack. */
  const size_t cap =
      512U + (size_t)TRANSFER_MAX_ACTIVE *
                 (2U * (TRANSFER_URL_MAX + TRANSFER_NAME_MAX +
                        TRANSFER_ERROR_MAX + TRANSFER_PATH_MAX) +
                  256U);
  transfer_snapshot_t *snaps = calloc(TRANSFER_MAX_ACTIVE, sizeof(*snaps));
  char *body = malloc(cap);
  if ((snaps == NULL) || (body == NULL)) {
    free(snaps);
    free(body);
    http_response_destroy(resp);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  char *esc = malloc(2U * (TRANSFER_URL_MAX + TRANSFER_NAME_MAX +
                           TRANSFER_ERROR_MAX + TRANSFER_PATH_MAX));
  if (esc == NULL) {
    free(snaps);
    free(body);
    http_response_destroy(resp);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }
  const size_t esc_url_size = 2U * TRANSFER_URL_MAX;
  const size_t esc_name_size = 2U * TRANSFER_NAME_MAX;
  const size_t esc_error_size = 2U * TRANSFER_ERROR_MAX;
  const size_t esc_dst_size = 2U * TRANSFER_PATH_MAX;
  char *esc_url = esc;
  char *esc_name = esc + esc_url_size;
  char *esc_error = esc_name + esc_name_size;
  char *esc_dst = esc_error + esc_error_size;

  size_t count = transfer_snapshot_all(snaps, TRANSFER_MAX_ACTIVE);
  size_t pos = 0U;
  int n = snprintf(body, cap, "{\"downloads\":[");
  if (n < 0) goto fail;
  pos = (size_t)n;
  for (size_t i = 0U; i < count; i++) {
    transfer_snapshot_t *s = &snaps[i];
    size_t ep = 0U;
    esc_name[0] = esc_url[0] = esc_error[0] = esc_dst[0] = '\0';
    (void)http_json_escape_append(esc_name, esc_name_size, &ep, s->filename);
    esc_name[ep < esc_name_size ? ep : esc_name_size - 1U] = '\0';
    ep = 0U;
    (void)http_json_escape_append(esc_url, esc_url_size, &ep, s->url);
    esc_url[ep < esc_url_size ? ep : esc_url_size - 1U] = '\0';
    ep = 0U;
    (void)http_json_escape_append(esc_error, esc_error_size, &ep,
                                  s->error ? s->error_msg : "");
    esc_error[ep < esc_error_size ? ep : esc_error_size - 1U] =
        '\0';
    ep = 0U;
    (void)http_json_escape_append(esc_dst, esc_dst_size, &ep, s->dst_path);
    esc_dst[ep < esc_dst_size ? ep : esc_dst_size - 1U] = '\0';
    unsigned progress = s->total_size > 0U
        ? (unsigned)((s->downloaded * 100U) / s->total_size)
        : (s->done && !s->error ? 100U : 0U);
    if (progress > 100U) progress = 100U;
    n = snprintf(body + pos, cap - pos,
        "%s{\"id\":%d,\"name\":\"%s\",\"url\":\"%s\",\"dst\":\"%s\",\"progress\":%u,"
        "\"downloaded\":%" PRIu64 ",\"total_size\":%" PRIu64 ",\"speed\":%.0f,"
        "\"done\":%s,\"error\":\"%s\",\"paused\":%s,\"queued\":%s,"
        "\"queue_position\":%d,\"cancelled\":%s}",
        i == 0U ? "" : ",", s->id, esc_name, esc_url, esc_dst, progress,
        s->downloaded, s->total_size, s->speed, s->done ? "true" : "false",
        esc_error, s->paused ? "true" : "false", s->queued ? "true" : "false",
        s->queue_position, s->cancelled ? "true" : "false");
    if (n < 0 || (size_t)n >= cap - pos) goto fail;
    pos += (size_t)n;
  }
  n = snprintf(body + pos, cap - pos, "]}");
  if (n < 0 || (size_t)n >= cap - pos) goto fail;
  pos += (size_t)n;

  http_response_set_body(resp, body, pos);
  free(esc);
  free(snaps);
  free(body);
  return resp;

fail:
  free(esc);
  free(snaps);
  free(body);
  http_response_destroy(resp);
  return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                             "Transfer status response too large");
}

http_response_t *http_api_transfer_pause(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  int id = 0, paused = 0;
  if (!json_body_int(request, "id", &id))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing download id");
  if (transfer_toggle_pause(id, &paused) != 0)
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Transfer not found");
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  char body[64];
  int len = snprintf(body, sizeof(body), "{\"ok\":true,\"paused\":%s}",
                     paused ? "true" : "false");
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

http_response_t *http_api_transfer_cancel(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  int id = 0;
  if (!json_body_int(request, "id", &id))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing download id");
  if (transfer_cancel(id) != 0)
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Transfer not found");
  return http_api_status_json_200(1, "Transfer cancellation requested", id);
}

static http_response_t *http_api_transfer_retry(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  int id = 0;
  if (!json_body_int(request, "id", &id))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing download id");
  if (transfer_retry(id) != 0)
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Failed download not found");
  return http_api_status_json_200(1, "Transfer queued for retry", id);
}

static http_response_t *http_api_transfer_delete(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  int id = 0;
  if (!json_body_int(request, "id", &id))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing download id");
  if (transfer_delete(id) != 0)
    return http_api_error_json(HTTP_STATUS_409_CONFLICT, "Could not delete download");
  return http_api_status_json_200(1, "Download removed", id);
}

static http_response_t *http_api_transfer_orphans(const http_request_t *request) {
  if (request->method != HTTP_METHOD_GET)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use GET");
  transfer_orphan_t *items = calloc(TRANSFER_ORPHAN_MAX, sizeof(*items));
  const size_t cap = 128U + TRANSFER_ORPHAN_MAX *
                            (2U * TRANSFER_PART_PATH_MAX + 80U);
  char *body = malloc(cap);
  char *escaped = malloc(2U * TRANSFER_PART_PATH_MAX);
  if (items == NULL || body == NULL || escaped == NULL) {
    free(items); free(body); free(escaped);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }
  int scanning = 0;
  size_t count = transfer_orphans_snapshot(items, TRANSFER_ORPHAN_MAX,
                                           &scanning);
  size_t pos = 0U;
  int n = snprintf(body, cap, "{\"scanning\":%s,\"orphans\":[",
                   scanning ? "true" : "false");
  if (n < 0 || (size_t)n >= cap) goto fail_orphans;
  pos = (size_t)n;
  for (size_t i = 0U; i < count; i++) {
    size_t ep = 0U;
    escaped[0] = '\0';
    (void)http_json_escape_append(escaped, 2U * TRANSFER_PART_PATH_MAX,
                                  &ep, items[i].path);
    escaped[ep < 2U * TRANSFER_PART_PATH_MAX ? ep :
            2U * TRANSFER_PART_PATH_MAX - 1U] = '\0';
    n = snprintf(body + pos, cap - pos,
                 "%s{\"path\":\"%s\",\"size\":%" PRIu64 "}",
                 i == 0U ? "" : ",", escaped, items[i].size);
    if (n < 0 || (size_t)n >= cap - pos) goto fail_orphans;
    pos += (size_t)n;
  }
  n = snprintf(body + pos, cap - pos, "]}");
  if (n < 0 || (size_t)n >= cap - pos) goto fail_orphans;
  pos += (size_t)n;
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp != NULL) {
    http_response_add_header(resp, "Content-Type", "application/json");
    http_response_add_header(resp, "Cache-Control", "no-store");
    http_response_set_body(resp, body, pos);
  }
  free(items); free(body); free(escaped);
  return resp;

fail_orphans:
  free(items); free(body); free(escaped);
  return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                             "Orphan list response too large");
}

static http_response_t *http_api_transfer_orphan_delete(
    const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  char path[TRANSFER_PART_PATH_MAX];
  if (!json_body_string(request, "path", path, sizeof(path)))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing path");
  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe)) ||
      strcmp(path, safe) != 0)
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Invalid path");
  if (transfer_orphan_delete(path) != 0)
    return http_api_error_json(HTTP_STATUS_409_CONFLICT,
                               "Partial file is no longer an orphan");
  return http_api_status_json_200(1, "Partial file deleted", 0);
}


http_response_t *http_api_transfer_handle(const http_request_t *request) {
  if (request == NULL) return NULL;
  if (http_api_route_is(request->uri, "/api/download/start")) return http_api_transfer_start(request);
  if (http_api_route_is(request->uri, "/api/download/status")) return http_api_transfer_status(request);
  if (http_api_route_is(request->uri, "/api/download/pause")) return http_api_transfer_pause(request);
  if (http_api_route_is(request->uri, "/api/download/cancel")) return http_api_transfer_cancel(request);
  if (http_api_route_is(request->uri, "/api/download/retry")) return http_api_transfer_retry(request);
  if (http_api_route_is(request->uri, "/api/download/delete")) return http_api_transfer_delete(request);
  if (http_api_route_is(request->uri, "/api/download/orphans")) return http_api_transfer_orphans(request);
  if (http_api_route_is(request->uri, "/api/download/orphan/delete")) return http_api_transfer_orphan_delete(request);
  return NULL;
}
