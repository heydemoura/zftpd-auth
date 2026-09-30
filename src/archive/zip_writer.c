/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file zip_writer.c
 * @brief Streaming ZIP writer (STORED entries, ZIP64 aware)
 *
 * See include/archive/zip_writer.h for the rationale.  The writer is a small
 * state machine driven by zip_writer_read(): header bytes come from an
 * internal buffer, file bytes go straight from pread() into the caller's
 * buffer, and the central directory is emitted after the last entry.
 */

#include "archive/zip_writer.h"

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include "pal_filesystem.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ZIP_LOCAL_SIG 0x04034b50U
#define ZIP_CENTRAL_SIG 0x02014b50U
#define ZIP_EOCD_SIG 0x06054b50U
#define ZIP64_EOCD_SIG 0x06064b50U
#define ZIP64_LOCATOR_SIG 0x07064b50U
#define ZIP64_EXTRA_ID 0x0001U

#define ZIP_METHOD_STORE 0U
#define ZIP_VERSION_STORE 20U
#define ZIP_VERSION_ZIP64 45U
#define ZIP_FLAG_UTF8 0x0800U
#define ZIP_U32_MAX 0xFFFFFFFFU
#define ZIP_U16_MAX 0xFFFFU

#define ZIP_STATE_LOCAL 0
#define ZIP_STATE_FILE 1
#define ZIP_STATE_DESCRIPTOR 2
#define ZIP_STATE_CENTRAL 3
#define ZIP_STATE_ZIP64_EOCD 4
#define ZIP_STATE_EOCD 5
#define ZIP_STATE_DONE 6
#define ZIP_STATE_ERROR 7

#define ZIP_NAME_MAX 1024
#define ZIP_HEADER_MAX 1280

typedef struct {
  char name[ZIP_NAME_MAX]; /* archive path, '/'-separated, dirs end with '/' */
  char disk[ZIP_NAME_MAX + 256];
  uint64_t size;
  uint64_t offset;
  uint32_t crc;
  uint32_t dos_time;
  int is_self; /**< SELF container: read decrypted through the VFS */
} zip_entry_t;

struct zip_writer {
  zip_entry_t *entries;
  size_t count;
  size_t capacity;
  int truncated;

  size_t index;    /* entry currently being emitted */
  int state;
  int fd;          /* open input file for ZIP_STATE_FILE */
  uint64_t sent;   /* bytes of the current file already streamed */
  uint32_t crc;    /* running CRC of the current file */
  int zip64_needed; /* any offset/size/count past the 32-bit limits */

  unsigned char header[ZIP_HEADER_MAX];
  size_t header_len;
  size_t header_pos;
  uint64_t offset;    /* bytes of the archive already produced */
  uint64_t cd_offset; /* offset where the central directory starts */
  int zip64_emitted;  /* tail records built once, then served */
  int eocd_emitted;
  int decrypt;        /* stream SELF containers decrypted (dumps) */
  int read_failed;    /* current entry unreadable: pad its announced size */
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  vfs_node_t self_node;
  int self_open;
#endif
};

void zip_writer_set_decrypt(zip_writer_t *zip, int decrypt) {
  if (zip != NULL) zip->decrypt = decrypt ? 1 : 0;
}

static void close_current_file(zip_writer_t *zip);

/* Header builders, also used to measure the archive before streaming it. */
static size_t build_local_header(zip_writer_t *zip, const zip_entry_t *entry);
static size_t build_central_header(zip_writer_t *zip, const zip_entry_t *entry);
static size_t build_data_descriptor(zip_writer_t *zip,
                                    const zip_entry_t *entry);

/*===========================================================================*
 * CRC-32 (IEEE 802.3, the polynomial ZIP uses)
 *===========================================================================*/

static uint32_t g_crc_table[8][256];
static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;

static void crc_build_table(void) {
  for (uint32_t i = 0U; i < 256U; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c & 1U) ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
    }
    g_crc_table[0][i] = c;
  }
  for (uint32_t i = 0U; i < 256U; i++) {
    uint32_t c = g_crc_table[0][i];
    for (int slice = 1; slice < 8; slice++) {
      c = g_crc_table[0][c & 0xFFU] ^ (c >> 8U);
      g_crc_table[slice][i] = c;
    }
  }
}

static uint32_t crc_update(uint32_t crc, const unsigned char *data, size_t len) {
  (void)pthread_once(&g_crc_once, crc_build_table);
  uint32_t c = crc ^ 0xFFFFFFFFU;
  while (len >= 8U) {
    uint32_t word = (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
                    ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
    c ^= word;
    c = g_crc_table[7][c & 0xFFU] ^ g_crc_table[6][(c >> 8U) & 0xFFU] ^
        g_crc_table[5][(c >> 16U) & 0xFFU] ^ g_crc_table[4][c >> 24U] ^
        g_crc_table[3][data[4]] ^ g_crc_table[2][data[5]] ^
        g_crc_table[1][data[6]] ^ g_crc_table[0][data[7]];
    data += 8U;
    len -= 8U;
  }
  while (len-- > 0U) {
    c = g_crc_table[0][(c ^ *data++) & 0xFFU] ^ (c >> 8U);
  }
  return c ^ 0xFFFFFFFFU;
}

/*===========================================================================*
 * Little-endian helpers
 *===========================================================================*/

static void put16(unsigned char *p, uint16_t v) {
  p[0] = (unsigned char)(v & 0xFFU);
  p[1] = (unsigned char)((v >> 8U) & 0xFFU);
}

static void put32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xFFU);
  p[1] = (unsigned char)((v >> 8U) & 0xFFU);
  p[2] = (unsigned char)((v >> 16U) & 0xFFU);
  p[3] = (unsigned char)((v >> 24U) & 0xFFU);
}

static void put64(unsigned char *p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (unsigned char)((v >> (8 * i)) & 0xFFU);
}

/** MS-DOS date/time, derived from the entry's mtime. */
static void dos_time_from(time_t when, uint32_t *out) {
  struct tm tm_buf;
  if (localtime_r(&when, &tm_buf) == NULL) {
    *out = 0U;
    return;
  }
  int year = tm_buf.tm_year + 1900;
  if (year < 1980) year = 1980;
  uint32_t date = (uint32_t)(((year - 1980) << 9) | ((tm_buf.tm_mon + 1) << 5) |
                             tm_buf.tm_mday);
  uint32_t time = (uint32_t)((tm_buf.tm_hour << 11) | (tm_buf.tm_min << 5) |
                             (tm_buf.tm_sec / 2));
  *out = (date << 16) | time;
}

/*===========================================================================*
 * Entry collection
 *===========================================================================*/

static zip_entry_t *entry_push(zip_writer_t *zip) {
  if (zip->count >= ZIP_WRITER_MAX_ENTRIES) {
    zip->truncated = 1;
    return NULL;
  }
  if (zip->count == zip->capacity) {
    size_t next = (zip->capacity == 0U) ? 64U : zip->capacity * 2U;
    if (next > ZIP_WRITER_MAX_ENTRIES) next = ZIP_WRITER_MAX_ENTRIES;
    zip_entry_t *grown = realloc(zip->entries, next * sizeof(*grown));
    if (grown == NULL) {
      zip->truncated = 1;
      return NULL;
    }
    zip->entries = grown;
    zip->capacity = next;
  }
  zip_entry_t *entry = &zip->entries[zip->count++];
  memset(entry, 0, sizeof(*entry));
  return entry;
}

static int name_append(char *dst, size_t dst_size, const char *prefix,
                       const char *leaf) {
  int n = snprintf(dst, dst_size, "%s%s", prefix, leaf);
  return (n > 0 && (size_t)n < dst_size) ? 0 : -1;
}

static int zip_cmp(const void *a, const void *b) {
  const char *const *lhs = (const char *const *)a;
  const char *const *rhs = (const char *const *)b;
  return strcmp(*lhs, *rhs);
}

/**
 * Add @p disk (file or directory) and, for directories, everything below it.
 *
 * @param prefix Archive path prefix ending with '/' ("" for the top level).
 */
static void collect_path(zip_writer_t *zip, const char *disk, const char *prefix,
                         int depth) {
  if (depth > 32) return; /* symlink loops / absurd nesting */

  struct stat st;
  if (lstat(disk, &st) != 0) return;
  if (S_ISLNK(st.st_mode)) return; /* never follow links out of the tree */

  if (S_ISDIR(st.st_mode)) {
    zip_entry_t *entry = entry_push(zip);
    if (entry == NULL) return;
    if (name_append(entry->name, sizeof(entry->name), prefix, "") != 0) return;
    size_t len = strlen(entry->name);
    if (len > 0U && entry->name[len - 1U] != '/') {
      if (len + 1U >= sizeof(entry->name)) return;
      entry->name[len] = '/';
      entry->name[len + 1U] = '\0';
    }
    /* entry_push() can realloc zip->entries while descending into children.
     * Keep the parent name on this stack frame before recursing. */
    char parent_name[ZIP_NAME_MAX];
    (void)memcpy(parent_name, entry->name, strlen(entry->name) + 1U);
    (void)snprintf(entry->disk, sizeof(entry->disk), "%s", disk);
    entry->size = 0U;
    dos_time_from(st.st_mtime, &entry->dos_time);

    DIR *dir = opendir(disk);
    if (dir == NULL) return;

    char **names = NULL;
    size_t name_count = 0U, name_cap = 0U;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
      if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
      if (name_count == name_cap) {
        size_t next = (name_cap == 0U) ? 16U : name_cap * 2U;
        char **grown = realloc(names, next * sizeof(*grown));
        if (grown == NULL) break;
        names = grown;
        name_cap = next;
      }
      names[name_count] = strdup(de->d_name);
      if (names[name_count] == NULL) break;
      name_count++;
    }
    closedir(dir);

    qsort(names, name_count, sizeof(*names), zip_cmp); /* stable output */

    for (size_t i = 0U; i < name_count; i++) {
      char child_disk[ZIP_NAME_MAX + 256];
      char child_name[ZIP_NAME_MAX];
      int disk_len = snprintf(child_disk, sizeof(child_disk), "%s/%s", disk, names[i]);
      if (disk_len < 0 || (size_t)disk_len >= sizeof(child_disk)) {
        free(names[i]);
        continue;
      }
      if (name_append(child_name, sizeof(child_name), parent_name, names[i]) != 0) {
        free(names[i]);
        continue;
      }
      collect_path(zip, child_disk, child_name, depth + 1);
      free(names[i]);
    }
    free(names);
    return;
  }

  if (!S_ISREG(st.st_mode)) return;

  zip_entry_t *entry = entry_push(zip);
  if (entry == NULL) return;

  /* Files inside a requested folder already carry their full archive path in
   * the prefix; a file requested on its own keeps its own name. */
  char leaf[ZIP_NAME_MAX];
  const char *entry_name = prefix;
  if (prefix[0] == '\0') {
    const char *slash = strrchr(disk, '/');
    (void)snprintf(leaf, sizeof(leaf), "%s",
                   (slash != NULL) ? slash + 1 : disk);
    entry_name = leaf;
  }
  if (name_append(entry->name, sizeof(entry->name), entry_name, "") != 0) return;
  (void)snprintf(entry->disk, sizeof(entry->disk), "%s", disk);
  entry->size = (uint64_t)st.st_size;
  dos_time_from(st.st_mtime, &entry->dos_time);

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  /*
   * Decrypted dumps: a SELF container streams as its ELF payload, and the
   * size must be known here — the archive length is announced before the
   * bytes are produced.
   */
  if (zip->decrypt != 0) {
    vfs_node_t node;
    if (psx_vfs_try_open_self(&node, disk) == 1) {
      entry->size = vfs_get_size(&node);
      entry->is_self = 1;
      vfs_close(&node);
    }
  }
#endif
}

static zip_writer_t *zip_create_impl(const char **paths, size_t count,
                                     int decrypt) {
  if (paths == NULL || count == 0U) return NULL;

  zip_writer_t *zip = calloc(1U, sizeof(*zip));
  if (zip == NULL) return NULL;
  zip->state = ZIP_STATE_LOCAL;
  zip->fd = -1;
  zip->decrypt = (decrypt != 0) ? 1 : 0;

  for (size_t i = 0U; i < count; i++) {
    if (paths[i] == NULL || paths[i][0] == '\0') continue;

    /* Archive names are relative to the parent, so a picked folder keeps its
     * own name as the top level entry. */
    const char *base = strrchr(paths[i], '/');
    base = (base != NULL) ? base + 1 : paths[i];
    if (base[0] == '\0') continue;

    char prefix[ZIP_NAME_MAX];
    if (name_append(prefix, sizeof(prefix), base, "/") != 0) continue;

    struct stat st;
    if (lstat(paths[i], &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      collect_path(zip, paths[i], prefix, 0);
    } else if (S_ISREG(st.st_mode)) {
      collect_path(zip, paths[i], "", 0); /* file: keep its own basename */
    }
  }

  return zip;
}

zip_writer_t *zip_writer_create(const char **paths, size_t count) {
  return zip_create_impl(paths, count, 0);
}

zip_writer_t *zip_writer_create_decrypt(const char **paths, size_t count,
                                        int decrypt) {
  return zip_create_impl(paths, count, decrypt);
}

size_t zip_writer_entry_count(const zip_writer_t *zip) {
  return (zip != NULL) ? zip->count : 0U;
}

uint64_t zip_writer_total_size(zip_writer_t *zip) {
  if (zip == NULL || zip->count == 0U) return 0U;

  /* Dry run: the same builders that emit the archive measure it, so the
   * announced length cannot drift from the bytes produced.  Only offsets and
   * sizes matter here, and those are known before any file is read. */
  uint64_t offset = 0U;

  for (size_t i = 0U; i < zip->count; i++) {
    zip_entry_t *entry = &zip->entries[i];
    entry->offset = offset;

    size_t local = build_local_header(zip, entry);
    offset += (uint64_t)local + entry->size;
    if (entry->size > 0U) {
      offset += (uint64_t)build_data_descriptor(zip, entry);
    }
    if (entry->size >= ZIP_U32_MAX || entry->offset >= ZIP_U32_MAX) {
      zip->zip64_needed = 1;
    }
  }

  uint64_t cd_offset = offset;
  for (size_t i = 0U; i < zip->count; i++) {
    offset += (uint64_t)build_central_header(zip, &zip->entries[i]);
  }

  int needs_zip64 = zip->zip64_needed || zip->count >= ZIP_U16_MAX ||
                    offset >= ZIP_U32_MAX;
  if (needs_zip64 != 0) offset += 76U; /* zip64 EOCD + locator */
  offset += 22U;                       /* classic EOCD */

  zip->cd_offset = cd_offset;
  return offset;
}

int zip_writer_truncated(const zip_writer_t *zip) {
  return (zip != NULL) ? zip->truncated : 0;
}

void zip_writer_destroy(zip_writer_t *zip) {
  if (zip == NULL) return;
  close_current_file(zip);
  free(zip->entries);
  free(zip);
}

/*===========================================================================*
 * Header emission
 *===========================================================================*/

static size_t build_local_header(zip_writer_t *zip, const zip_entry_t *entry) {
  unsigned char *p = zip->header;
  size_t name_len = strlen(entry->name);
  int zip64 = (entry->size >= ZIP_U32_MAX);
  /* The CRC is only known once the bytes streamed, so non-empty entries carry
   * the "data descriptor follows" flag and repeat the values after the data. */
  int descriptor = (entry->size > 0U);
  size_t extra_len = zip64 ? 20U : 0U;

  put32(p + 0, ZIP_LOCAL_SIG);
  put16(p + 4, zip64 ? ZIP_VERSION_ZIP64 : ZIP_VERSION_STORE);
  put16(p + 6, (uint16_t)(ZIP_FLAG_UTF8 | (descriptor ? 0x0008U : 0U)));
  put16(p + 8, ZIP_METHOD_STORE);
  put32(p + 10, entry->dos_time);
  put32(p + 14, 0U); /* CRC only in the descriptor when streaming */
  put32(p + 18, zip64 ? ZIP_U32_MAX : (uint32_t)entry->size);
  put32(p + 22, zip64 ? ZIP_U32_MAX : (uint32_t)entry->size);
  put16(p + 26, (uint16_t)name_len);
  put16(p + 28, (uint16_t)extra_len);

  memcpy(p + 30, entry->name, name_len);
  size_t pos = 30U + name_len;
  if (zip64) {
    put16(p + pos, ZIP64_EXTRA_ID);
    put16(p + pos + 2, 16U);
    put64(p + pos + 4, entry->size);
    put64(p + pos + 12, entry->size);
    pos += 20U;
  }

  return pos;
}

/** Data descriptor written after a streamed entry (bit 3 in the flags). */
static size_t build_data_descriptor(zip_writer_t *zip, const zip_entry_t *entry) {
  unsigned char *p = zip->header;
  int zip64 = (entry->size >= ZIP_U32_MAX);

  put32(p + 0, 0x08074b50U);
  put32(p + 4, entry->crc);
  if (zip64) {
    put64(p + 8, entry->size);
    put64(p + 16, entry->size);
    return 24U;
  }
  put32(p + 8, (uint32_t)entry->size);
  put32(p + 12, (uint32_t)entry->size);
  return 16U;
}

static size_t build_central_header(zip_writer_t *zip, const zip_entry_t *entry) {
  unsigned char *p = zip->header;
  size_t name_len = strlen(entry->name);
  uint64_t offset = entry->offset;
  int zip64 = (entry->size >= ZIP_U32_MAX) || (offset >= ZIP_U32_MAX);
  size_t extra_len = 0U;
  size_t pos;

  put32(p + 0, ZIP_CENTRAL_SIG);
  put16(p + 4, zip64 ? ZIP_VERSION_ZIP64 : ZIP_VERSION_STORE); /* made by  */
  put16(p + 6, zip64 ? ZIP_VERSION_ZIP64 : ZIP_VERSION_STORE); /* needed   */
  put16(p + 8, ZIP_FLAG_UTF8);
  put16(p + 10, ZIP_METHOD_STORE);
  put32(p + 12, entry->dos_time);
  put32(p + 16, entry->crc);
  put32(p + 20, entry->size >= ZIP_U32_MAX ? ZIP_U32_MAX : (uint32_t)entry->size);
  put32(p + 24, entry->size >= ZIP_U32_MAX ? ZIP_U32_MAX : (uint32_t)entry->size);
  put16(p + 28, (uint16_t)name_len);
  put16(p + 30, 0U); /* extra length, patched below */
  put16(p + 32, 0U); /* comment length */
  put16(p + 34, 0U); /* disk number */
  put16(p + 36, 0U); /* internal attributes */
  put32(p + 38, 0U); /* external attributes */
  put32(p + 42, offset >= ZIP_U32_MAX ? ZIP_U32_MAX : (uint32_t)offset);

  memcpy(p + 46, entry->name, name_len);
  pos = 46U + name_len;

  if (zip64) {
    size_t field = 0U;
    field += (entry->size >= ZIP_U32_MAX) ? 8U : 0U;
    field += (entry->size >= ZIP_U32_MAX) ? 8U : 0U;
    field += (offset >= ZIP_U32_MAX) ? 8U : 0U;

    put16(p + pos, ZIP64_EXTRA_ID);
    put16(p + pos + 2, (uint16_t)field);
    size_t q = pos + 4U;
    if (entry->size >= ZIP_U32_MAX) {
      put64(p + q, entry->size);
      q += 8U;
      put64(p + q, entry->size);
      q += 8U;
    }
    if (offset >= ZIP_U32_MAX) {
      put64(p + q, offset);
      q += 8U;
    }
    extra_len = 4U + field;
    pos = q;
    put16(p + 30, (uint16_t)extra_len);
    zip->zip64_needed = 1;
  }

  return pos;
}

/*===========================================================================*
 * Producer
 *===========================================================================*/

static int open_next_file(zip_writer_t *zip, const zip_entry_t *entry) {
  if (zip->fd >= 0) {
    close(zip->fd);
    zip->fd = -1;
  }
  zip->sent = 0U;
  zip->crc = 0U;

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (entry->is_self != 0 && zip->decrypt != 0) {
    if (psx_vfs_try_open_self(&zip->self_node, entry->disk) == 1) {
      zip->self_open = 1;
      return 0;
    }
    return -1; /* the container stopped being readable: skip it */
  }
#endif

  zip->fd = open(entry->disk, O_RDONLY);
  return (zip->fd >= 0) ? 0 : -1;
}

/** True when the current entry is already being read through the VFS. */
static int zip_self_open(const zip_writer_t *zip) {
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  return (zip->self_open != 0);
#else
  (void)zip;
  return 0;
#endif
}

/** Release whatever the current entry had open. */
static void close_current_file(zip_writer_t *zip) {
  if (zip->fd >= 0) {
    close(zip->fd);
    zip->fd = -1;
  }
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (zip->self_open != 0) {
    vfs_close(&zip->self_node);
    zip->self_open = 0;
  }
#endif
}

static size_t serve_header(zip_writer_t *zip, void *buf, size_t len) {
  size_t available = zip->header_len - zip->header_pos;
  size_t take = (len < available) ? len : available;
  memcpy(buf, zip->header + zip->header_pos, take);
  zip->header_pos += take;
  zip->offset += take;
  return take;
}

/**
 * Close the current entry: record its CRC and queue the data descriptor that
 * carries the values the local header had to leave blank.
 */
static int finish_entry(zip_writer_t *zip) {
  zip_entry_t *entry = &zip->entries[zip->index];
  entry->crc = zip->crc;
  close_current_file(zip);
  if (entry->size >= ZIP_U32_MAX || entry->offset >= ZIP_U32_MAX) {
    zip->zip64_needed = 1;
  }
  zip->header_len = (entry->size > 0U) ? build_data_descriptor(zip, entry) : 0U;
  zip->header_pos = 0U;
  zip->state = ZIP_STATE_DESCRIPTOR;
  return 0;
}

ssize_t zip_writer_read(zip_writer_t *zip, void *buf, size_t len) {
  if (zip == NULL || buf == NULL || len == 0U) return -1;
  unsigned char *out = (unsigned char *)buf;
  size_t produced = 0U;

  while (produced < len) {
    switch (zip->state) {
    case ZIP_STATE_LOCAL: {
      if (zip->index >= zip->count) {
        zip->cd_offset = zip->offset; /* central directory starts here */
        zip->state = ZIP_STATE_CENTRAL;
        zip->index = 0U;
        continue;
      }
      zip_entry_t *entry = &zip->entries[zip->index];
      entry->offset = zip->offset;
      zip->header_len = build_local_header(zip, entry);
      zip->header_pos = 0U;
      zip->sent = 0U; /* per-entry state: never inherit the previous file */
      zip->crc = 0U;
      zip->read_failed = 0;
      zip->state = ZIP_STATE_FILE;
      break;
    }

    case ZIP_STATE_FILE: {
      zip_entry_t *entry = &zip->entries[zip->index];

      if ((zip->header_pos == 0U) && (zip->fd < 0) && !zip_self_open(zip) &&
          (zip->read_failed == 0) && (entry->size > 0U) &&
          (open_next_file(zip, entry) != 0)) {
        if (entry->is_self != 0 && zip->decrypt != 0) {
          return -1; /* never present an encrypted SELF as an all-zero ELF */
        }
        /*
         * Unreadable file. The length of this archive was announced before
         * streaming, so the entry cannot simply disappear: it is padded with
         * zeros to the size stated in the headers.  A client then gets a
         * complete, well-formed archive instead of a truncated transfer.
         */
        zip->read_failed = 1;
      }

      if (zip->header_pos < zip->header_len) {
        produced += serve_header(zip, out + produced, len - produced);
        continue;
      }

      uint64_t remaining = entry->size - zip->sent;
      if (remaining == 0U) {
        (void)finish_entry(zip);
        continue;
      }

      if (zip->read_failed != 0) {
        size_t pad = (size_t)((remaining < (uint64_t)(len - produced))
                                  ? remaining
                                  : (uint64_t)(len - produced));
        memset(out + produced, 0, pad);
        zip->crc = crc_update(zip->crc, out + produced, pad);
        zip->sent += (uint64_t)pad;
        zip->offset += (uint64_t)pad;
        produced += pad;
        break;
      }

      size_t want = (size_t)((remaining < (uint64_t)(len - produced))
                                 ? remaining
                                 : (uint64_t)(len - produced));
      ssize_t got;
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
      if (zip->self_open != 0) {
        got = psx_vfs_read(&zip->self_node, out + produced, want);
      } else {
        got = pread(zip->fd, out + produced, want, (off_t)zip->sent);
      }
#else
      got = pread(zip->fd, out + produced, want, (off_t)zip->sent);
#endif
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) {
        if (entry->is_self != 0 && zip->decrypt != 0) {
          if (got == 0) errno = EIO;
          return -1;
        }
        /* Premature end (file shrank, SELF became unreadable): pad the rest of
         * the announced size so the archive length stays exact. */
        zip->read_failed = 1;
        continue;
      }
      zip->crc = crc_update(zip->crc, out + produced, (size_t)got);
      zip->sent += (uint64_t)got;
      zip->offset += (uint64_t)got;
      produced += (size_t)got;
      break;
    }

    case ZIP_STATE_DESCRIPTOR: {
      if (zip->header_pos < zip->header_len) {
        produced += serve_header(zip, out + produced, len - produced);
        continue;
      }
      zip->index++;
      zip->state = ZIP_STATE_LOCAL;
      break;
    }

    case ZIP_STATE_CENTRAL: {
      if (zip->header_pos < zip->header_len) {
        produced += serve_header(zip, out + produced, len - produced);
        continue;
      }
      if (zip->index >= zip->count) {
        zip->state = ZIP_STATE_ZIP64_EOCD;
        continue;
      }
      zip->header_len = build_central_header(zip, &zip->entries[zip->index]);
      zip->header_pos = 0U;
      zip->index++;
      break;
    }

    case ZIP_STATE_ZIP64_EOCD: {
      if (zip->zip64_emitted == 0) {
        zip->zip64_emitted = 1;
        int needs_zip64 = zip->zip64_needed || zip->count >= ZIP_U16_MAX ||
                          zip->offset >= ZIP_U32_MAX;
        unsigned char *p = zip->header;
        size_t cd_size = (size_t)(zip->offset - zip->cd_offset);

        if (needs_zip64 != 0) {
          put32(p + 0, ZIP64_EOCD_SIG);
          put64(p + 4, 44U);
          put16(p + 12, ZIP_VERSION_ZIP64);
          put16(p + 14, ZIP_VERSION_ZIP64);
          put32(p + 16, 0U);
          put32(p + 20, 0U);
          put64(p + 24, (uint64_t)zip->count);
          put64(p + 32, (uint64_t)zip->count);
          put64(p + 40, (uint64_t)cd_size);
          put64(p + 48, zip->cd_offset);
          put32(p + 56, ZIP64_LOCATOR_SIG);
          put32(p + 60, 0U);
          put64(p + 64, zip->offset); /* offset of the ZIP64 EOCD itself */
          put32(p + 72, 1U);
          zip->header_len = 76U;
        } else {
          zip->header_len = 0U;
        }
        zip->header_pos = 0U;
      }

      if (zip->header_pos < zip->header_len) {
        produced += serve_header(zip, out + produced, len - produced);
        continue;
      }
      zip->state = ZIP_STATE_EOCD;
      break;
    }

    case ZIP_STATE_EOCD: {
      if (zip->eocd_emitted == 0) {
        zip->eocd_emitted = 1;
        unsigned char *p = zip->header;
        uint64_t cd_size = zip->offset - zip->cd_offset;
        /* "zip64" here means the zip64 EOCD was written, so the classic
         * record carries the 0xFFFF/0xFFFFFFFF sentinels. */
        int zip64 = zip->zip64_emitted != 0 && zip->header_len == 76U;

        put32(p + 0, ZIP_EOCD_SIG);
        put16(p + 4, 0U);
        put16(p + 6, 0U);
        put16(p + 8, zip64 ? ZIP_U16_MAX : (uint16_t)zip->count);
        put16(p + 10, zip64 ? ZIP_U16_MAX : (uint16_t)zip->count);
        put32(p + 12, (zip64 || cd_size >= ZIP_U32_MAX) ? ZIP_U32_MAX
                                                        : (uint32_t)cd_size);
        put32(p + 16, (zip64 || zip->cd_offset >= ZIP_U32_MAX)
                          ? ZIP_U32_MAX
                          : (uint32_t)zip->cd_offset);
        put16(p + 20, 0U);
        zip->header_len = 22U;
        zip->header_pos = 0U;
      }

      if (zip->header_pos < zip->header_len) {
        produced += serve_header(zip, out + produced, len - produced);
        continue;
      }
      zip->state = ZIP_STATE_DONE;
      break;
    }

    case ZIP_STATE_DONE:
    case ZIP_STATE_ERROR:
    default:
      return (ssize_t)produced;

    }
  }

  return (ssize_t)produced;
}
