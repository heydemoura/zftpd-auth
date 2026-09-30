/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file pal_volume.h
 * @brief Filesystem volume labels (exFAT/FAT) for removable volumes
 *
 * The console mounts every USB stick on /mnt/usbN, so the mountpoint says
 * nothing about the device.  The label the user set on the stick (e.g.
 * "SanDisk") lives inside the filesystem, and reading it is enough to label
 * the volume in the UI.
 *
 * Plugins read the raw device exported by statfs() (e.g. /dev/da0s1); the
 * parser only needs a readable file, which is what makes it testable on the
 * host with synthetic images.
 */

#ifndef PAL_VOLUME_H
#define PAL_VOLUME_H

#include <stddef.h>

/**
 * @brief Read the volume label of an exFAT or FAT filesystem.
 *
 * @param[in]  device  Device or image to inspect.
 * @param[out] out     Label buffer, always NUL terminated on success.
 * @param[in]  out_size Size of @p out (12 bytes are enough for exFAT/FAT).
 *
 * @return 0 when a non-empty label was found, -1 otherwise.
 */
int pal_volume_label(const char *device, char *out, size_t out_size);

#endif /* PAL_VOLUME_H */
