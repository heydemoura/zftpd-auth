/**
 * @file pal_dns_filter.h
 * @brief Userland DNS filter for the Sony CDN domains.
 *
 * The kernel-level net filter needs an executable kernel page, which some
 * firmwares refuse to hand out.  This listener achieves the same goal in
 * userland: the console is pointed at this resolver, the Sony CDN names answer
 * with 0.0.0.0 (a black hole) and everything else is forwarded upstream.
 *
 * The filter is inert until the console's DNS server is set to this host, and
 * it is harmless when port 53 is already taken (the listener reports that and
 * step aside).
 */

#ifndef PAL_DNS_FILTER_H
#define PAL_DNS_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Shell-style wildcard match, case-insensitive.
 *
 * Supports `*` (any run, including empty) and `?` (exactly one character),
 * which covers the mask shapes used by the Sony CDN lists.
 */
bool pal_dns_mask_match(const char *mask, const char *name);

/**
 * @brief Resolve a queried name to the override address.
 *
 * @param[out] ip Receives the 4 bytes to answer with (0.0.0.0).
 * @return true when the name is blackholed.
 */
bool pal_dns_override_for(const char *name, uint8_t ip[4]);

/**
 * @brief Start the UDP/TCP DNS listeners on port 53.
 *
 * @return 0 when the listeners are running, -1 otherwise (logged).
 */
int pal_dns_filter_start(void);

/**
 * @brief Stop the listeners and release the port.
 */
void pal_dns_filter_stop(void);

/** @return true while the listeners are bound. */
bool pal_dns_filter_running(void);

/** @return true when the user disabled the filter from the settings. */
bool pal_dns_filter_enabled(void);

/** @return true when another resolver is configured on this unit. */
bool pal_dns_filter_other_resolver_present(void);

/** @return Number of blackholed masks / forwarded exceptions. */
size_t pal_dns_filter_mask_count(void);
size_t pal_dns_filter_exception_count(void);

/**
 * @brief Enable or disable the filter and remember the choice.
 *
 * @return 0 on success, -1 when the listeners could not be started.
 */
int pal_dns_filter_set_enabled(bool enabled);

#endif /* PAL_DNS_FILTER_H */
