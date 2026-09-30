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
/** @file http_server.c @brief HTTP connection lifecycle and request dispatch. */

#include "http_server.h"
#include "http_server_internal.h"
#include "ftp_config.h"
#include "ftp_log.h"
#include "http_api.h"
#include "http_api_internal.h"
#include "http_config.h"
#include "http_parser.h"
#include "http_response.h"
#include "http_response_stream.h"
#include "pal_fileio.h"
#include "pal_network.h"
#include "pal_notification.h"
#include "pal_resilient_server.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h> /* TCP_NODELAY */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>





static http_server_t g_http_server;
static atomic_int g_http_server_in_use = ATOMIC_VAR_INIT(0);
static http_connection_t g_http_connections[HTTP_MAX_CONNECTIONS];

typedef struct {
  int fd;
  http_response_t *response;
} http_background_response_t;

typedef struct {
  int fd;
  http_request_t request;
  char range_name[6];
  char range_value[128];
} http_background_request_t;

/* A game dump can take minutes. Send it outside the event loop so status
 * requests and other HTTP clients remain responsive during the transfer. */
static void *http_background_send(void *opaque) {
  http_background_response_t *task = (http_background_response_t *)opaque;
  (void)http_response_stream_send(task->fd, task->response, 1);
  http_response_destroy(task->response);
  close(task->fd);
  free(task);
  return NULL;
}

/* File reads can block indefinitely on a slow or disconnected USB device.
 * Resolve and send these GETs off the event loop so status and the rest of
 * the UI remain usable even while one file operation is stuck. */
static void *http_background_file_get(void *opaque) {
  http_background_request_t *task = (http_background_request_t *)opaque;
  http_response_t *response = http_api_handle(&task->request);
  if (response == NULL) {
    response = http_response_create(HTTP_STATUS_500_INTERNAL_ERROR);
    if (response != NULL) {
      static const char message[] = "Internal Server Error";
      (void)http_response_set_body(response, message, sizeof(message) - 1U);
    }
  }
  if (response != NULL) {
    (void)http_response_stream_send(task->fd, response, 1);
    http_response_destroy(response);
  }
  close(task->fd);
  free(task);
  return NULL;
}

static void http_connections_init(void) {
  for (size_t i = 0; i < (size_t)HTTP_MAX_CONNECTIONS; i++) {
    g_http_connections[i].fd = -1;
    g_http_connections[i].server = NULL;
    g_http_connections[i].buffer_used = 0;
#if ENABLE_WEB_UPLOAD
    g_http_connections[i].upload.fd = -1;
#endif
  }
}

http_connection_t *http_server_connection_acquire(http_server_t *server,
                                                  int client_fd) {
  if ((server == NULL) || (client_fd < 0)) {
    return NULL;
  }
  for (size_t i = 0; i < (size_t)HTTP_MAX_CONNECTIONS; i++) {
    if (g_http_connections[i].fd < 0) {
      http_connection_t *conn = &g_http_connections[i];
      memset(conn, 0, sizeof(*conn));
      conn->server = server;
      conn->fd = client_fd;
      conn->buffer_used = 0;
#if ENABLE_WEB_UPLOAD
      conn->upload.fd = -1;
#endif
      return conn;
    }
  }
  return NULL;
}

void http_server_connection_release(http_connection_t *conn) {
  if (conn == NULL) {
    return;
  }
#if ENABLE_WEB_UPLOAD
  http_upload_reset(conn);
#endif
  conn->fd = -1;
  conn->server = NULL;
  conn->buffer_used = 0;
}

/* Internal server-core callbacks. */
static int http_handle_request(http_connection_t *conn, size_t request_length);
static void http_close_connection(http_connection_t *conn);


http_server_t *http_server_create(event_loop_t *loop, const char *bind_addr,
                                  const char *root_path) {
  if ((loop == NULL) || (bind_addr == NULL) || (root_path == NULL)) {
    return NULL;
  }

  if (atomic_load(&g_http_server_in_use) != 0) {
    return NULL;
  }

  memset(&g_http_server, 0, sizeof(g_http_server));
  g_http_server.listen_fd = -1;
  g_http_server.wake_r = -1;
  g_http_server.wake_w = -1;
  atomic_store(&g_http_server.pending_listen_fd, -1);
  g_http_server.loop = loop;
  atomic_store(&g_http_server.connection_count, 0);
  atomic_store(&g_http_server.recreating, 0);
  atomic_store(&g_http_server.alive, 1);

  http_api_set_root(root_path);
  http_connections_init();
  if (http_listener_start(&g_http_server, bind_addr) != 0) {
    atomic_store(&g_http_server.alive, 0);
    return NULL;
  }

  atomic_store(&g_http_server_in_use, 1);
  return &g_http_server;
}

void http_server_destroy(http_server_t *server) {
  if (server != NULL) {
    if (server == &g_http_server) {
      atomic_store(&server->alive, 0);
      for (size_t i = 0; i < (size_t)HTTP_MAX_CONNECTIONS; i++) {
        if (g_http_connections[i].fd >= 0)
          http_close_connection(&g_http_connections[i]);
      }
      http_listener_stop(server);
      atomic_store(&g_http_server_in_use, 0);
    }
  }
}


int http_server_client_callback(int fd, uint32_t events, void *data) {
  http_connection_t *conn = (http_connection_t *)data;
  (void)fd;

  if (events & (EVENT_CLOSE | EVENT_ERROR)) {
    http_close_connection(conn);
    return -1;
  }

  if (events & EVENT_READ) {
#if ENABLE_WEB_UPLOAD
    if (http_upload_is_active(conn)) {
      if (http_upload_continue(conn) < 0) {
        http_close_connection(conn);
        return -1;
      }
      return 0;
    }
#endif

    size_t remaining = sizeof(conn->buffer) - conn->buffer_used - 1;
    if (remaining == 0) {
      /* Buffer full without complete request — drop */
      http_close_connection(conn);
      return -1;
    }

    ssize_t n;
    do {
      n = read(conn->fd, conn->buffer + conn->buffer_used, remaining);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (n <= 0) {
      http_close_connection(conn);
      return -1;
    }

    conn->buffer_used += (size_t)n;
    conn->buffer[conn->buffer_used] = '\0';

    http_request_head_t head;
    int head_rc = http_peek_request_head(conn->buffer, conn->buffer_used, &head);
    if (head_rc == -2) return 0;
    if (head_rc != 0) {
      http_close_connection(conn);
      return -1;
    }

    size_t header_len = head.header_length;
    size_t content_length = head.content_length;

#if ENABLE_WEB_UPLOAD
    if (http_upload_matches(&head)) {
      if (http_upload_start(conn, &head) < 0) {
        http_close_connection(conn);
        return -1;
      }
      return 0;
    }
#endif

    const size_t buf_limit = sizeof(conn->buffer) - 1U;
    if (content_length > buf_limit || header_len > buf_limit - content_length) {
      http_response_t *resp = http_response_create(HTTP_STATUS_400_BAD_REQUEST);
      if (resp != NULL) {
        static const char msg[] = "Request too large";
        if (http_response_set_body(resp, msg, sizeof(msg) - 1U) == 0 &&
            resp->used > 0U)
          (void)pal_send_all(conn->fd, resp->data, resp->used, 0);
        http_response_destroy(resp);
      }
      http_close_connection(conn);
      return -1;
    }

    size_t request_length = header_len + content_length;
    if (conn->buffer_used < request_length) return 0;

    (void)http_handle_request(conn, request_length);
    http_close_connection(conn);
    return -1;
  }

  return 0;
}


static int http_handle_request(http_connection_t *conn, size_t request_length) {
  http_request_t request;
  if (http_parse_request(conn->buffer, request_length, &request) < 0)
    return -1;

  if (request.method == HTTP_METHOD_GET &&
      ((strncmp(request.uri, "/api/file/get", 13) == 0 &&
        (request.uri[13] == '?' || request.uri[13] == '\0')) ||
       (strncmp(request.uri, "/api/download", 13) == 0 &&
        (request.uri[13] == '?' || request.uri[13] == '\0')))) {
    http_background_request_t *task = calloc(1U, sizeof(*task));
    if (task != NULL) {
      task->fd = dup(conn->fd);
      task->request.method = request.method;
      (void)snprintf(task->request.uri, sizeof(task->request.uri), "%s",
                     request.uri);
      const char *range = http_get_header(&request, "Range");
      if (range != NULL) {
        /* Parsed headers point into conn->buffer, which is released as soon
         * as this handler returns.  Keep the range in the background task. */
        (void)snprintf(task->range_value, sizeof(task->range_value), "%s",
                       strlen(range) < sizeof(task->range_value) ? range :
                                                                 "invalid");
        memcpy(task->range_name, "Range", sizeof(task->range_name));
        task->request.headers[0].name = task->range_name;
        task->request.headers[0].value = task->range_value;
        task->request.num_headers = 1;
      }
      if (task->fd >= 0) {
        pthread_t tid;
        pthread_attr_t attr;
        (void)pthread_attr_init(&attr);
        (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&tid, &attr, http_background_file_get, task);
        (void)pthread_attr_destroy(&attr);
        if (rc == 0) return 0;
        close(task->fd);
      }
      free(task);
    }
  }

  http_response_t *response = http_api_handle(&request);
  if (response == NULL) {
    response = http_response_create(HTTP_STATUS_500_INTERNAL_ERROR);
    if (response == NULL) return -1;
    static const char message[] = "Internal Server Error";
    if (http_response_set_body(response, message, sizeof(message) - 1U) != 0) {
      http_response_destroy(response);
      return -1;
    }
  }

  if (request.method == HTTP_METHOD_GET &&
      strncmp(request.uri, "/api/dump?", 10) == 0 &&
      response->stream_read != NULL) {
    http_background_response_t *task = malloc(sizeof(*task));
    if (task != NULL) {
      task->fd = dup(conn->fd);
      task->response = response;
      if (task->fd >= 0) {
        pthread_t tid;
        pthread_attr_t attr;
        (void)pthread_attr_init(&attr);
        (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&tid, &attr, http_background_send, task);
        (void)pthread_attr_destroy(&attr);
        if (rc == 0) return 0;
        close(task->fd);
      }
      free(task);
    }
  }

  int result = http_response_stream_send(conn->fd, response,
                                          request.method != HTTP_METHOD_HEAD);
  http_response_destroy(response);
  return result;
}


static void http_close_connection(http_connection_t *conn) {
  if (conn == NULL) {
    return;
  }

  http_server_t *server = conn->server;
  int fd = conn->fd;

  if ((server != NULL) && (fd >= 0)) {
    event_loop_remove(server->loop, fd);
    if (atomic_load(&server->connection_count) > 0) {
      (void)atomic_fetch_sub(&server->connection_count, 1);
    }
  }

  if (fd >= 0) {
    close(fd);
  }
  http_server_connection_release(conn);
}
