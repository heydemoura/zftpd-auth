/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file ps4_pacbrew_compat.c
 * @brief libc glue for the PacBrew ps4-openorbis libcurl
 *
 * The PacBrew port of libcurl is built against the OpenOrbis musl libc while
 * the payload itself links against the ps4-payload-sdk.  Three symbols the
 * OpenOrbis build expects do not exist in the payload runtime:
 *
 *   __errno_location()  musl reaches errno through this accessor; the payload
 *                       libc exports __error() instead (see <errno.h>).
 *   fnmatch()           curl uses it for file:// wildcards.
 *   execl()             curl uses it to delegate NTLM authentication.
 *
 * zftpd's downloader reaches neither of the two functions, so they are
 * implemented with plain behavior rather than left to break the link.
 *
 * The file is compiled only for PS4 builds with ENABLE_LIBCURL=1.
 */

#include <errno.h>
#include <fnmatch.h>
#include <stddef.h>
#include <unistd.h>

int *__errno_location(void);
int *__errno_location(void) {
  return __error();
}

int fnmatch(const char *pattern, const char *string, int flags);
int fnmatch(const char *pattern, const char *string, int flags) {
  (void)flags;
  while (*pattern != '\0') {
    if (*pattern == '*') {
      while (*pattern == '*') pattern++;
      if (*pattern == '\0') return 0;
      while (*string != '\0') {
        if (fnmatch(pattern, string, 0) == 0) return 0;
        string++;
      }
      return FNM_NOMATCH;
    }
    if (*string == '\0') return FNM_NOMATCH;
    if (*pattern == '?') {
      pattern++;
      string++;
      continue;
    }
    if (*pattern == '[') {
      const char *set = pattern + 1;
      int negate = (*set == '!' || *set == '^');
      int matched = 0;
      int first = 1;
      char previous = '\0';
      if (negate) set++;
      while (*set != '\0' && (*set != ']' || first)) {
        first = 0;
        if (*set == '-' && previous != '\0' && set[1] != '\0' && set[1] != ']') {
          if ((unsigned char)previous <= (unsigned char)*string &&
              (unsigned char)*string <= (unsigned char)set[1])
            matched = 1;
          set += 2;
          previous = '\0';
          continue;
        }
        if (*set == *string) matched = 1;
        previous = *set;
        set++;
      }
      if (*set != ']' || matched == negate) return FNM_NOMATCH;
      pattern = set + 1;
      string++;
      continue;
    }
    if (*pattern == '\\' && pattern[1] != '\0') pattern++;
    if (*pattern != *string) return FNM_NOMATCH;
    pattern++;
    string++;
  }
  return (*string == '\0') ? 0 : FNM_NOMATCH;
}

int execl(const char *path, const char *arg, ...);
int execl(const char *path, const char *arg, ...) {
  (void)path;
  (void)arg;
  errno = ENOSYS;
  return -1;
}
