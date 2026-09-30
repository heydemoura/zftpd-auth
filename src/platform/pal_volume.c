/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file pal_volume.c
 * @brief Volume label reader for exFAT and FAT12/16/32
 *
 * exFAT keeps the label in a directory entry of the root directory (type
 * 0x83, 11 UTF-16LE characters); FAT keeps it in the boot sector
 * (BS_VolLab, 0x47) with a root directory entry (0x08) as the legacy spot.
 * Both are read from the raw device without mounting anything.
 */

#include "pal_volume.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define VOLUME_SECTOR 512U
#define VOLUME_NAME_CHARS 11U

static uint32_t read_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/** Trim trailing spaces and reject the "NO NAME" placeholder. */
static int copy_fat_label(const char *raw, size_t len, char *out,
                          size_t out_size) {
  while (len > 0U && (raw[len - 1U] == ' ' || raw[len - 1U] == '\0')) len--;
  if (len == 0U) return -1;
  if (len >= 7U && strncmp(raw, "NO NAME", 7U) == 0) return -1;
  if (len + 1U > out_size) len = out_size - 1U;
  memcpy(out, raw, len);
  out[len] = '\0';
  return 0;
}

static int fat_label(const unsigned char *boot, char *out, size_t out_size) {
  return copy_fat_label((const char *)boot + 0x47U, VOLUME_NAME_CHARS, out,
                        out_size);
}

/**
 * exFAT: walk the first directory cluster of the root directory looking for
 * the volume label entry.
 */
static int exfat_label(int fd, const unsigned char *boot, char *out,
                       size_t out_size) {
  unsigned int bytes_per_sector = 1U << boot[0x6CU];
  unsigned int sectors_per_cluster = 1U << boot[0x6DU];
  uint32_t cluster_heap = read_le32(boot + 0x58U);
  uint32_t root_cluster = read_le32(boot + 0x60U);
  if (bytes_per_sector < 512U || bytes_per_sector > 4096U) return -1;
  if (sectors_per_cluster == 0U || root_cluster < 2U) return -1;

  uint64_t root_off = (uint64_t)cluster_heap * bytes_per_sector +
                      (uint64_t)(root_cluster - 2U) * sectors_per_cluster *
                          bytes_per_sector;

  /* Scan the whole first cluster of the root directory: the label entry is
   * usually first, but a formatted volume may place it after the bitmap and
   * upcase table entries. */
  size_t cluster_bytes = (size_t)sectors_per_cluster * bytes_per_sector;
  if (cluster_bytes > 32768U) cluster_bytes = 32768U;
  unsigned char entries[32768U];
  ssize_t got = pread(fd, entries, cluster_bytes, (off_t)root_off);
  if (got <= 0) return -1;
  const size_t have = (size_t)got;

  for (size_t i = 0U; i + 32U <= have; i += 32U) {
    if (entries[i] != 0x83U) continue; /* volume label entry */
    char label[VOLUME_NAME_CHARS + 1U];
    size_t len = 0U;
    for (size_t c = 0U; c < VOLUME_NAME_CHARS; c++) {
      unsigned char lo = entries[i + 2U + c * 2U];
      unsigned char hi = entries[i + 3U + c * 2U];
      if (lo == 0U && hi == 0U) break;
      /* Labels from Windows are ASCII in practice; keep the low byte and
       * substitute '?' for anything outside it. */
      label[len++] = (hi == 0U && lo >= 32U) ? (char)lo : '?';
    }
    return copy_fat_label(label, len, out, out_size);
  }
  return -1;
}

int pal_volume_label(const char *device, char *out, size_t out_size) {
  if (device == NULL || out == NULL || out_size < 2U) return -1;
  out[0] = '\0';

  int fd = open(device, O_RDONLY);
  if (fd < 0) return -1;

  unsigned char boot[VOLUME_SECTOR];
  ssize_t got = pread(fd, boot, sizeof(boot), 0);
  if (got != (ssize_t)sizeof(boot)) {
    close(fd);
    return -1;
  }

  int rc = -1;
  if (memcmp(boot + 0x03U, "EXFAT   ", 8U) == 0) {
    rc = exfat_label(fd, boot, out, out_size);
  } else if (boot[0x1FEU] == 0x55U && boot[0x1FFU] == 0xAAU) {
    rc = fat_label(boot, out, out_size);
  }

  close(fd);
  if (rc != 0) out[0] = '\0';
  return rc;
}
