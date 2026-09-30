/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file ps5_fw_offsets.c
 * @brief Per-firmware PS5 kernel offsets — the only firmware table in zftpd
 *
 * OFFSET PROVENANCE
 * -----------------
 *   pager_table_off  pagertab[OBJT_VNODE]/[OBJT_SELF] slot table used for the
 *                    MAP_SELF pager swap.  Same values as
 *                    ps5-payload-dev/ftpsrv and idlesauce/ps5-self-pager.
 *
 *   sysent_off       sysent[] base patched by the net filter.  Same values as
 *                    the public kstuff offset tables.
 *
 *   Both are relative to KERNEL_ADDRESS_DATA_BASE (the kdata base supplied to
 *   the payload by the loader), not to the kernel .text base.
 *
 * A row is only added once its offsets are verified against a kernel image or
 * a published table.  An offset left at 0 means "not verified for this
 * release": the pager falls back to errno ENOSYS and the net filter refuses
 * the install, both of which are safe.  A guessed address is not.
 */

#include "ps5_fw_offsets.h"

#include <stddef.h>

#define PS5_FW_OFFSETS_COUNT \
    (sizeof(g_ps5_fw_offsets) / sizeof(g_ps5_fw_offsets[0]))

/**
 * Firmware table — the single source of truth for firmware-keyed offsets.
 *
 * Columns: firmware | pagertab | sysent
 *
 * Keep rows in ascending firmware order: ps5_fw_offsets_lookup() relies on the
 * ordering only for readability, but tests/test_ps5_fw_offsets.c asserts it.
 */
static const ps5_fw_offsets_t g_ps5_fw_offsets[] = {
    /* --- FW 1.xx --- */
    {100, 0xC27C40U, 0U},
    {101, 0xC27C40U, 0U},
    {102, 0xC27C40U, 0U},
    {105, 0xC27C40U, 0U},
    {110, 0xC27C40U, 0U},
    {111, 0xC27C40U, 0U},
    {112, 0xC27C40U, 0U},
    {113, 0xC27CA0U, 0U},
    {114, 0xC27CA0U, 0U},

    /* --- FW 2.xx --- */
    {200, 0xC4EF60U, 0U},
    {220, 0xC4EFA0U, 0U},
    {225, 0xC4EFA0U, 0U},
    {226, 0xC4EFA0U, 0U},
    {230, 0xC4F120U, 0U},
    {250, 0xC4F120U, 0U},
    {270, 0xC4F120U, 0U},

    /* --- FW 3.xx --- */
    {300, 0xCAF8C0U, 0U},
    {310, 0xCAF8C0U, 0U},
    {320, 0xCAF8C0U, 0U},
    {321, 0xCAF8C0U, 0U},

    /* --- FW 4.xx --- */
    {400, 0xD20840U, 0U},
    {402, 0xD20840U, 0U},
    {403, 0xD20840U, 0x1709C0U},
    {450, 0xD20840U, 0U},
    {451, 0xD20840U, 0U},

    /* --- FW 5.xx --- */
    {500, 0xE0FEF0U, 0U},
    {502, 0xE0FEF0U, 0U},
    {510, 0xE0FEF0U, 0U},
    {550, 0xE0FEF0U, 0U},

    /* --- FW 6.xx --- */
    {600, 0xE30410U, 0U},
    {602, 0xE30410U, 0U},
    {650, 0xE30410U, 0U},

    /* --- FW 7.xx --- */
    {700, 0xE310C0U, 0x1B7030U},
    {701, 0xE310C0U, 0U},
    {720, 0xE31180U, 0U},
    {740, 0xE31180U, 0U},
    {760, 0xE31180U, 0U},
    {761, 0xE31180U, 0x1B7260U},

    /* --- FW 8.xx --- */
    {800, 0xE31250U, 0U},
    {820, 0xE31250U, 0x1A7DB0U},
    {840, 0xE31250U, 0U},
    {860, 0xE31250U, 0x1A7DB0U},

    /* --- FW 9.xx --- */
    {900, 0xDE0420U, 0x1AAC10U},
    {905, 0xDE0420U, 0x1AAC10U},
    {920, 0xDE0420U, 0x1AAC60U},
    {940, 0xDE0420U, 0x1AAC60U},
    {960, 0xDE0420U, 0x1AAC60U},

    /* --- FW 10.xx --- */
    {1000, 0xDE04F0U, 0x1AD100U},
    {1001, 0xDE04F0U, 0x1AD100U},
    {1020, 0xDE04F0U, 0x1AD120U},
    {1040, 0xDE04F0U, 0x1AD120U},
    {1060, 0xDE04F0U, 0x1AD120U},

    /* --- FW 11.xx --- */
    {1100, 0xDF1940U, 0x1B0B70U},
    {1120, 0xDF1940U, 0x1B0B70U},
    {1140, 0xDF1960U, 0x1B0B20U},
    {1160, 0xDF1960U, 0x1B08E0U},

    /* --- FW 12.xx --- */
    {1200, 0xDF2860U, 0x1AF4D0U},
    {1202, 0xDF2860U, 0x1AF4D0U},
    {1220, 0xDF2860U, 0x1AF4D0U},
    {1240, 0xDF2860U, 0x1AF4D0U},
    {1260, 0xDF2860U, 0x1AF4D0U},
    {1270, 0xDF2860U, 0x1AF4D0U},

    /* --- FW 13.xx --- */
    {1300, 0xE038D0U, 0x1B6A60U},
    {1320, 0xE038D0U, 0x1B6AC0U},
    {1340, 0xE03910U, 0x1B6D30U},
    {1342, 0xE03910U, 0x1B6D30U},
    {1360, 0xE03910U, 0x1B6E50U},
};

uint32_t ps5_fw_normalize(uint32_t fw_version)
{
    uint32_t major_minor = (fw_version >> 16U) & 0xFFFFU;

    if (major_minor == 0U) {
        return 0U;
    }

    /* BCD nibbles: 0x1360 -> major 13, minor 60 */
    uint32_t major = (((major_minor >> 12U) & 0xFU) * 10U) +
                     ((major_minor >> 8U) & 0xFU);
    uint32_t minor = (((major_minor >> 4U) & 0xFU) * 10U) +
                     (major_minor & 0xFU);

    return (major * 100U) + minor;
}

const ps5_fw_offsets_t *ps5_fw_offsets_lookup(uint32_t fw_version)
{
    uint32_t key = ps5_fw_normalize(fw_version);

    if (key == 0U) {
        return NULL;
    }

    for (size_t i = 0U; i < PS5_FW_OFFSETS_COUNT; i++) {
        if (g_ps5_fw_offsets[i].fw == key) {
            return &g_ps5_fw_offsets[i];
        }
    }

    return NULL;
}
