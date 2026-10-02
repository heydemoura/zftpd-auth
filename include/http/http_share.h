/*
MIT License

Copyright (c) 2026 Seregon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/**
 * @file http_share.h
 * @brief Public share links: a file or folder reachable at /s/<id> without a
 *        login.  Shares are persisted and may carry an expiry.
 *
 *   GET /s/<id>                file: direct download; folder: HTML listing
 *   GET /s/<id>/<sub/path>     entry inside a shared folder
 *   GET /s/<id>/<sub>?zip=1    a folder (or the share root) as a ZIP stream
 */

#ifndef HTTP_SHARE_H
#define HTTP_SHARE_H

#include "ftp_config.h"
#include "http_parser.h"
#include "http_response.h"
#include <stddef.h>
#include <time.h>

#define HTTP_SHARE_MAX 256U
#define HTTP_SHARE_ID_HEX 32U
#define HTTP_SHARE_URL_PREFIX "/s/"

typedef struct {
  char id[HTTP_SHARE_ID_HEX + 1U];
  char path[FTP_PATH_MAX]; /**< canonical path inside the served root */
  int is_dir;
  time_t created;
  time_t expires; /**< 0 = never */
  char owner[33]; /**< login that created it; empty without the gate */
} http_share_t;

/** Load the persisted shares (uses the auth state directory). */
void http_share_init(void);
void http_share_shutdown(void);

int http_share_create(const char *path, time_t expires, const char *owner,
                      http_share_t *out, char *err, size_t err_size);
int http_share_delete(const char *id);
size_t http_share_count(void);
int http_share_get(size_t index, http_share_t *out);
int http_share_find(const char *id, http_share_t *out);
int http_share_is_expired(const http_share_t *share, time_t now);
size_t http_share_purge_expired(void);

/** 1 when the URI addresses a share link. */
int http_share_route_matches(const char *uri);
http_response_t *http_share_handle(const http_request_t *request);

#endif /* HTTP_SHARE_H */
