/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file ps5_fw_offsets.h
 * @brief Single source of truth for per-firmware PS5 kernel offsets
 *
 * WHY THIS MODULE EXISTS
 * ----------------------
 * Two consumers need firmware-keyed kernel addresses:
 *
 *   - SELF pager swap  (pal_filesystem_psx.c)  → pager_table_off
 *   - sysent hook      (ps5_net_filter.c)      → sysent_off
 *
 * Each used to carry its own firmware switch, in two different styles, keyed
 * on two different encodings.  Adding a firmware meant editing both, and the
 * two tables were free to drift — they did: the sysent table stopped at
 * 12.70 while the pager switch already covered 13.40.  Both read from the
 * table in ps5_fw_offsets.c now.
 *
 * KEY ENCODING
 * ------------
 * `fw` is decimal major * 100 + minor: 1360 means FW 13.60, which the SDK
 * reports as 0x13600000.  ps5_fw_normalize() converts the raw value returned
 * by kernel_get_fw_version() into this key so callers do no bit arithmetic.
 *
 * ADDING A FIRMWARE
 * -----------------
 *   1. Verify the offsets for the release (kernel dump, kstuff, SDK).
 *   2. Add ONE row to g_ps5_fw_offsets[] in ps5_fw_offsets.c.
 *   3. Leave an offset at 0 when it is not verified: the consumer then
 *      reports "unsupported on this firmware" instead of reading a guessed
 *      address into the kernel.
 *
 * @note This module is platform-neutral (no PS5 headers): it is compiled into
 *       PS5 builds only, but it can be unit-tested on the host.
 */

#ifndef PS5_FW_OFFSETS_H
#define PS5_FW_OFFSETS_H

#include <stdint.h>

/**
 * td->td_proc offset within struct thread.
 *
 * Firmware-invariant on every supported release, matches the ps5-payload-sdk
 * constant of the same meaning.
 */
#define PS5_OFF_THREAD_TD_PROC 0x008U

/**
 * proc->p_pid offset within struct proc (4-byte field).
 *
 * Firmware-invariant, matches KERNEL_OFFSET_PROC_P_PID from the ps5-payload-sdk.
 * This is a PS5 layout: the PS4 value (0x060) points at unrelated bytes here.
 */
#define PS5_OFF_PROC_P_PID 0x0BCU

/** One row per supported firmware. */
typedef struct {
    /** Decimal-encoded version: 1360 == FW 13.60 (see ps5_fw_normalize()). */
    uint32_t fw;

    /** pagertab[] offset from KERNEL_ADDRESS_DATA_BASE, 0 if not verified. */
    uint32_t pager_table_off;

    /** sysent[] offset from KERNEL_ADDRESS_DATA_BASE, 0 if not verified. */
    uint32_t sysent_off;
} ps5_fw_offsets_t;

/**
 * @brief Convert a raw kernel_get_fw_version() value to the table key.
 *
 * The SDK reports the system software version as BCD nibbles, e.g.
 * 0x13600000 for FW 13.60 and 0x04030000 for FW 4.03.
 *
 * @param[in] fw_version  Raw value from kernel_get_fw_version().
 * @return Decimal-encoded major * 100 + minor (e.g. 1360 for 13.60),
 *         or 0 if fw_version carries no version information.
 */
uint32_t ps5_fw_normalize(uint32_t fw_version);

/**
 * @brief Look up the offsets row for a firmware.
 *
 * @param[in] fw_version  Raw value from kernel_get_fw_version().
 * @return Pointer to the matching row, or NULL when the firmware is unknown.
 *
 * @note O(n) linear scan over a fixed table; no allocation, no failure modes
 *       beyond "not found".
 */
const ps5_fw_offsets_t *ps5_fw_offsets_lookup(uint32_t fw_version);

#endif /* PS5_FW_OFFSETS_H */
