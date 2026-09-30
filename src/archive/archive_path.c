#include "archive_path.h"
#include "pal_limits.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static int is_separator(char c) { return c == '/' || c == '\\'; }

int archive_path_is_safe_relative(const char *entry, size_t entry_len) {
  if (entry == NULL || entry_len == 0U || is_separator(entry[0]) ||
      memchr(entry, '\0', entry_len) != NULL)
    return 0;
  if (entry_len >= 2U && entry[1] == ':') return 0;

  while (entry_len > 0U && is_separator(entry[entry_len - 1U])) entry_len--;
  if (entry_len == 0U) return 0;

  size_t component_start = 0U;
  for (size_t i = 0U; i <= entry_len; i++) {
    if (i < entry_len && !is_separator(entry[i])) {
      if ((unsigned char)entry[i] < 0x20U) return 0;
      continue;
    }
    size_t component_len = i - component_start;
    if (component_len == 0U) return 0;
    if ((component_len == 1U && entry[component_start] == '.') ||
        (component_len == 2U && entry[component_start] == '.' &&
         entry[component_start + 1U] == '.'))
      return 0;
    component_start = i + 1U;
  }
  return 1;
}

static int ensure_directory(const char *path) {
  struct stat st;
  if (lstat(path, &st) == 0)
    return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) ? 0 : -1;
  if (errno != ENOENT || mkdir(path, 0777) != 0) return -1;
  return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)
             ? 0
             : -1;
}

static int append_component(char *path, size_t path_size,
                            const char *component, size_t component_len) {
  size_t used = strlen(path);
  int need_slash = used == 0U || path[used - 1U] != '/';
  size_t needed = used + (size_t)need_slash + component_len + 1U;
  if (needed > path_size) return -1;
  if (need_slash) path[used++] = '/';
  memcpy(path + used, component, component_len);
  path[used + component_len] = '\0';
  return 0;
}

static int prepare_root(const char *root, char *out, size_t out_size) {
  if (root == NULL || root[0] != '/') return -1;
  size_t len = strlen(root);
  if (len == 0U || len >= out_size) return -1;
  memcpy(out, root, len + 1U);
  while (len > 1U && out[len - 1U] == '/') out[--len] = '\0';

  char walk[PAL_PATH_MAX] = "/";
  const char *p = out + 1;
  while (*p != '\0') {
    const char *slash = strchr(p, '/');
    size_t component_len = slash != NULL ? (size_t)(slash - p) : strlen(p);
    if (component_len == 0U ||
        append_component(walk, sizeof(walk), p, component_len) != 0 ||
        ensure_directory(walk) != 0)
      return -1;
    if (slash == NULL) break;
    p = slash + 1;
  }
  return 0;
}

int archive_path_prepare(const char *root, const char *entry, size_t entry_len,
                         int is_directory, char *out, size_t out_size) {
  if (out == NULL || out_size == 0U ||
      !archive_path_is_safe_relative(entry, entry_len))
    return -1;

  while (entry_len > 0U && is_separator(entry[entry_len - 1U])) entry_len--;
  if (prepare_root(root, out, out_size) != 0) return -1;

  size_t component_start = 0U;
  for (size_t i = 0U; i <= entry_len; i++) {
    if (i < entry_len && !is_separator(entry[i])) continue;
    size_t component_len = i - component_start;
    if (append_component(out, out_size, entry + component_start,
                         component_len) != 0)
      return -1;

    int is_last = i == entry_len;
    if (!is_last || is_directory) {
      if (ensure_directory(out) != 0) return -1;
    } else {
      struct stat st;
      if (lstat(out, &st) == 0) {
        if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode) || st.st_nlink != 1)
          return -1;
      } else if (errno != ENOENT) {
        return -1;
      }
    }
    component_start = i + 1U;
  }
  return 0;
}
