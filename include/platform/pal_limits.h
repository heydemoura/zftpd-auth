#ifndef ZFTPD_PAL_LIMITS_H
#define ZFTPD_PAL_LIMITS_H

#include <limits.h>

#ifndef PAL_PATH_MAX
#if defined(__APPLE__) && defined(__MACH__)
#define PAL_PATH_MAX 1024U
#elif defined(PS4) || defined(PS5) || defined(PLATFORM_PS4) || \
    defined(PLATFORM_PS5)
#define PAL_PATH_MAX 1024U
#elif defined(PS3)
#define PAL_PATH_MAX 512U
#elif defined(PATH_MAX)
#define PAL_PATH_MAX PATH_MAX
#else
#define PAL_PATH_MAX 4096U
#endif
#endif

#endif
