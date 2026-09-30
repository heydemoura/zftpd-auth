/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file pal_disc.c
 * @brief Optical drive control for PS4/PS5 (Blu-ray eject)
 *
 * The drive is managed by SceBdSvc and exposed as a FreeBSD cd(4) device:
 *
 *   open("/dev/cd0", O_RDONLY | O_NONBLOCK)
 *     ioctl(fd, CDIOCALLOW)   unlocks the ejection interlock (non-critical)
 *     ioctl(fd, CDIOCEJECT)   SCSI START STOP UNIT with LoEj=1
 *   close(fd)
 *
 * Access needs the post-exploit environment; without it the open fails with
 * EACCES.  Digital Edition consoles have no drive node at all (ENOENT).
 */

#include "pal_disc.h"

#include <errno.h>

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)

#include <fcntl.h>
#include <stddef.h>
#include <sys/cdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* /dev/cd1 is a rare fallback on consoles that enumerate the drive later. */
static const char *const k_disc_devices[] = {"/dev/cd0", "/dev/cd1", NULL};

static int disc_open(void) {
  for (size_t i = 0U; k_disc_devices[i] != NULL; i++) {
    int fd = open(k_disc_devices[i], O_RDONLY | O_NONBLOCK);
    if (fd >= 0) return fd;
  }
  return -1;
}

int pal_disc_present(void) {
  int fd = disc_open();
  if (fd < 0) return 0;
  (void)close(fd);
  return 1;
}

int pal_disc_eject(void) {
  int fd = disc_open();
  if (fd < 0) return -errno;

  (void)ioctl(fd, CDIOCALLOW);
  int rc = ioctl(fd, CDIOCEJECT);
  int saved = (rc == 0) ? 0 : errno;
  (void)close(fd);

  return (saved == 0) ? 0 : -saved;
}

#else /* host builds: nothing to control */

int pal_disc_present(void) { return 0; }
int pal_disc_eject(void) { return -ENOSYS; }

#endif

const char *pal_disc_reason(int err) {
  switch (err) {
  case 0:
    return "Disc ejected";
#ifdef ENOSYS
  case -ENOSYS:
    return "Disc control is not available on this build";
#endif
#ifdef ENOENT
  case -ENOENT:
    return "No Blu-ray drive found (Digital Edition console?)";
#endif
#ifdef ENXIO
  case -ENXIO:
    return "Drive not ready: no disc inserted";
#endif
#ifdef EBUSY
  case -EBUSY:
    return "Drive busy: a disc game is running";
#endif
#ifdef EACCES
  case -EACCES:
    return "Permission denied: zftpd needs the kernel exploit";
#endif
#ifdef EIO
  case -EIO:
    return "The drive reported an I/O error";
#endif
#ifdef EINVAL
  case -EINVAL:
    return "The drive rejected the eject command";
#endif
  default:
    return "Could not eject the disc";
  }
}
