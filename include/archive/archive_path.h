#ifndef ZFTPD_ARCHIVE_PATH_H
#define ZFTPD_ARCHIVE_PATH_H

#include <stddef.h>

int archive_path_prepare(const char *root, const char *entry, size_t entry_len,
                         int is_directory, char *out, size_t out_size);
int archive_path_is_safe_relative(const char *entry, size_t entry_len);

#endif
