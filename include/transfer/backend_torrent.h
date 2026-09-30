#ifndef ZFTPD_BACKEND_TORRENT_H
#define ZFTPD_BACKEND_TORRENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called from the BitTorrent worker. Return -1 to cancel, 1 to pause, or 0
 * to continue. Progress is verified payload, not network protocol overhead. */
typedef int (*transfer_torrent_progress_cb)(void *ctx, uint64_t done,
                                            uint64_t total, uint64_t speed);

int transfer_torrent_validate(const char *uri);
int transfer_torrent_run(const char *uri, const char *save_dir,
                         const char *resume_file,
                         transfer_torrent_progress_cb progress, void *ctx,
                         char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
