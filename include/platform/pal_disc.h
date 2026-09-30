/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file pal_disc.h
 * @brief Platform Abstraction Layer — optical drive control
 *
 * Ported from the author's BD-EJ payload: the Blu-ray drive is a standard
 * FreeBSD cd(4) device, so unloading the tray is open("/dev/cd0") plus the
 * CDIOCALLOW / CDIOCEJECT ioctls handled by SceBdSvc.  Exposing it here lets
 * the console be controlled from the web UI like the other system actions.
 *
 * PLATFORMS: PS4/PS5.  Every other target reports "not available" so callers
 * never need platform conditionals.
 */

#ifndef PAL_DISC_H
#define PAL_DISC_H

/**
 * @brief Eject the Blu-ray disc.
 *
 * @return 0 on success, a negative errno otherwise (-ENOSYS when the platform
 *         has no optical drive support).
 *
 * @note CDIOCALLOW is attempted first and its failure ignored: some drives
 *       keep the software ejection interlock engaged, some ignore the call.
 */
int pal_disc_eject(void);

/**
 * @brief Whether the console exposes an optical drive.
 *
 * @return 1 when /dev/cd0 (or the fallback node) can be opened, 0 otherwise.
 *         Digital Edition consoles always answer 0.
 */
int pal_disc_present(void);

/**
 * @brief Human readable description of a pal_disc_eject() result.
 *
 * @param[in] err  0 or the negative errno returned by pal_disc_eject().
 * @return Static string, never NULL.
 */
const char *pal_disc_reason(int err);

#endif /* PAL_DISC_H */
