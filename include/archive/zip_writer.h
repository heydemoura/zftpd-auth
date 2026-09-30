/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file zip_writer.h
 * @brief Streaming ZIP writer for bulk downloads
 *
 * The web UI lets the user pick many files (and whole folders) and download
 * them as one archive.  Building the archive on disk first would need as much
 * free space as the selection and would double the I/O, so the entries are
 * produced while the HTTP response streams:
 *
 *   local header → file bytes → … → central directory → EOCD
 *
 * Entries are STORED (method 0): console payloads and media files are already
 * compressed, and skipping deflate keeps the transfer CPU-free.  The writer
 * emits ZIP64 records when an entry, the archive offset or the entry count
 * crosses the 32-bit limits, so multi-gigabyte selections stay valid.
 */

#ifndef ZIP_WRITER_H
#define ZIP_WRITER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/** Upper bound on collected entries; further files are skipped. */
#define ZIP_WRITER_MAX_ENTRIES 4096

typedef struct zip_writer zip_writer_t;

/**
 * @brief Collect @p paths (files or directories, recursed) into a writer.
 *
 * @param[in] paths  Absolute filesystem paths.
 * @param[in] count  Number of paths.
 * @return Writer on success, NULL on allocation failure.
 *
 * @note The archive names are relative to each requested path's parent, so
 *       zipping /data/games yields "games/…" entries.
 */
zip_writer_t *zip_writer_create(const char **paths, size_t count);

/** Number of files/directories collected (the EOCD count). */
size_t zip_writer_entry_count(const zip_writer_t *zip);

/**
 * @brief Stream SELF containers decrypted (MAP_SELF) instead of raw.
 *
 * Must be set before the entries are collected: a decrypted container has a
 * different size, and the archive states every size before streaming.
 * Ignored on platforms without the PS4/PS5 pager.
 */
void zip_writer_set_decrypt(zip_writer_t *zip, int decrypt);

/** Same as zip_writer_create(), with the decryption flag set before the walk. */
zip_writer_t *zip_writer_create_decrypt(const char **paths, size_t count,
                                        int decrypt);

/**
 * @brief Exact size of the archive that zip_writer_read() will produce.
 *
 * Lets the HTTP layer answer with Content-Length instead of chunked encoding:
 * console browsers and download managers show progress and complete
 * reliably only when the length is known.  Computed with the same header
 * builders that emit the archive, so the two always agree.
 */
uint64_t zip_writer_total_size(zip_writer_t *zip);

/** 1 when the selection exceeded ZIP_WRITER_MAX_ENTRIES and was cut short. */
int zip_writer_truncated(const zip_writer_t *zip);

/**
 * @brief Producer callback for the HTTP streaming response.
 *
 * @return Bytes written to @p buf, 0 at end of archive, -1 on error.
 */
ssize_t zip_writer_read(zip_writer_t *zip, void *buf, size_t len);

/** Close the current file and free the writer. Safe on a NULL pointer. */
void zip_writer_destroy(zip_writer_t *zip);

#endif /* ZIP_WRITER_H */
