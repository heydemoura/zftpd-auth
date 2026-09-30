#include "transfer/transfer_manager.h"
#include "../src/transfer/transfer_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { \
  if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return 1; \
  } \
} while (0)

static int test_round_trip(void) {
  char dir[] = "/tmp/zftpd-state-XXXXXX";
  CHECK(mkdtemp(dir) != NULL);

  char path[TRANSFER_STATE_PATH_MAX];
  CHECK(snprintf(path, sizeof(path), "%s/transfers.state", dir) > 0);

  transfer_state_entry_t in[2];
  memset(in, 0, sizeof(in));
  in[0].id = 7;
  in[0].paused = 1;
  in[0].status = 'P';
  in[0].total_size = 5000000000ULL;
  in[0].downloaded = 1234567890ULL;
  (void)snprintf(in[0].url, sizeof(in[0].url),
                 "https://example.com/a%%20b.bin?x=1");
  (void)snprintf(in[0].dst, sizeof(in[0].dst), "/data/My Games");
  (void)snprintf(in[0].filename, sizeof(in[0].filename), "a b.bin");
  in[1].id = 12;
  in[1].status = 'E';
  in[1].downloaded = 1024U;
  (void)snprintf(in[1].error_msg, sizeof(in[1].error_msg),
                 "Remote replied 429\ttry later");
  (void)snprintf(in[1].url, sizeof(in[1].url),
                 "ftp://host/dir\twith\ttabs\nand \"quotes\"\\tail");
  (void)snprintf(in[1].dst, sizeof(in[1].dst), "/tmp");
  (void)snprintf(in[1].filename, sizeof(in[1].filename), "weird\tname\n.bin");

  CHECK(transfer_state_write(path, in, 2) == 0);

  transfer_state_entry_t out[4];
  memset(out, 0, sizeof(out));
  size_t count = transfer_state_read(path, out, 4);
  CHECK(count == 2);
  CHECK(out[0].id == 7 && out[0].paused == 1);
  CHECK(out[0].status == 'P' && out[0].total_size == in[0].total_size);
  CHECK(out[0].downloaded == in[0].downloaded);
  CHECK(strcmp(out[0].url, in[0].url) == 0);
  CHECK(strcmp(out[0].dst, in[0].dst) == 0);
  CHECK(strcmp(out[0].filename, in[0].filename) == 0);
  CHECK(out[1].id == 12 && out[1].paused == 0 && out[1].status == 'E');
  CHECK(out[1].downloaded == in[1].downloaded);
  CHECK(strcmp(out[1].error_msg, in[1].error_msg) == 0);
  CHECK(strcmp(out[1].url, in[1].url) == 0);
  CHECK(strcmp(out[1].dst, in[1].dst) == 0);
  CHECK(strcmp(out[1].filename, in[1].filename) == 0);

  /* Capacity is honoured, not exceeded. */
  CHECK(transfer_state_read(path, out, 1) == 1);
  char temp[TRANSFER_STATE_PATH_MAX + 8U];
  CHECK(snprintf(temp, sizeof(temp), "%s.tmp", path) > 0);
  CHECK(access(temp, F_OK) != 0);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(dir) == 0);
  return 0;
}

/* A state file that is corrupt, truncated or written by another version must
 * never keep the daemon from starting: whatever cannot be parsed is dropped. */
static int test_damaged_files(void) {
  char dir[] = "/tmp/zftpd-state-bad-XXXXXX";
  CHECK(mkdtemp(dir) != NULL);

  char path[TRANSFER_STATE_PATH_MAX];
  CHECK(snprintf(path, sizeof(path), "%s/transfers.state", dir) > 0);
  transfer_state_entry_t out[4];

  memset(out, 0, sizeof(out));
  CHECK(transfer_state_read(path, out, 4) == 0); /* missing file */

  FILE *fp = fopen(path, "w");
  CHECK(fp != NULL);
  fputs("not-a-state-file\n", fp);
  CHECK(fclose(fp) == 0);
  CHECK(transfer_state_read(path, out, 4) == 0);

  memset(out, 0, sizeof(out));
  fp = fopen(path, "w");
  CHECK(fp != NULL);
  fputs("zftpd-transfers 1\n", fp);
  fputs("garbage-without-tabs\n", fp);
  fputs("not-a-number\t0\thttp://host/a\t/dst\ta.bin\n", fp);
  fputs("99\t0\t\t/dst\ta.bin\n", fp);              /* empty url     */
  fputs("99\t0\thttp://host/a\t\tb.bin\n", fp);      /* empty dst     */
  fputs("99\t0\thttp://host/a\t/dst\t\n", fp);       /* empty name    */
  fputs("5\t1\thttp://host/ok.bin\t/dst\tok.bin\n", fp);
  CHECK(fclose(fp) == 0);

  CHECK(transfer_state_read(path, out, 4) == 1);
  CHECK(out[0].id == 5 && out[0].paused == 1);
  CHECK(out[0].status == 'P');
  CHECK(strcmp(out[0].filename, "ok.bin") == 0);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(dir) == 0);
  return 0;
}

int main(void) {
  CHECK(test_round_trip() == 0);
  CHECK(test_damaged_files() == 0);
  puts("test_transfer_state: ok");
  return 0;
}
