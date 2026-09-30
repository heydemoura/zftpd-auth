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
 * @file ftp_instance.c
 * @brief Process-lifetime daemon identity for rest-mode reconnect
 */

#include "ftp_instance.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_uint_fast64_t g_daemon_instance_id = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t g_daemon_start_ns = ATOMIC_VAR_INIT(0);

static uint64_t monotonic_ns_now(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
  }
  /* Fallback: wall clock (still unique enough for instance seed). */
  return (uint64_t)time(NULL) * 1000000000ULL;
}

void ftp_instance_init(void) {
  (void)ftp_daemon_instance_id();
}

uint64_t ftp_daemon_instance_id(void) {
  uint64_t id = atomic_load(&g_daemon_instance_id);
  if (id != 0U) {
    return id;
  }

  uint64_t start_ns = monotonic_ns_now();
  uint64_t seed = start_ns ^ (uint64_t)(uintptr_t)&g_daemon_instance_id ^
                  (uint64_t)getpid();
  /* Mix seed; keep non-zero so clients can distinguish "unknown" (0). */
  seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
  if (seed == 0U) {
    seed = 1ULL;
  }

  uint64_t expected = 0U;
  if (atomic_compare_exchange_strong(&g_daemon_instance_id, &expected, seed)) {
    atomic_store(&g_daemon_start_ns, start_ns);
    return seed;
  }
  return atomic_load(&g_daemon_instance_id);
}

uint64_t ftp_daemon_start_monotonic_ns(void) {
  (void)ftp_daemon_instance_id();
  return atomic_load(&g_daemon_start_ns);
}

int ftp_instance_identity_format(char *out, size_t capacity, int pid,
                                 uint64_t token) {
  if (out == NULL || capacity == 0U || pid <= 1) {
    return -1;
  }
  int written = snprintf(out, capacity, "%d %016llx\n", pid,
                         (unsigned long long)token);
  if (written <= 0 || (size_t)written >= capacity) {
    return -1;
  }
  return 0;
}

int ftp_instance_identity_parse(const char *text, int *pid, uint64_t *token) {
  if (text == NULL) {
    return -1;
  }

  errno = 0;
  char *pid_end = NULL;
  long parsed_pid = strtol(text, &pid_end, 10);
  if (pid_end == text || errno != 0 || parsed_pid <= 1L || parsed_pid > INT_MAX) {
    return -1;
  }
  if (*pid_end != ' ' && *pid_end != '\t') {
    return -1;
  }

  const char *token_start = pid_end + 1;
  while (*token_start == ' ' || *token_start == '\t') {
    token_start++;
  }

  errno = 0;
  char *token_end = NULL;
  unsigned long long parsed_token = strtoull(token_start, &token_end, 16);
  if (token_end == token_start || errno != 0) {
    return -1;
  }
  while (*token_end == ' ' || *token_end == '\t') {
    token_end++;
  }
  if (*token_end != '\0' && *token_end != '\n' && *token_end != '\r') {
    return -1;
  }

  if (pid != NULL) {
    *pid = (int)parsed_pid;
  }
  if (token != NULL) {
    *token = (uint64_t)parsed_token;
  }
  return 0;
}

int ftp_instance_stop_command_format(char *out, size_t capacity,
                                     uint64_t token) {
  if (out == NULL || capacity == 0U) {
    return -1;
  }
  int written = snprintf(out, capacity, "STOP %016llx\n",
                         (unsigned long long)token);
  if (written <= 0 || (size_t)written >= capacity) {
    return -1;
  }
  return 0;
}

int ftp_instance_stop_command_parse(const char *line, uint64_t *token) {
  static const char prefix[] = "STOP ";
  if (line == NULL) {
    return -1;
  }
  if (strncmp(line, prefix, sizeof(prefix) - 1U) != 0) {
    return -1;
  }

  const char *start = line + sizeof(prefix) - 1U;
  errno = 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(start, &end, 16);
  if (end == start || errno != 0) {
    return -1;
  }
  while (*end == ' ' || *end == '\t') {
    end++;
  }
  if (*end != '\0' && *end != '\n' && *end != '\r') {
    return -1;
  }

  if (token != NULL) {
    *token = (uint64_t)parsed;
  }
  return 0;
}
