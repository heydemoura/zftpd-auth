#include "http_server_internal.h"
#include "ftp_log.h"
#include "pal_network.h"
#include "pal_notification.h"
#include "pal_resilient_server.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int listener_accept_callback(int fd, uint32_t events, void *data);
static void listener_schedule_recreate(http_server_t *server);

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int open_listen_socket(http_server_t *server, int *out_fd) {
  if (server == NULL || out_fd == NULL) return -1;
  int fd = socket(server->af, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  pal_socket_set_nosigpipe(fd);
  int reuse = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (server->af == AF_INET6) {
    int v6only = 0;
    (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
  }
  if (bind(fd, (struct sockaddr *)&server->listen_addr,
           server->listen_addr_len) < 0 ||
      listen(fd, 128) < 0 || set_nonblocking(fd) != 0) {
    close(fd);
    return -1;
  }
  *out_fd = fd;
  return 0;
}

static void wake_event_loop(http_server_t *server) {
  if (server == NULL || server->wake_w < 0) return;
  char byte = 1;
  ssize_t n = write(server->wake_w, &byte, 1U);
  if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    ftp_log_line(FTP_LOG_WARN, "[restmode-http] wake pipe write failed");
  }
}

static int wake_callback(int fd, uint32_t events, void *data) {
  (void)events;
  http_server_t *server = (http_server_t *)data;
  if (server == NULL || atomic_load(&server->alive) == 0) return -1;

  char buf[32];
  while (read(fd, buf, sizeof(buf)) > 0) {}
  int new_fd = atomic_exchange(&server->pending_listen_fd, -1);
  if (new_fd < 0) return 0;
  if (server->listen_fd >= 0 && server->listen_fd != new_fd) {
    (void)event_loop_remove(server->loop, server->listen_fd);
    close(server->listen_fd);
  }
  if (event_loop_add(server->loop, new_fd, EVENT_READ,
                     listener_accept_callback, server) != 0) {
    close(new_fd);
    atomic_store(&server->recreating, 0);
    listener_schedule_recreate(server);
    return 0;
  }

  server->listen_fd = new_fd;
  atomic_store(&server->recreating, 0);
  ftp_log_line(FTP_LOG_INFO, "[restmode-http] Listen socket recreated");
  char msg[96];
  (void)snprintf(msg, sizeof(msg), "zftpd: HTTP resumed on port %u",
                 (unsigned)server->port);
  pal_notification_send(msg);
  return 0;
}

static void *recreate_thread(void *arg) {
  http_server_t *server = (http_server_t *)arg;
  unsigned attempt = 0U;
  while (server != NULL && atomic_load(&server->alive) != 0) {
    int new_fd = -1;
    if (attempt > 0U && (attempt % 3U) == 0U) (void)pal_network_reinit();
    if (open_listen_socket(server, &new_fd) == 0) {
      if (atomic_load(&server->alive) == 0) {
        close(new_fd);
        break;
      }
      int previous = atomic_exchange(&server->pending_listen_fd, new_fd);
      if (previous >= 0) close(previous);
      wake_event_loop(server);
      return NULL;
    }
    unsigned delay = pal_restmode_backoff_ms(attempt++);
    if (pal_restmode_sleep_ms(delay, &server->alive) != 0) break;
  }
  atomic_store(&server->recreating, 0);
  return NULL;
}

static void listener_schedule_recreate(http_server_t *server) {
  int expected = 0;
  if (server == NULL || atomic_load(&server->alive) == 0 ||
      !atomic_compare_exchange_strong(&server->recreating, &expected, 1))
    return;

  if (server->listen_fd >= 0) {
    (void)event_loop_remove(server->loop, server->listen_fd);
    close(server->listen_fd);
    server->listen_fd = -1;
  }

  pthread_t tid;
  if (pthread_create(&tid, NULL, recreate_thread, server) != 0) {
    atomic_store(&server->recreating, 0);
    ftp_log_line(FTP_LOG_ERROR, "[restmode-http] recreate thread failed");
    return;
  }
  (void)pthread_detach(tid);
}

static void tune_client_socket(int fd) {
  /* A download aborted from the browser closes the peer early: the write that
   * follows must fail with EPIPE, not kill the daemon. */
  pal_socket_set_nosigpipe(fd);
  int one = 1;
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  int rcvbuf = (int)HTTP_UPLOAD_RCVBUF_SIZE;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
#if (HTTP_SNDBUF_SIZE) > 0U
  int sndbuf = (int)HTTP_SNDBUF_SIZE;
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
#endif
}

static int listener_accept_callback(int fd, uint32_t events, void *data) {
  http_server_t *server = (http_server_t *)data;
  if (server == NULL) return -1;
  if ((events & (EVENT_ERROR | EVENT_CLOSE)) != 0U) {
    listener_schedule_recreate(server);
    return -1;
  }

  int accepted_any = 0;
  for (;;) {
    struct sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
      int error = errno;
      if (error == EINTR) continue;
      if (error == EAGAIN || error == EWOULDBLOCK) {
        if (!accepted_any && !pal_listen_fd_alive(fd)) {
          listener_schedule_recreate(server);
          return -1;
        }
        return 0;
      }
      if (pal_errno_is_listener_lost(error)) {
        listener_schedule_recreate(server);
        return -1;
      }
      return 0;
    }

    accepted_any = 1;
    tune_client_socket(client_fd);
    if (atomic_load(&server->connection_count) >= HTTP_MAX_CONNECTIONS) {
      close(client_fd);
      continue;
    }

    http_connection_t *conn =
        http_server_connection_acquire(server, client_fd);
    if (conn == NULL) {
      close(client_fd);
      continue;
    }
    if (event_loop_add(server->loop, client_fd, EVENT_READ,
                       http_server_client_callback, conn) != 0) {
      close(client_fd);
      http_server_connection_release(conn);
      continue;
    }
    (void)atomic_fetch_add(&server->connection_count, 1);
  }
}

int http_listener_start(http_server_t *server, const char *bind_addr) {
  if (server == NULL || bind_addr == NULL) return -1;
  size_t len = strlen(bind_addr);
  if (len >= sizeof(server->bind_addr)) return -1;
  memcpy(server->bind_addr, bind_addr, len + 1U);

  if (pal_make_sockaddr_ex(bind_addr, &server->listen_addr,
                           &server->listen_addr_len) != FTP_OK)
    return -1;
  server->af = server->listen_addr.ss_family;
  if (server->af == AF_INET6) {
    server->port = ntohs(((struct sockaddr_in6 *)&server->listen_addr)->sin6_port);
  } else if (server->af == AF_INET) {
    server->port = ntohs(((struct sockaddr_in *)&server->listen_addr)->sin_port);
  } else {
    return -1;
  }

  if (open_listen_socket(server, &server->listen_fd) != 0) return -1;
  int pipe_fd[2];
  if (pipe(pipe_fd) == 0) {
    server->wake_r = pipe_fd[0];
    server->wake_w = pipe_fd[1];
    if (set_nonblocking(server->wake_r) != 0 ||
        set_nonblocking(server->wake_w) != 0 ||
        event_loop_add(server->loop, server->wake_r, EVENT_READ,
                       wake_callback, server) != 0) {
      close(server->wake_r);
      close(server->wake_w);
      server->wake_r = server->wake_w = -1;
    }
  }

  if (event_loop_add(server->loop, server->listen_fd, EVENT_READ,
                     listener_accept_callback, server) != 0) {
    http_listener_stop(server);
    return -1;
  }
  return 0;
}

void http_listener_stop(http_server_t *server) {
  if (server == NULL) return;
  wake_event_loop(server);
  if (server->listen_fd >= 0) {
    (void)event_loop_remove(server->loop, server->listen_fd);
    close(server->listen_fd);
    server->listen_fd = -1;
  }

  for (unsigned waited = 0U;
       atomic_load(&server->recreating) != 0 && waited < 2000U;
       waited += 50U)
    usleep(50U * 1000U);

  int pending = atomic_exchange(&server->pending_listen_fd, -1);
  if (pending >= 0) close(pending);
  if (server->wake_r >= 0) {
    (void)event_loop_remove(server->loop, server->wake_r);
    close(server->wake_r);
    server->wake_r = -1;
  }
  if (server->wake_w >= 0) {
    close(server->wake_w);
    server->wake_w = -1;
  }
}
