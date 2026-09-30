#include "transfer/transfer_manager.h"
#include "../src/transfer/transfer_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

int main(void) {
  char root[] = "/tmp/zftpd-orphans-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  char sub[TRANSFER_PART_PATH_MAX], part[TRANSFER_PART_PATH_MAX];
  char link_path[TRANSFER_PART_PATH_MAX], ordinary[TRANSFER_PART_PATH_MAX];
  CHECK(snprintf(sub, sizeof(sub), "%s/downloads", root) > 0);
  CHECK(snprintf(part, sizeof(part), "%s/game.pkg.zftpd.part", sub) > 0);
  CHECK(snprintf(link_path, sizeof(link_path), "%s/link.zftpd.part", sub) > 0);
  CHECK(snprintf(ordinary, sizeof(ordinary), "%s/other.pkg", sub) > 0);
  CHECK(mkdir(sub, 0700) == 0);
  FILE *fp = fopen(part, "wb");
  CHECK(fp != NULL && fputs("partial", fp) >= 0 && fclose(fp) == 0);
  fp = fopen(ordinary, "wb");
  CHECK(fp != NULL && fputs("keep", fp) >= 0 && fclose(fp) == 0);
  CHECK(symlink(ordinary, link_path) == 0);

  transfer_orphans_set_root(root);
  transfer_orphans_scan_now();
  transfer_orphan_t found[TRANSFER_ORPHAN_MAX];
  int scanning = 0;
  CHECK(transfer_orphans_snapshot(found, TRANSFER_ORPHAN_MAX, &scanning) == 1U);
  CHECK(scanning == 0 && strcmp(found[0].path, part) == 0);
  CHECK(found[0].size == 7U);
  CHECK(transfer_orphan_delete(ordinary) != 0);
  CHECK(transfer_orphan_delete(link_path) != 0);
  CHECK(transfer_orphan_delete(part) == 0);
  CHECK(access(part, F_OK) != 0 && access(ordinary, F_OK) == 0);
  CHECK(transfer_orphans_snapshot(found, TRANSFER_ORPHAN_MAX, &scanning) == 0U);

  transfer_orphans_set_root(NULL);
  CHECK(unlink(link_path) == 0);
  CHECK(unlink(ordinary) == 0);
  CHECK(rmdir(sub) == 0);
  CHECK(rmdir(root) == 0);
  puts("test_transfer_orphans: ok");
  return 0;
}
