/**
 * @file pal_dns_filter.c
 * @brief Blackholes the Sony CDN domains at the DNS level.
 *
 * Managed in-process so no second payload is needed: names matching the
 * override masks answer 0.0.0.0, names matching the exception list (and
 * everything else) are forwarded upstream.
 */

#include "pal_dns_filter.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "pal_notification.h"

#define DNS_PORT 53
#define DNS_MAX_MESSAGE 1500
#define DNS_UPSTREAM_PRIMARY "1.1.1.1"
#define DNS_UPSTREAM_SECONDARY "8.8.8.8"
#define DNS_UPSTREAM_TIMEOUT_MS 1500
#define DNS_TTL_SECONDS 60

/* Marker file: present means the user turned the filter off in the settings. */
#define DNS_DISABLE_MARKER "/data/zftpd/dnsfilter.disabled"
/* Another resolver may already own the console: do not fight over port 53. */
#define OTHER_RESOLVER_CONFIG "/data/nanodns/nanodns.ini"

/* Answer A records with this address: a black hole. */
static const uint8_t k_blackhole[4] = {0, 0, 0, 0};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static const char *const k_override_masks[] = {
    "*.playstation.com",  "*.playstation.com.*", "playstation.com",
    "*.playstation.net",  "*.playstation.net.*", "playstation.net",
    "*.psndl.net",        "psndl.net",
};

/*
 * Names that must keep resolving: they are service endpoints the console needs
 * for anything but content delivery, and blackholing them makes the system
 * look broken.
 */
static const char *const k_exception_masks[] = {
    "feature.api.playstation.com", "*.stun.playstation.net",
    "stun.*.playstation.net",      "ena.net.playstation.net",
    "post.net.playstation.net",    "gst.prod.dl.playstation.net",
};

static char dns_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool pal_dns_mask_match(const char *mask, const char *name) {
  if ((mask == NULL) || (name == NULL)) {
    return false;
  }

  while (*mask != '\0') {
    if (*mask == '*') {
      while (*mask == '*') {
        mask++;
      }
      if (*mask == '\0') {
        return true;
      }
      for (const char *tail = name; *tail != '\0'; tail++) {
        if (pal_dns_mask_match(mask, tail)) {
          return true;
        }
      }
      return false;
    }

    if (*name == '\0') {
      return false;
    }
    if ((*mask != '?') && (dns_lower(*mask) != dns_lower(*name))) {
      return false;
    }
    mask++;
    name++;
  }

  return *name == '\0';
}

static bool matches_any(const char *name, const char *const *masks, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (pal_dns_mask_match(masks[i], name)) {
      return true;
    }
  }
  return false;
}

bool pal_dns_override_for(const char *name, uint8_t ip[4]) {
  if ((name == NULL) || (ip == NULL)) {
    return false;
  }
  if (matches_any(name, k_exception_masks, ARRAY_LEN(k_exception_masks))) {
    return false;
  }
  if (!matches_any(name, k_override_masks, ARRAY_LEN(k_override_masks))) {
    return false;
  }

  memcpy(ip, k_blackhole, sizeof(k_blackhole));
  return true;
}

/*---------------------------------------------------------------------------*
 * Wire format helpers
 *---------------------------------------------------------------------------*/

static int g_dns_udp_fd = -1;
static int g_dns_tcp_fd = -1;
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
/* Only the console builds run the resolver loops that watch this flag. */
static bool g_dns_stop = false;
#endif
static pthread_t g_dns_udp_thread;
static pthread_t g_dns_tcp_thread;
static bool g_dns_udp_live = false;
static bool g_dns_tcp_live = false;

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)

static uint16_t read_be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/**
 * Reads the question's name into @p out (lowercased, dot separated).
 *
 * @return Offset just past the terminating zero, or -1 when malformed or when
 *         the query uses compression pointers (which queries do not).
 */
static int dns_read_name(const uint8_t *msg, size_t len, size_t off, char *out,
                         size_t out_size) {
  size_t used = 0U;

  while (off < len) {
    uint8_t label = msg[off++];
    if (label == 0U) {
      if (out_size == 0U) {
        return -1;
      }
      out[used] = '\0';
      return (int)off;
    }
    if ((label & 0xC0U) != 0U) {
      return -1;
    }
    if ((off + label) > len) {
      return -1;
    }
    if ((used + (size_t)label + 2U) > out_size) {
      return -1;
    }
    if (used > 0U) {
      out[used++] = '.';
    }
    for (uint8_t i = 0; i < label; i++) {
      out[used++] = dns_lower((char)msg[off + i]);
    }
    off += label;
  }

  return -1;
}

/**
 * Builds a response that answers the question with the override address.
 *
 * A and AAAA get an answer (0.0.0.0 / ::) so the client never falls back to a
 * real lookup; other types get NOERROR with no records.
 */
static size_t dns_build_override(const uint8_t *query, size_t query_len,
                                 size_t question_end, uint16_t qtype,
                                 uint8_t *out, size_t out_size) {
  if ((query_len < 12U) || (question_end + 4U > query_len)) {
    return 0U;
  }

  bool answer = (qtype == 1U) || (qtype == 28U);
  size_t answer_size = answer ? (((qtype == 28U) ? 16U : 4U) + 12U) : 0U;
  if (out_size < (question_end + answer_size)) {
    return 0U;
  }

  memcpy(out, query, question_end);
  out[2] = 0x81U; /* QR=1, opcode 0, RD=1 */
  out[3] = 0x80U; /* RA=1, rcode=NOERROR */
  out[6] = answer ? 0x00U : 0x00U;
  out[7] = answer ? 0x01U : 0x00U;
  out[8] = 0U;
  out[9] = 0U; /* NSCOUNT */
  out[10] = 0U;
  out[11] = 0U; /* ARCOUNT */

  size_t o = question_end;
  if (answer) {
    out[o++] = 0xC0U;
    out[o++] = 0x0CU; /* name: pointer to the question */
    out[o++] = 0x00U;
    out[o++] = (qtype == 28U) ? 0x1CU : 0x01U;
    out[o++] = 0x00U;
    out[o++] = 0x01U; /* IN */
    out[o++] = 0x00U;
    out[o++] = 0x00U;
    out[o++] = 0x00U;
    out[o++] = DNS_TTL_SECONDS;
    out[o++] = 0x00U;
    out[o++] = (qtype == 28U) ? 0x10U : 0x04U;
    if (qtype == 28U) {
      memset(out + o, 0, 16U);
      o += 16U;
    } else {
      memcpy(out + o, k_blackhole, sizeof(k_blackhole));
      o += sizeof(k_blackhole);
    }
  }

  return o;
}

/*---------------------------------------------------------------------------*
 * Upstream forwarding
 *---------------------------------------------------------------------------*/

static int forward_to(const char *server, const uint8_t *query, size_t query_len,
                      uint8_t *reply, size_t reply_size) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DNS_PORT);
  if (inet_pton(AF_INET, server, &addr.sin_addr) != 1) {
    return -1;
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }

  struct timeval tv;
  tv.tv_sec = DNS_UPSTREAM_TIMEOUT_MS / 1000;
  tv.tv_usec = (DNS_UPSTREAM_TIMEOUT_MS % 1000) * 1000;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  ssize_t sent = sendto(fd, query, query_len, 0, (struct sockaddr *)&addr, sizeof(addr));
  if (sent != (ssize_t)query_len) {
    (void)close(fd);
    return -1;
  }

  ssize_t got = recv(fd, reply, reply_size, 0);
  (void)close(fd);
  return (got > 0) ? (int)got : -1;
}

static int forward_upstream(const uint8_t *query, size_t query_len, uint8_t *reply,
                            size_t reply_size) {
  int got = forward_to(DNS_UPSTREAM_PRIMARY, query, query_len, reply, reply_size);
  if (got < 0) {
    got = forward_to(DNS_UPSTREAM_SECONDARY, query, query_len, reply, reply_size);
  }
  return got;
}

/*---------------------------------------------------------------------------*
 * Listeners
 *---------------------------------------------------------------------------*/

static size_t handle_query(const uint8_t *query, size_t query_len, uint8_t *reply,
                           size_t reply_size, bool *overridden) {
  *overridden = false;

  if (query_len < 12U) {
    return 0U;
  }

  char name[256];
  int name_end = dns_read_name(query, query_len, 12U, name, sizeof(name));
  if (name_end < 0) {
    return 0U;
  }
  size_t question_end = (size_t)name_end + 4U;
  if (question_end > query_len) {
    return 0U;
  }
  uint16_t qtype = read_be16(query + (size_t)name_end);

  uint8_t blackhole[4];
  if (pal_dns_override_for(name, blackhole)) {
    *overridden = true;
    return dns_build_override(query, query_len, question_end, qtype, reply,
                              reply_size);
  }

  return 0U;
}

static void *udp_loop(void *unused) {
  (void)unused;

  uint8_t query[DNS_MAX_MESSAGE];
  uint8_t reply[DNS_MAX_MESSAGE];

  while (!g_dns_stop) {
    struct sockaddr_storage from;
    socklen_t from_len = sizeof(from);
    ssize_t got = recvfrom(g_dns_udp_fd, query, sizeof(query), 0,
                           (struct sockaddr *)&from, &from_len);
    if (got <= 0) {
      continue;
    }

    bool overridden = false;
    size_t out_len = handle_query(query, (size_t)got, reply, sizeof(reply), &overridden);
    if (out_len == 0U) {
      int forwarded = forward_upstream(query, (size_t)got, reply, sizeof(reply));
      if (forwarded <= 0) {
        continue;
      }
      out_len = (size_t)forwarded;
    }

    (void)sendto(g_dns_udp_fd, reply, out_len, 0, (struct sockaddr *)&from, from_len);
  }

  return NULL;
}

static void *tcp_loop(void *unused) {
  (void)unused;

  while (!g_dns_stop) {
    struct sockaddr_storage from;
    socklen_t from_len = sizeof(from);
    int client = accept(g_dns_tcp_fd, (struct sockaddr *)&from, &from_len);
    if (client < 0) {
      continue;
    }

    uint8_t length_bytes[2];
    if (recv(client, length_bytes, sizeof(length_bytes), MSG_WAITALL) ==
        (ssize_t)sizeof(length_bytes)) {
      size_t query_len = read_be16(length_bytes);
      uint8_t query[DNS_MAX_MESSAGE];
      uint8_t reply[DNS_MAX_MESSAGE];

      if ((query_len > 0U) && (query_len <= sizeof(query)) &&
          (recv(client, query, query_len, MSG_WAITALL) == (ssize_t)query_len)) {
        bool overridden = false;
        size_t out_len = handle_query(query, query_len, reply, sizeof(reply), &overridden);
        if (out_len == 0U) {
          int forwarded = forward_upstream(query, query_len, reply, sizeof(reply));
          out_len = (forwarded > 0) ? (size_t)forwarded : 0U;
        }
        if (out_len > 0U) {
          uint8_t framed[2] = {(uint8_t)(out_len >> 8), (uint8_t)(out_len & 0xFFU)};
          if (send(client, framed, sizeof(framed), 0) == (ssize_t)sizeof(framed)) {
            (void)send(client, reply, out_len, 0);
          }
        }
      }
    }

    (void)close(client);
  }

  return NULL;
}

static int bind_listener(int type, int protocol) {
  int fd = socket(AF_INET, type, protocol);
  if (fd < 0) {
    return -1;
  }

  int reuse = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  if (type == SOCK_DGRAM) {
    /*
     * A receive timeout lets the loop notice a stop request.  Without it the
     * thread stays blocked on the socket while that very socket is being
     * closed, so the port is not released and the next enable fails.
     */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DNS_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    (void)close(fd);
    return -1;
  }

  return fd;
}

#endif /* PLATFORM_PS4 || PLATFORM_PS5 */

int pal_dns_filter_start(void) {
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (g_dns_udp_fd >= 0) {
    return 0;
  }

  if (!pal_dns_filter_enabled()) {
    printf("[zftpd dns] Disabled from the web settings; not starting\n");
    return -1;
  }

  /*
   * Another resolver already serves the console: taking port 53 would either
   * fail or shadow a setup the user built on purpose, so step aside.
   */
  if (pal_dns_filter_other_resolver_present()) {
    printf("[zftpd dns] another resolver is configured on this console (%s); "
           "leaving port 53 to it\n", OTHER_RESOLVER_CONFIG);
    pal_notification_send("zftpd: another resolver is active, DNS filter skipped");
    return -1;
  }

  g_dns_stop = false;

  /* A previous teardown may still be releasing the port: retry briefly. */
  for (int attempt = 0; (attempt < 5) && (g_dns_udp_fd < 0); attempt++) {
    if (attempt > 0) {
      usleep(100000);
    }
    g_dns_udp_fd = bind_listener(SOCK_DGRAM, IPPROTO_UDP);
  }

  if (g_dns_udp_fd < 0) {
    printf("[zftpd dns] Port %d unavailable (%s); DNS filter disabled — "
           "another resolver is already serving it\n",
           DNS_PORT, strerror(errno));
    return -1;
  }

  if (pthread_create(&g_dns_udp_thread, NULL, udp_loop, NULL) == 0) {
    g_dns_udp_live = true;
  }

  g_dns_tcp_fd = bind_listener(SOCK_STREAM, IPPROTO_TCP);
  if (g_dns_tcp_fd >= 0) {
    if (listen(g_dns_tcp_fd, 8) == 0) {
      if (pthread_create(&g_dns_tcp_thread, NULL, tcp_loop, NULL) == 0) {
        g_dns_tcp_live = true;
      }
    } else {
      (void)close(g_dns_tcp_fd);
      g_dns_tcp_fd = -1;
    }
  }

  printf("[zftpd dns] Filter active on port %d: %zu Sony CDN masks blackholed, "
         "%zu exceptions forwarded upstream\n",
         DNS_PORT, ARRAY_LEN(k_override_masks), ARRAY_LEN(k_exception_masks));
  pal_notification_send("zftpd: DNS filter active");
  return 0;
#else
  return -1; /* Only the console builds serve DNS; port 53 needs root. */
#endif
}

void pal_dns_filter_stop(void) {
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  g_dns_stop = true;
#endif

  /*
   * Wake both loops, wait for them to leave, and only then close the sockets:
   * closing while a thread is still blocked inside recvfrom/accept keeps the
   * port bound, which made the first re-enable fail and the second succeed.
   */
  if (g_dns_tcp_fd >= 0) {
    (void)shutdown(g_dns_tcp_fd, SHUT_RDWR);
  }

  if (g_dns_udp_live) {
    (void)pthread_join(g_dns_udp_thread, NULL);
    g_dns_udp_live = false;
  }
  if (g_dns_tcp_live) {
    (void)pthread_join(g_dns_tcp_thread, NULL);
    g_dns_tcp_live = false;
  }

  if (g_dns_udp_fd >= 0) {
    (void)close(g_dns_udp_fd);
    g_dns_udp_fd = -1;
  }
  if (g_dns_tcp_fd >= 0) {
    (void)close(g_dns_tcp_fd);
    g_dns_tcp_fd = -1;
  }
}

bool pal_dns_filter_enabled(void) {
  return access(DNS_DISABLE_MARKER, F_OK) != 0;
}

bool pal_dns_filter_other_resolver_present(void) {
  return access(OTHER_RESOLVER_CONFIG, F_OK) == 0;
}

bool pal_dns_filter_running(void) {
  return g_dns_udp_fd >= 0;
}

size_t pal_dns_filter_mask_count(void) {
  return ARRAY_LEN(k_override_masks);
}

size_t pal_dns_filter_exception_count(void) {
  return ARRAY_LEN(k_exception_masks);
}

int pal_dns_filter_set_enabled(bool enabled) {
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (enabled) {
    (void)unlink(DNS_DISABLE_MARKER);
    return pal_dns_filter_start();
  }

  pal_dns_filter_stop();

  FILE *marker = fopen(DNS_DISABLE_MARKER, "w");
  if (marker != NULL) {
    (void)fputs("disabled\n", marker);
    (void)fclose(marker);
  }
  printf("[zftpd dns] Filter stopped and disabled\n");
  return 0;
#else
  (void)enabled;
  return -1;
#endif
}
