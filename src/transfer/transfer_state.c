/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file transfer_state.c
 * @brief On-disk record of unfinished downloads
 *
 * WHY
 * ---
 * The download queue lives in memory, so re-injecting the payload would lose
 * every queued download.  The partial files stay on disk, so recording which
 * jobs were still unfinished (and where they were headed) lets the next
 * instance pick them up and continue from the bytes already downloaded.
 *
 * FORMAT
 * ------
 * Plain text, one job per line, fields separated by TABs:
 *
 *   zftpd-transfers 2
 *   <id>	<status>	<total>	<downloaded>	<url>	<dst>	<filename>	<error>
 *
 * URLs and paths are escaped (\\, \t, \n, \r) so a value can never break the
 * record layout.  A malformed or truncated line is skipped: a corrupt state
 * file must never keep the daemon from starting.
 *
 * State changes are written atomically. The partial file size is checked once
 * during restore, since it is authoritative after a crash or buffered write.
 * Version 1 files remain readable.
 */

#include "transfer_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** Header line identifying the file; a mismatch discards the whole file. */
static const char k_state_magic[] = "zftpd-transfers 2";
static const char k_legacy_magic[] = "zftpd-transfers 1";

/** Directories probed for the state file, in order. */
static const char *const k_state_dirs[] = {"/data/zftpd", "/tmp/zftpd", NULL};

/** Overrides the probe list; used by the unit tests. */
static const char *g_state_dir = NULL;

void transfer_state_set_dir(const char *dir) { g_state_dir = dir; }

int transfer_state_path(char *out, size_t out_size) {
  if (out == NULL || out_size < 2U) return -1;

  if (g_state_dir != NULL) {
    if (mkdir(g_state_dir, 0755) != 0 && errno != EEXIST) return -1;
    int n = snprintf(out, out_size, "%s/transfers.state", g_state_dir);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
  }

  for (size_t i = 0U; k_state_dirs[i] != NULL; i++) {
    if (mkdir(k_state_dirs[i], 0755) != 0 && errno != EEXIST) continue;
    if (access(k_state_dirs[i], W_OK) != 0) continue;
    int n = snprintf(out, out_size, "%s/transfers.state", k_state_dirs[i]);
    if (n > 0 && (size_t)n < out_size) return 0;
  }

  return -1;
}

static void write_escaped(FILE *fp, const char *value) {
  for (const char *p = value; *p != '\0'; p++) {
    switch (*p) {
    case '\\':
      fputs("\\\\", fp);
      break;
    case '\t':
      fputs("\\t", fp);
      break;
    case '\n':
      fputs("\\n", fp);
      break;
    case '\r':
      fputs("\\r", fp);
      break;
    default:
      fputc(*p, fp);
      break;
    }
  }
}

static void read_escaped(char *dst, size_t dst_size, const char *src) {
  size_t pos = 0U;
  if (dst_size == 0U) return;

  while (*src != '\0' && pos + 1U < dst_size) {
    if (*src != '\\') {
      dst[pos++] = *src++;
      continue;
    }
    src++;
    switch (*src) {
    case '\\':
      dst[pos++] = '\\';
      break;
    case 't':
      dst[pos++] = '\t';
      break;
    case 'n':
      dst[pos++] = '\n';
      break;
    case 'r':
      dst[pos++] = '\r';
      break;
    case '\0':
      dst[pos] = '\0';
      return;
    default:
      dst[pos++] = *src;
      break;
    }
    if (*src != '\0') src++;
  }

  dst[pos] = '\0';
}

int transfer_state_write(const char *path, const transfer_state_entry_t *entries,
                         size_t count) {
  if (path == NULL || entries == NULL) return -1;

  char temp[TRANSFER_STATE_PATH_MAX + 8U];
  int n = snprintf(temp, sizeof(temp), "%s.tmp", path);
  if (n < 0 || (size_t)n >= sizeof(temp)) return -1;
  int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  (void)fchmod(fd, 0600);
  FILE *fp = fdopen(fd, "w");
  if (fp == NULL) {
    (void)close(fd);
    (void)unlink(temp);
    return -1;
  }

  fputs(k_state_magic, fp);
  fputc('\n', fp);
  for (size_t i = 0U; i < count; i++) {
    char id[16];
    (void)snprintf(id, sizeof(id), "%d", entries[i].id);
    fputs(id, fp);
    (void)fprintf(fp, "\t%c\t%" PRIu64 "\t%" PRIu64 "\t",
                  entries[i].status != '\0' ? entries[i].status :
                  (entries[i].paused ? 'P' : 'A'),
                  entries[i].total_size, entries[i].downloaded);
    write_escaped(fp, entries[i].url);
    fputc('\t', fp);
    write_escaped(fp, entries[i].dst);
    fputc('\t', fp);
    write_escaped(fp, entries[i].filename);
    fputc('\t', fp);
    write_escaped(fp, entries[i].error_msg);
    fputc('\n', fp);
  }

  int rc = fflush(fp);
  if (rc == 0) rc = fsync(fd);
  if (fclose(fp) != 0) rc = -1;
  if (rc == 0) rc = rename(temp, path);
  if (rc != 0) (void)unlink(temp);
  return rc;
}

/** Split one line into the five fields; 0 on success. */
static int split_fields(char *line, char **out, size_t out_count) {
  size_t field = 0U;
  char *p = line;

  while (field < out_count) {
    out[field++] = p;
    char *tab = strchr(p, '\t');
    if (tab == NULL) break;
    *tab = '\0';
    p = tab + 1;
  }

  return field == out_count && strchr(out[field - 1U], '\t') == NULL ? 0 : -1;
}

static int parse_u64(const char *value, uint64_t *out) {
  if (value == NULL || value[0] < '0' || value[0] > '9') return -1;
  errno = 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0') return -1;
  *out = (uint64_t)parsed;
  return 0;
}

size_t transfer_state_read(const char *path, transfer_state_entry_t *out,
                           size_t capacity) {
  if (path == NULL || out == NULL || capacity == 0U) return 0U;

  FILE *fp = fopen(path, "r");
  if (fp == NULL) return 0U;

  char line[8192];
  if (fgets(line, sizeof(line), fp) == NULL) {
    (void)fclose(fp);
    return 0U;
  }
  line[strcspn(line, "\r\n")] = '\0';
  int legacy = strcmp(line, k_legacy_magic) == 0;
  if (!legacy && strcmp(line, k_state_magic) != 0) {
    (void)fclose(fp);
    return 0U;
  }

  size_t count = 0U;
  while (count < capacity && fgets(line, sizeof(line), fp) != NULL) {
    if (strchr(line, '\n') == NULL && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != '\n' && c != EOF) {
      }
      continue;
    }

    char *fields[8];
    line[strcspn(line, "\r\n")] = '\0';
    if (split_fields(line, fields, legacy ? 5U : 8U) != 0) continue;

    char *end = NULL;
    long id = strtol(fields[0], &end, 10);
    if (end == fields[0] || *end != '\0' || id <= 0L || id > 1000000L) continue;

    transfer_state_entry_t *entry = &out[count];
    memset(entry, 0, sizeof(*entry));
    if (legacy) {
      entry->paused = fields[1][0] == '1';
      entry->status = entry->paused ? 'P' : 'A';
      read_escaped(entry->url, sizeof(entry->url), fields[2]);
      read_escaped(entry->dst, sizeof(entry->dst), fields[3]);
      read_escaped(entry->filename, sizeof(entry->filename), fields[4]);
    } else {
      if (fields[1][0] == '\0' || fields[1][1] != '\0' ||
          strchr("AQPDEC", fields[1][0]) == NULL ||
          parse_u64(fields[2], &entry->total_size) != 0 ||
          parse_u64(fields[3], &entry->downloaded) != 0) continue;
      entry->status = fields[1][0];
      entry->paused = entry->status == 'P';
      read_escaped(entry->url, sizeof(entry->url), fields[4]);
      read_escaped(entry->dst, sizeof(entry->dst), fields[5]);
      read_escaped(entry->filename, sizeof(entry->filename), fields[6]);
      read_escaped(entry->error_msg, sizeof(entry->error_msg), fields[7]);
    }
    /* An empty destination would resolve to the filesystem root: refuse it. */
    if (entry->url[0] == '\0' || entry->dst[0] == '\0' ||
        entry->filename[0] == '\0') {
      continue;
    }

    entry->id = (int)id;
    count++;
  }

  (void)fclose(fp);
  return count;
}
