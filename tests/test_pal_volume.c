#include "pal_volume.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { \
  if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return 1; \
  } \
} while (0)

static void put16(unsigned char *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8U);
}

static void put32(unsigned char *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8U);
  p[2] = (uint8_t)(v >> 16U);
  p[3] = (uint8_t)(v >> 24U);
}

/** exFAT: boot sector + a root directory holding the volume label entry. */
static int test_exfat_label(void) {
  unsigned char image[1024];
  memset(image, 0, sizeof(image));

  memcpy(image + 0x03, "EXFAT   ", 8U);
  put32(image + 0x58, 1U);  /* cluster heap starts at sector 1 */
  put32(image + 0x60, 2U);  /* root directory is the first cluster */
  image[0x6C] = 9U;         /* 512 byte sectors */
  image[0x6D] = 0U;         /* 1 sector per cluster */

  unsigned char *entry = image + 512;
  entry[0] = 0x83U; /* volume label entry */
  const char *label = "SanDisk";
  for (size_t i = 0U; label[i] != '\0'; i++) {
    put16(entry + 2U + i * 2U, (uint16_t)(unsigned char)label[i]);
  }

  char path[] = "/tmp/zftpd-exfat-XXXXXX";
  int fd = mkstemp(path);
  CHECK(fd >= 0);
  CHECK(write(fd, image, sizeof(image)) == (ssize_t)sizeof(image));
  CHECK(close(fd) == 0);

  char out[64];
  CHECK(pal_volume_label(path, out, sizeof(out)) == 0);
  CHECK(strcmp(out, "SanDisk") == 0);

  CHECK(unlink(path) == 0);
  return 0;
}

/** FAT: the label lives in the boot sector (BS_VolLab). */
static int test_fat_label(void) {
  unsigned char image[512];
  memset(image, 0, sizeof(image));
  memcpy(image + 0x47, "KINGSTON   ", 11U);
  image[0x1FEU] = 0x55U;
  image[0x1FFU] = 0xAAU;

  char path[] = "/tmp/zftpd-fat-XXXXXX";
  int fd = mkstemp(path);
  CHECK(fd >= 0);
  CHECK(write(fd, image, sizeof(image)) == (ssize_t)sizeof(image));
  CHECK(close(fd) == 0);

  char out[64];
  CHECK(pal_volume_label(path, out, sizeof(out)) == 0);
  CHECK(strcmp(out, "KINGSTON") == 0);

  /* The generic placeholder Windows writes must not become a place name. */
  memcpy(image + 0x47, "NO NAME    ", 11U);
  fd = open(path, O_WRONLY);
  CHECK(fd >= 0);
  CHECK(pwrite(fd, image, sizeof(image), 0) == (ssize_t)sizeof(image));
  CHECK(close(fd) == 0);
  CHECK(pal_volume_label(path, out, sizeof(out)) != 0);

  CHECK(unlink(path) == 0);
  return 0;
}

static int test_missing_device(void) {
  char out[64];
  CHECK(pal_volume_label("/tmp/zftpd-no-such-device", out, sizeof(out)) != 0);
  CHECK(out[0] == '\0');
  return 0;
}

int main(void) {
  CHECK(test_exfat_label() == 0);
  CHECK(test_fat_label() == 0);
  CHECK(test_missing_device() == 0);
  puts("test_pal_volume: ok");
  return 0;
}
