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
 * @file test_instance.c
 * @brief Unit tests for daemon instance identity + restmode backoff helpers
 */

#include "ftp_instance.h"
#include "pal_resilient_server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  ftp_instance_init();

  uint64_t id1 = ftp_daemon_instance_id();
  uint64_t id2 = ftp_daemon_instance_id();
  uint64_t start1 = ftp_daemon_start_monotonic_ns();
  uint64_t start2 = ftp_daemon_start_monotonic_ns();

  if (id1 == 0U) {
    fprintf(stderr, "instance_id must be non-zero\n");
    return 1;
  }
  if (id1 != id2) {
    fprintf(stderr, "instance_id must be stable within process\n");
    return 2;
  }
  if (start1 == 0U || start1 != start2) {
    fprintf(stderr, "start_monotonic_ns must be stable and non-zero\n");
    return 3;
  }

  if (pal_restmode_backoff_ms(0) != 500U) return 4;
  if (pal_restmode_backoff_ms(1) != 1000U) return 5;
  if (pal_restmode_backoff_ms(5) != 10000U) return 6;
  if (pal_restmode_backoff_ms(99) != 10000U) return 7;

  if (!pal_errno_is_listener_lost(EBADF)) return 8;
  if (pal_errno_is_listener_lost(EAGAIN)) return 9;

  if (pal_listen_fd_alive(-1) != 0) return 10;

  char line[80];
  char command[64];
  int pid = 0;
  uint64_t token = 0U;

  if (ftp_instance_identity_format(line, sizeof(line), 4242, 0x0123456789abcdefULL) != 0)
    return 11;
  if (strcmp(line, "4242 0123456789abcdef\n") != 0) return 12;
  if (ftp_instance_identity_parse(line, &pid, &token) != 0 || pid != 4242 ||
      token != 0x0123456789abcdefULL)
    return 13;

  /* A buffer that cannot hold the whole line is refused, never truncated. */
  if (ftp_instance_identity_format(line, 4U, 4242, 1U) == 0) return 14;
  if (ftp_instance_identity_format(line, sizeof(line), 1, 1U) == 0) return 15;

  /* Malformed instance files must not look like a live payload. */
  if (ftp_instance_identity_parse("not-a-pid\n", &pid, &token) == 0) return 16;
  if (ftp_instance_identity_parse("1 abc\n", &pid, &token) == 0) return 17;
  if (ftp_instance_identity_parse("4242 \n", &pid, &token) == 0) return 18;
  if (ftp_instance_identity_parse("4242 abc trailing\n", &pid, &token) == 0) return 19;
  if (ftp_instance_identity_parse("4242 abc", &pid, &token) != 0 ||
      token != 0xabcULL)
    return 20;

  if (ftp_instance_stop_command_format(command, sizeof(command),
                                       0xfeedfacecafebeefULL) != 0)
    return 21;
  if (strcmp(command, "STOP feedfacecafebeef\n") != 0) return 22;
  if (ftp_instance_stop_command_parse(command, &token) != 0 ||
      token != 0xfeedfacecafebeefULL)
    return 23;

  /* Only the exact command is accepted: a stray token or a different verb
   * must never stop a running payload. */
  if (ftp_instance_stop_command_parse("STOP feedfacecafebeef extra\n", &token) == 0)
    return 24;
  if (ftp_instance_stop_command_parse("stop feedfacecafebeef\n", &token) == 0)
    return 25;
  if (ftp_instance_stop_command_parse("STOP\n", &token) == 0) return 26;
  if (ftp_instance_stop_command_format(command, 6U, 1U) == 0) return 27;

  printf("test_instance: ok (id=%016llx start=%llu)\n",
         (unsigned long long)id1, (unsigned long long)start1);
  return 0;
}
