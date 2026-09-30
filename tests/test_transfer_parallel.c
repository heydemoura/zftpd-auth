#include "transfer/transfer_manager.h"
#include "../src/transfer/transfer_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIXTURE_SIZE (128U * 1024U * 1024U + 123U)

#define CHECK(x) do { \
  if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return 1; \
  } \
} while (0)

#if defined(ENABLE_LIBCURL) && ENABLE_LIBCURL
typedef struct {
  int listen_fd;
  atomic_int stop;
  atomic_int failed;
  atomic_int requests;
  atomic_int bad_validator;
  atomic_int slow_sample;
  pthread_t clients[64];
  size_t client_count;
} server_ctx_t;

typedef struct {
  server_ctx_t *server;
  int fd;
} client_ctx_t;

static int send_all(int fd, const void *data, size_t len) {
  const unsigned char *p = data;
  while (len != 0U) {
    ssize_t sent = send(fd, p, len, 0);
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) return -1;
    p += (size_t)sent;
    len -= (size_t)sent;
  }
  return 0;
}

static void *serve_client(void *opaque) {
  client_ctx_t *client = opaque;
  server_ctx_t *server = client->server;
  int fd = client->fd;
  free(client);

  char request[4096] = {0};
  size_t used = 0U;
  while (used + 1U < sizeof(request) &&
         strstr(request, "\r\n\r\n") == NULL) {
    ssize_t got = recv(fd, request + used, sizeof(request) - used - 1U, 0);
    if (got <= 0) break;
    used += (size_t)got;
    request[used] = '\0';
  }

  unsigned long long first = 0U, last = 0U;
  const char *range = strstr(request, "Range: bytes=");
  if (strstr(request, "GET /blob.bin ") == NULL || range == NULL ||
      sscanf(range, "Range: bytes=%llu-%llu", &first, &last) != 2 ||
      first > last || last >= FIXTURE_SIZE ||
      (last != 0U &&
       strstr(request, "If-Range: \"parallel-fixture-v1\"") == NULL)) {
    atomic_store(&server->failed, 1);
    (void)close(fd);
    return NULL;
  }

  uint64_t length = (uint64_t)(last - first + 1U);
  int bad = last != 0U && atomic_load(&server->bad_validator) != 0;
  char header[512];
  int n = snprintf(header, sizeof(header),
                   "HTTP/1.1 206 Partial Content\r\n"
                   "Content-Length: %" PRIu64 "\r\n"
                   "Content-Range: bytes %llu-%llu/%u\r\n"
                   "ETag: \"%s\"\r\n"
                   "Connection: close\r\n\r\n",
                   length, first, last, FIXTURE_SIZE,
                   bad ? "different-file" : "parallel-fixture-v1");
  if (n <= 0 || (size_t)n >= sizeof(header) ||
      send_all(fd, header, (size_t)n) != 0) {
    atomic_store(&server->failed, 1);
    (void)close(fd);
    return NULL;
  }

  /* Make the 32 MiB sample deliberately slower than a single fast stream so
   * this fixture exercises the four-connection branch deterministically. */
  if (bad == 0 && atomic_load(&server->slow_sample) != 0 &&
      first == 0U && last == 32U * 1024U * 1024U - 1U)
    usleep(2500000U);

  unsigned char chunk[65536];
  for (uint64_t offset = (uint64_t)first; offset <= (uint64_t)last;) {
    size_t count = (size_t)(((uint64_t)last - offset + 1U) < sizeof(chunk)
                                ? ((uint64_t)last - offset + 1U)
                                : sizeof(chunk));
    for (size_t i = 0U; i < count; i++)
      chunk[i] = (unsigned char)((offset + i) % 251U);
    if (send_all(fd, chunk, count) != 0) {
      if (bad == 0) atomic_store(&server->failed, 1);
      break;
    }
    offset += count;
  }
  atomic_fetch_add(&server->requests, 1);
  (void)close(fd);
  return NULL;
}

static void *serve_http(void *opaque) {
  server_ctx_t *server = opaque;
  while (atomic_load(&server->stop) == 0) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(server->listen_fd, &readfds);
    struct timeval timeout = {0, 200000};
    int ready = select(server->listen_fd + 1, &readfds, NULL, NULL, &timeout);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) continue;
    int fd = accept(server->listen_fd, NULL, NULL);
    if (fd < 0) continue;
    if (server->client_count >= sizeof(server->clients) /
                                    sizeof(server->clients[0])) {
      atomic_store(&server->failed, 1);
      (void)close(fd);
      continue;
    }
    client_ctx_t *client = malloc(sizeof(*client));
    if (client == NULL) {
      atomic_store(&server->failed, 1);
      (void)close(fd);
      continue;
    }
    client->server = server;
    client->fd = fd;
    if (pthread_create(&server->clients[server->client_count], NULL,
                       serve_client, client) != 0) {
      atomic_store(&server->failed, 1);
      free(client);
      (void)close(fd);
      continue;
    }
    server->client_count++;
  }
  for (size_t i = 0U; i < server->client_count; i++)
    (void)pthread_join(server->clients[i], NULL);
  return NULL;
}

static int wait_for_transfer(int id, transfer_snapshot_t *out) {
  for (int i = 0; i < 1500; i++) {
    transfer_snapshot_t snapshots[TRANSFER_MAX_ACTIVE];
    size_t count = transfer_snapshot_all(snapshots, TRANSFER_MAX_ACTIVE);
    for (size_t j = 0U; j < count; j++) {
      if (snapshots[j].id == id && snapshots[j].done != 0) {
        *out = snapshots[j];
        return 0;
      }
    }
    usleep(20000U);
  }
  return -1;
}

static int verify_file(const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) return -1;
  unsigned char chunk[65536];
  uint64_t offset = 0U;
  size_t count = 0U;
  while ((count = fread(chunk, 1U, sizeof(chunk), file)) != 0U) {
    for (size_t i = 0U; i < count; i++) {
      if (chunk[i] != (unsigned char)((offset + i) % 251U)) {
        (void)fclose(file);
        return -1;
      }
    }
    offset += count;
  }
  int rc = ferror(file);
  if (fclose(file) != 0 || rc != 0 || offset != FIXTURE_SIZE) return -1;
  return 0;
}

static int write_partial(const char *path, size_t length, int corrupt) {
  FILE *file = fopen(path, "wb");
  if (file == NULL) return -1;
  unsigned char chunk[65536];
  for (size_t offset = 0U; offset < length;) {
    size_t count = length - offset < sizeof(chunk) ? length - offset
                                                   : sizeof(chunk);
    for (size_t i = 0U; i < count; i++)
      chunk[i] = (unsigned char)((offset + i) % 251U);
    if (corrupt != 0 && offset == 0U) chunk[0] ^= 1U;
    if (fwrite(chunk, 1U, count, file) != count) {
      (void)fclose(file);
      return -1;
    }
    offset += count;
  }
  return fclose(file) == 0 ? 0 : -1;
}
#endif

int main(void) {
#if !defined(ENABLE_LIBCURL) || !ENABLE_LIBCURL
  puts("test_transfer_parallel: skipped (libcurl disabled)");
  return 0;
#else
  (void)signal(SIGPIPE, SIG_IGN);
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(listen_fd >= 0);
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  CHECK(bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) == 0);
  CHECK(listen(listen_fd, 16) == 0);
  socklen_t address_len = sizeof(address);
  CHECK(getsockname(listen_fd, (struct sockaddr *)&address, &address_len) == 0);

  server_ctx_t server = {0};
  server.listen_fd = listen_fd;
  atomic_store(&server.slow_sample, 1);
  pthread_t server_thread;
  CHECK(pthread_create(&server_thread, NULL, serve_http, &server) == 0);

  char directory[] = "/tmp/zftpd-parallel-XXXXXX";
  CHECK(mkdtemp(directory) != NULL);
  transfer_state_set_dir(directory);
  char url[128];
  (void)snprintf(url, sizeof(url), "http://127.0.0.1:%u/blob.bin",
                 (unsigned)ntohs(address.sin_port));
  int id = 0, queued = 0;
  char name[TRANSFER_NAME_MAX], error[TRANSFER_ERROR_MAX];
  CHECK(transfer_start(url, directory, &id, &queued, name, sizeof(name),
                       error, sizeof(error)) == 0);
  CHECK(queued == 0);

  transfer_snapshot_t snapshot;
  CHECK(wait_for_transfer(id, &snapshot) == 0);
  if (snapshot.error != 0)
    fprintf(stderr, "transfer error: %s\n", snapshot.error_msg);
  CHECK(snapshot.error == 0);
  CHECK(snapshot.downloaded == FIXTURE_SIZE);
  CHECK(snapshot.total_size == FIXTURE_SIZE);
  CHECK(atomic_load(&server.failed) == 0);
  CHECK(atomic_load(&server.requests) == 15);

  char path[512];
  (void)snprintf(path, sizeof(path), "%s/%s", directory, name);
  CHECK(verify_file(path) == 0);

  /* A fast sample should continue on one validated connection. */
  atomic_store(&server.slow_sample, 0);
  int fast_id = 0;
  CHECK(transfer_start(url, directory, &fast_id, &queued, name, sizeof(name),
                       error, sizeof(error)) == 0);
  transfer_snapshot_t fast;
  CHECK(wait_for_transfer(fast_id, &fast) == 0);
  if (fast.error != 0)
    fprintf(stderr, "fast transfer error: %s\n", fast.error_msg);
  CHECK(fast.error == 0 && fast.downloaded == FIXTURE_SIZE);
  CHECK(atomic_load(&server.requests) == 18);
  CHECK(verify_file(path) == 0);

  /* Existing partials use verified parallel ranges too, including when the
   * resume offset does not line up with the 8 MiB chunks. */
  char part_path[540];
  (void)snprintf(part_path, sizeof(part_path), "%s.zftpd.part", path);
  const size_t prefix_size = 40U * 1024U * 1024U + 7U;
  CHECK(write_partial(part_path, prefix_size, 0) == 0);
  int resumed_id = 0;
  CHECK(transfer_start(url, directory, &resumed_id, &queued, name,
                       sizeof(name), error, sizeof(error)) == 0);
  transfer_snapshot_t resumed;
  CHECK(wait_for_transfer(resumed_id, &resumed) == 0);
  if (resumed.error != 0)
    fprintf(stderr, "resume error: %s\n", resumed.error_msg);
  CHECK(resumed.error == 0 && resumed.downloaded == FIXTURE_SIZE);
  CHECK(atomic_load(&server.requests) == 34);
  CHECK(verify_file(path) == 0);

  /* A different local prefix must be preserved for inspection, not silently
   * combined with the remote suffix. */
  CHECK(write_partial(part_path, prefix_size, 1) == 0);
  int mismatched_id = 0;
  CHECK(transfer_start(url, directory, &mismatched_id, &queued, name,
                       sizeof(name), error, sizeof(error)) == 0);
  transfer_snapshot_t mismatched;
  CHECK(wait_for_transfer(mismatched_id, &mismatched) == 0);
  CHECK(mismatched.error != 0);
  CHECK(mismatched.downloaded == prefix_size);
  struct stat st;
  CHECK(stat(part_path, &st) == 0 && st.st_size == (off_t)prefix_size);
  CHECK(atomic_load(&server.requests) == 36);
  CHECK(transfer_delete(mismatched_id) == 0);
  CHECK(access(part_path, F_OK) != 0);

  /* A changed ETag must never overwrite the existing complete file or leave
   * a misleading partial prefix that could later be resumed as valid. */
  atomic_store(&server.bad_validator, 1);
  int changed_id = 0;
  CHECK(transfer_start(url, directory, &changed_id, &queued, name, sizeof(name),
                       error, sizeof(error)) == 0);
  transfer_snapshot_t changed;
  CHECK(wait_for_transfer(changed_id, &changed) == 0);
  CHECK(changed.error != 0);
  CHECK(changed.downloaded == 0U);
  CHECK(stat(path, &st) == 0 && st.st_size == FIXTURE_SIZE);
  CHECK(stat(part_path, &st) == 0 && st.st_size == 0);

  atomic_store(&server.stop, 1);
  (void)pthread_join(server_thread, NULL);
  (void)close(listen_fd);
  CHECK(atomic_load(&server.failed) == 0);
  CHECK(unlink(path) == 0);
  CHECK(unlink(part_path) == 0);
  char state_path[TRANSFER_STATE_PATH_MAX];
  CHECK(transfer_state_path(state_path, sizeof(state_path)) == 0);
  (void)unlink(state_path);
  transfer_state_set_dir(NULL);
  CHECK(rmdir(directory) == 0);
  puts("test_transfer_parallel: ok");
  return 0;
#endif
}
