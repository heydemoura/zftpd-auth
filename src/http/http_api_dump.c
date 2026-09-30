/* HTTP API domain: decrypted game dumps (see transfer/dump_job.h).
 *
 * A dump only ever reads the running title's sandbox mount, so starting one
 * means: launch the title, wait for the mount, dump.  Nothing here touches
 * /user/app, whose content is the encrypted image.
 */
#include "http_api.h"
#include "http_api_internal.h"
#include "ftp_config.h"
#include "pal_limits.h"
#include "pal_notification.h"
#include "transfer/dump_job.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Launch handler from the games admin domain: reused as-is so the payload does
 * not grow a second copy of the sceLncUtil plumbing. */
http_response_t *http_games_launch(const http_request_t *request);

typedef struct {
  dump_job_t *job;
  int id;
  int in_use;
  int worker_active; /**< Local writer still owns job/slot memory. */
  int to_console;
  int format;
  int decrypt;
  int cancel_requested; /**< set by POST /api/dump/cancel */
  int state;
  char title_id[32];
  char dest[DUMP_PATH_MAX];
  char source[DUMP_PATH_MAX]; /**< resolved once the job exists */
  char error[320];
  uint64_t bytes;
  uint64_t bytes_done;
  size_t entries;
  time_t started;
  time_t state_since; /**< when the current state was entered (staleness) */
} dump_slot_t;

/* A dump that never advances must not block the next one: each state has a
 * deadline after which the slot is retired (and reported as failed). */
#define DUMP_LAUNCH_DEADLINE_S 150
#define DUMP_READY_DEADLINE_S 20  /* the client never fetched the archive  */
#define DUMP_RUN_DEADLINE_S 21600 /* six hours: a stuck local copy         */

#define DUMP_STATE_IDLE 0
#define DUMP_STATE_LAUNCHING 1 /* title started, waiting for the sandbox  */
#define DUMP_STATE_RUNNING 2   /* local dump in progress                  */
#define DUMP_STATE_READY 3     /* streamed dump prepared for download     */
#define DUMP_STATE_DONE 4
#define DUMP_STATE_FAILED 5

#define DUMP_WAIT_STEPS 120 /* * 500 ms = one minute to mount */
#define DUMP_WAIT_STEP_NS 500000000L

static dump_slot_t g_dump;
static pthread_mutex_t g_dump_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_dump_next_id = 1;

static const char *dump_state_name(int state) {
  switch (state) {
  case DUMP_STATE_LAUNCHING: return "launching";
  case DUMP_STATE_RUNNING: return "running";
  case DUMP_STATE_READY: return "ready";
  case DUMP_STATE_DONE: return "done";
  case DUMP_STATE_FAILED: return "failed";
  default: return "idle";
  }
}

/** Move the slot to @p state (caller holds the lock). */
static void dump_set_state(dump_slot_t *slot, int state) {
  slot->state = state;
  slot->state_since = time(NULL);
}

static void dump_fail(dump_slot_t *slot, const char *message);

/**
 * Retire a slot that can no longer make progress: a terminal state, or a state
 * that outlived its deadline.  Caller holds the lock.
 */
static void dump_reap_if_stale(dump_slot_t *slot) {
  if (slot->in_use == 0) return;

  if (slot->state == DUMP_STATE_DONE || slot->state == DUMP_STATE_FAILED) {
    return; /* already finished: the next start simply reuses the slot */
  }

  time_t age = time(NULL) - slot->state_since;
  const char *reason = NULL;
  if (slot->state == DUMP_STATE_LAUNCHING && age > DUMP_LAUNCH_DEADLINE_S) {
    reason = "The title never started: the launch did not mount a sandbox";
  } else if (slot->state == DUMP_STATE_READY && age > DUMP_READY_DEADLINE_S) {
    reason = "The download never started, the prepared dump was discarded";
  } else if (slot->state == DUMP_STATE_RUNNING && age > DUMP_RUN_DEADLINE_S) {
    reason = "The dump stalled and was abandoned";
  }
  if (reason == NULL) return;

  /* A local writer may still be inside dump_job_write_local().  Do not free
   * its job or recycle the slot until the worker has actually returned. */
  if (!slot->worker_active) {
    dump_job_destroy(slot->job);
    slot->job = NULL;
  }
  dump_fail(slot, reason);
}

/** {"key":"value"} reader (the dump body is small and flat by design). */
static int json_string(const char *body, const char *key, char *out,
                       size_t out_size) {
  if (body == NULL || key == NULL || out == NULL || out_size == 0U) return -1;
  out[0] = '\0';

  char pattern[64];
  (void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(body, pattern);
  if (p == NULL) return -1;
  const char *colon = strchr(p + strlen(pattern), ':');
  if (colon == NULL) return -1;
  const char *quote = strchr(colon, '"');
  if (quote == NULL) return -1;
  const char *end = strchr(quote + 1, '"');
  if (end == NULL) return -1;

  size_t len = (size_t)(end - quote - 1);
  if (len >= out_size) len = out_size - 1U;
  memcpy(out, quote + 1, len);
  out[len] = '\0';
  return 0;
}

/** {"key":1} / {"key":true} reader; returns 0 when the key is absent. */
static int json_int(const char *body, const char *key, int *out) {
  if (body == NULL || key == NULL || out == NULL) return 0;

  char pattern[64];
  (void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(body, pattern);
  if (p == NULL) return 0;
  const char *colon = strchr(p + strlen(pattern), ':');
  if (colon == NULL) return 0;

  const char *value = colon + 1;
  while (*value == ' ' || *value == '\t') value++;
  if (strncmp(value, "true", 4) == 0) {
    *out = 1;
    return 1;
  }
  if (strncmp(value, "false", 5) == 0) {
    *out = 0;
    return 1;
  }
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  if (end == value) return 0;
  *out = (int)parsed;
  return 1;
}

/** Ask the console to start the title, exactly like the Games view does. */
static int dump_launch_title(const char *title_id, char *error,
                             size_t error_size) {
  http_request_t request;
  memset(&request, 0, sizeof(request));
  request.method = HTTP_METHOD_POST;
  (void)snprintf(request.uri, sizeof(request.uri), "/api/admin/launch?id=%s",
                 title_id);

  http_response_t *resp = http_games_launch(&request);
  if (resp == NULL) {
    (void)snprintf(error, error_size, "Could not request launch of %s", title_id);
    return -1;
  }

  /* 200 with "status":"ok" means the launch API accepted the request. */
  int ok = (strstr(resp->data, "\"ok\":true") != NULL ||
            strstr(resp->data, "\"status\": \"ok\"") != NULL ||
            strstr(resp->data, "\"status\":\"ok\"") != NULL)
               ? 0
               : -1;
  if (ok != 0) {
    char detail[192] = {0};
    if (json_string(resp->data, "message", detail, sizeof(detail)) == 0 &&
        detail[0] != '\0') {
      (void)snprintf(error, error_size, "Game did not start: %s", detail);
    } else {
      (void)snprintf(error, error_size, "Game %s did not start", title_id);
    }
  }
  http_response_destroy(resp);
  return ok;
}

/** Launch when needed and wait for the sandbox to appear. */
static int dump_await_sandbox(dump_slot_t *slot, char *error,
                              size_t error_size) {
  if (!dump_source_ready(slot->title_id)) {
    if (dump_launch_title(slot->title_id, error, error_size) != 0) return -1;
  }

  for (int step = 0; step < DUMP_WAIT_STEPS; step++) {
    if (dump_source_ready(slot->title_id)) return 0;

    pthread_mutex_lock(&g_dump_lock);
    int cancelled = slot->cancel_requested;
    pthread_mutex_unlock(&g_dump_lock);
    if (cancelled) {
      (void)snprintf(error, error_size, "Cancelled");
      return -1;
    }

    struct timespec ts = {0, DUMP_WAIT_STEP_NS};
    (void)nanosleep(&ts, NULL);
  }
  (void)snprintf(error, error_size,
                 "Game %s did not start or its sandbox did not mount within 60 seconds",
                 slot->title_id);
  return -1;
}

static void dump_fail(dump_slot_t *slot, const char *message) {
  dump_set_state(slot, DUMP_STATE_FAILED);
  (void)snprintf(slot->error, sizeof(slot->error), "%s", message);
}

static void dump_local_progress(uint64_t bytes, void *ctx) {
  dump_slot_t *slot = (dump_slot_t *)ctx;
  pthread_mutex_lock(&g_dump_lock);
  if (slot->state == DUMP_STATE_RUNNING) slot->bytes_done = bytes;
  pthread_mutex_unlock(&g_dump_lock);
}

static void *dump_worker(void *opaque) {
  dump_slot_t *slot = (dump_slot_t *)opaque;
  int worker_id = slot->id;

  char launch_error[320] = {0};
  if (dump_await_sandbox(slot, launch_error, sizeof(launch_error)) != 0) {
    pthread_mutex_lock(&g_dump_lock);
    dump_fail(slot, launch_error);
    slot->worker_active = 0;
    pthread_mutex_unlock(&g_dump_lock);
    return NULL;
  }

  pthread_mutex_lock(&g_dump_lock);
  if (slot->cancel_requested) {
    dump_fail(slot, "Cancelled");
    slot->worker_active = 0;
    pthread_mutex_unlock(&g_dump_lock);
    return NULL;
  }

  slot->job = dump_job_create(slot->title_id, (dump_format_t)slot->format,
                              slot->decrypt);
  if (slot->job == NULL) {
    char detail[DUMP_PATH_MAX];
    (void)dump_resolve_source(slot->title_id, detail, sizeof(detail));
    dump_fail(slot, detail);
    slot->worker_active = 0;
    pthread_mutex_unlock(&g_dump_lock);
    return NULL;
  }

  if (slot->cancel_requested) {
    /* The cancellation arrived while the source tree was being walked. */
    dump_job_destroy(slot->job);
    slot->job = NULL;
    dump_fail(slot, "Cancelled");
    slot->worker_active = 0;
    pthread_mutex_unlock(&g_dump_lock);
    return NULL;
  }

  slot->bytes = dump_job_size(slot->job);
  slot->entries = dump_job_entries(slot->job);
  (void)snprintf(slot->source, sizeof(slot->source), "%s",
                 dump_job_source(slot->job));
  if (slot->format == DUMP_FORMAT_ZIP &&
      (slot->entries == 0U || dump_job_truncated(slot->job) != 0)) {
    dump_job_destroy(slot->job);
    slot->job = NULL;
    dump_fail(slot, "Dump could not include every file (empty source or entry limit reached)");
    slot->worker_active = 0;
    pthread_mutex_unlock(&g_dump_lock);
    return NULL;
  }

  if (slot->to_console == 0) {
    /* Streaming: the client now downloads /api/dump?id=<id>; if it does not,
     * the slot is retired by its deadline. */
    dump_set_state(slot, DUMP_STATE_READY);
    slot->worker_active = 0;
    pthread_mutex_unlock(&g_dump_lock);
    return NULL;
  }

  dump_set_state(slot, DUMP_STATE_RUNNING);
  pthread_mutex_unlock(&g_dump_lock);

  char error[256] = {0};
  int rc = dump_job_write_local(slot->job, slot->dest, error, sizeof(error),
                                dump_local_progress, slot);

  char notice[96] = {0};
  pthread_mutex_lock(&g_dump_lock);
  if (slot->id != worker_id || slot->state != DUMP_STATE_RUNNING) {
    if (slot->id == worker_id) {
      dump_job_destroy(slot->job);
      slot->job = NULL;
      slot->worker_active = 0;
    }
    pthread_mutex_unlock(&g_dump_lock);
    return NULL;
  }
  if (rc == 0) {
    slot->bytes_done = slot->bytes;
    dump_set_state(slot, DUMP_STATE_DONE);
    (void)snprintf(notice, sizeof(notice), "zftpd: dump %s completed on console",
                   slot->title_id);
  } else {
    dump_fail(slot, error[0] != '\0' ? error : "Dump failed");
  }
  slot->worker_active = 0;
  pthread_mutex_unlock(&g_dump_lock);
  if (notice[0] != '\0') pal_notification_send(notice);
  return NULL;
}

static http_response_t *api_dump_start(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  }
  if (request->body == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing JSON body");
  }

  char title_id[32];
  if (json_string(request->body, "title_id", title_id, sizeof(title_id)) != 0 ||
      title_id[0] == '\0') {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing title_id");
  }

  char target[16] = "download";
  (void)json_string(request->body, "target", target, sizeof(target));
  char format[16] = "zip";
  (void)json_string(request->body, "format", format, sizeof(format));
  char dest[DUMP_PATH_MAX] = {0};
  (void)json_string(request->body, "dest", dest, sizeof(dest));
  /* Decrypted output is the point of a dump, so it is the default. */
  int decrypt = 1;
  (void)json_int(request->body, "decrypt", &decrypt);

  int to_console = (strcmp(target, "local") == 0) ? 1 : 0;
  int fmt = (strcmp(format, "files") == 0) ? DUMP_FORMAT_FILES : DUMP_FORMAT_ZIP;

  if (to_console != 0 && dest[0] == '\0') {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "Missing destination path");
  }
  if (to_console == 0 && fmt == DUMP_FORMAT_FILES) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "A streamed dump is always a ZIP");
  }

  pthread_mutex_lock(&g_dump_lock);
  /* A stalled dump is retired here, so a failure can never block the next
   * attempt. */
  dump_reap_if_stale(&g_dump);
  if (g_dump.in_use != 0 && (g_dump.worker_active ||
      (g_dump.state != DUMP_STATE_DONE && g_dump.state != DUMP_STATE_FAILED))) {
    pthread_mutex_unlock(&g_dump_lock);
    return http_api_error_json(HTTP_STATUS_409_CONFLICT,
        g_dump.worker_active ? "The previous dump is still finishing" :
                               "A dump is already running");
  }

  dump_job_destroy(g_dump.job);
  memset(&g_dump, 0, sizeof(g_dump));
  g_dump.in_use = 1;
  g_dump.id = g_dump_next_id++;
  g_dump.to_console = to_console;
  g_dump.worker_active = 1;
  g_dump.format = fmt;
  g_dump.decrypt = decrypt != 0 ? 1 : 0;
  g_dump.started = time(NULL);
  (void)snprintf(g_dump.title_id, sizeof(g_dump.title_id), "%s", title_id);
  (void)snprintf(g_dump.dest, sizeof(g_dump.dest), "%s", dest);
  dump_set_state(&g_dump, DUMP_STATE_LAUNCHING);
  g_dump.entries = 0U;

  pthread_t tid;
  pthread_attr_t attr;
  (void)pthread_attr_init(&attr);
  (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&tid, &attr, dump_worker, &g_dump);
  (void)pthread_attr_destroy(&attr);
  if (rc != 0) {
    dump_fail(&g_dump, "Cannot start dump thread");
    g_dump.worker_active = 0;
    pthread_mutex_unlock(&g_dump_lock);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                               "Cannot start dump thread");
  }

  char body[192];
  int len = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"id\":%d,\"state\":\"launching\","
                     "\"title_id\":\"%s\",\"target\":\"%s\"}",
                     g_dump.id, title_id, to_console != 0 ? "local" : "download");
  pthread_mutex_unlock(&g_dump_lock);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

static http_response_t *api_dump_status(const http_request_t *request) {
  (void)request;

  pthread_mutex_lock(&g_dump_lock);
  /* The client polls this: a stalled dump turns into "failed" right away
   * instead of staying pending forever. */
  dump_reap_if_stale(&g_dump);
  char body[1600];
  int len = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"active\":%s,\"id\":%d,\"state\":\"%s\","
                     "\"title_id\":\"%s\",\"source\":\"%s\",\"entries\":%zu,"
                     "\"size\":%" PRIu64 ",\"bytes_done\":%" PRIu64
                     ",\"started\":%lld,\"to_console\":%s,\"cancelled\":%s,"
                     "\"message\":\"%s\"}",
                     g_dump.in_use != 0 ? "true" : "false", g_dump.id,
                     dump_state_name(g_dump.state), g_dump.title_id,
                     g_dump.source,
                     g_dump.entries, g_dump.bytes, g_dump.bytes_done,
                     (long long)g_dump.started,
                     g_dump.to_console != 0 ? "true" : "false",
                     g_dump.cancel_requested != 0 ? "true" : "false",
                     g_dump.error[0] != '\0' ? g_dump.error : g_dump.source);
  pthread_mutex_unlock(&g_dump_lock);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

typedef struct {
  dump_job_t *job;
  int id;
  uint64_t expected;
  uint64_t produced;
} dump_stream_t;

static ssize_t dump_body_read(void *ctx, void *buf, size_t len) {
  dump_stream_t *stream = (dump_stream_t *)ctx;

  /* A cancel request ends the response early; the HTTP layer then reports the
   * download as failed. */
  pthread_mutex_lock(&g_dump_lock);
  int cancelled = g_dump.in_use != 0 && g_dump.id == stream->id &&
                  g_dump.cancel_requested != 0;
  pthread_mutex_unlock(&g_dump_lock);
  if (cancelled) {
    errno = ECANCELED;
    return -1;
  }

  ssize_t got = dump_job_read(stream->job, buf, len);
  if (got > 0) {
    stream->produced += (uint64_t)got;
    pthread_mutex_lock(&g_dump_lock);
    if (g_dump.id == stream->id && g_dump.state == DUMP_STATE_RUNNING)
      g_dump.bytes_done = stream->produced;
    pthread_mutex_unlock(&g_dump_lock);
  }
  return got;
}

static void dump_body_result(void *ctx, int result) {
  dump_stream_t *stream = (dump_stream_t *)ctx;
  char notice[96] = {0};
  pthread_mutex_lock(&g_dump_lock);
  if (g_dump.in_use != 0 && g_dump.id == stream->id &&
      g_dump.state == DUMP_STATE_RUNNING) {
    if (result == 0 && stream->produced == stream->expected) {
      dump_set_state(&g_dump, DUMP_STATE_DONE);
      (void)snprintf(notice, sizeof(notice),
                     "zftpd: dump %s downloaded", g_dump.title_id);
    } else if (g_dump.cancel_requested != 0) {
      dump_fail(&g_dump, "Cancelled");
    } else {
      dump_fail(&g_dump, "Dump download was interrupted or incomplete");
    }
  }
  pthread_mutex_unlock(&g_dump_lock);
  if (notice[0] != '\0') pal_notification_send(notice);
}

static void dump_body_close(void *ctx) {
  dump_stream_t *stream = (dump_stream_t *)ctx;
  dump_job_destroy(stream->job);
  free(stream);
}

static void dump_download_fail(int id, const char *message) {
  pthread_mutex_lock(&g_dump_lock);
  if (g_dump.in_use != 0 && g_dump.id == id &&
      g_dump.state == DUMP_STATE_RUNNING) {
    dump_fail(&g_dump, message);
  }
  pthread_mutex_unlock(&g_dump_lock);
}

static http_response_t *api_dump_download(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char id_text[16] = {0};
  if (query != NULL) {
    const char *id_param = strstr(query, "id=");
    if (id_param != NULL) {
      (void)snprintf(id_text, sizeof(id_text), "%s", id_param + 3);
    }
  }

  pthread_mutex_lock(&g_dump_lock);
  int id = atoi(id_text);
  if (g_dump.in_use == 0 || id != g_dump.id || g_dump.job == NULL ||
      g_dump.to_console != 0 || g_dump.state != DUMP_STATE_READY) {
    pthread_mutex_unlock(&g_dump_lock);
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "No prepared dump");
  }

  dump_job_t *job = g_dump.job;
  uint64_t size = g_dump.bytes;
  char title[32];
  (void)snprintf(title, sizeof(title), "%s", g_dump.title_id);
  g_dump.job = NULL; /* single use */
  dump_set_state(&g_dump, DUMP_STATE_RUNNING);
  pthread_mutex_unlock(&g_dump_lock);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) {
    dump_job_destroy(job);
    dump_download_fail(id, "Cannot create dump response");
    return NULL;
  }

  char len_str[32];
  (void)snprintf(len_str, sizeof(len_str), "%" PRIu64, size);
  http_response_add_header(resp, "Content-Type", "application/zip");
  http_response_add_header(resp, "Content-Length", len_str);
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_add_header(resp, "X-Zftpd-Dump", title);
  char disposition[128];
  (void)snprintf(disposition, sizeof(disposition),
                 "attachment; filename=\"%s.zip\"", title);
  http_response_add_header(resp, "Content-Disposition", disposition);

  if (http_response_finalize(resp) != 0) {
    dump_job_destroy(job);
    http_response_destroy(resp);
    dump_download_fail(id, "Cannot prepare dump response");
    return NULL;
  }

  dump_stream_t *stream = calloc(1U, sizeof(*stream));
  if (stream == NULL) {
    dump_job_destroy(job);
    http_response_destroy(resp);
    dump_download_fail(id, "Cannot allocate dump stream");
    return NULL;
  }
  stream->job = job;
  stream->id = id;
  stream->expected = size;
  resp->stream_ctx = stream;
  resp->stream_read = dump_body_read;
  resp->stream_result = dump_body_result;
  resp->stream_close = dump_body_close;
  resp->stream_chunked = 0; /* Content-Length was announced */
  return resp;
}

static http_response_t *api_dump_cancel(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  }

  /* The id is optional: there is only ever one dump slot. */
  int id = 0;
  (void)json_int(request->body, "id", &id);

  pthread_mutex_lock(&g_dump_lock);
  if (g_dump.in_use == 0 || (id != 0 && id != g_dump.id)) {
    pthread_mutex_unlock(&g_dump_lock);
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "No dump to cancel");
  }
  if (g_dump.state == DUMP_STATE_DONE || g_dump.state == DUMP_STATE_FAILED) {
    pthread_mutex_unlock(&g_dump_lock);
    return http_api_error_json(HTTP_STATUS_409_CONFLICT,
                               "The dump has already finished");
  }

  g_dump.cancel_requested = 1;
  if (g_dump.worker_active) {
    /* Local copy in progress: the writer stops at the next chunk. */
    dump_job_cancel(g_dump.job);
  } else if (g_dump.job != NULL) {
    /* Prepared archive nobody downloaded yet: drop it right here. */
    dump_job_destroy(g_dump.job);
    g_dump.job = NULL;
    dump_fail(&g_dump, "Cancelled");
  }
  /* A streamed download sees the flag in dump_body_read() and ends early. */

  char body[128];
  int len = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"id\":%d,\"state\":\"%s\","
                     "\"cancelled\":true}",
                     g_dump.id, dump_state_name(g_dump.state));
  pthread_mutex_unlock(&g_dump_lock);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

http_response_t *http_api_dump_handle(const http_request_t *request) {
  if (request == NULL) return NULL;
  if (http_api_route_is(request->uri, "/api/dump/start"))
    return api_dump_start(request);
  if (http_api_route_is(request->uri, "/api/dump/status"))
    return api_dump_status(request);
  if (http_api_route_is(request->uri, "/api/dump/cancel"))
    return api_dump_cancel(request);
  if (http_api_route_is(request->uri, "/api/dump"))
    return api_dump_download(request);
  return NULL;
}
