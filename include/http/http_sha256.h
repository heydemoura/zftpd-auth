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
 * @file http_sha256.h
 * @brief SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 (FIPS 180-4 / RFC 2104 /
 *        RFC 8018) used for web-interface password hashing.
 *
 * Self-contained so the console builds need no external crypto library.
 */

#ifndef HTTP_SHA256_H
#define HTTP_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define HTTP_SHA256_DIGEST_SIZE 32U
#define HTTP_SHA256_BLOCK_SIZE 64U

typedef struct {
  uint32_t state[8];
  uint64_t total;
  uint8_t buffer[HTTP_SHA256_BLOCK_SIZE];
  size_t buffered;
} http_sha256_t;

void http_sha256_init(http_sha256_t *ctx);
void http_sha256_update(http_sha256_t *ctx, const void *data, size_t len);
void http_sha256_final(http_sha256_t *ctx, uint8_t out[HTTP_SHA256_DIGEST_SIZE]);
void http_sha256(const void *data, size_t len,
                 uint8_t out[HTTP_SHA256_DIGEST_SIZE]);

void http_hmac_sha256(const void *key, size_t key_len, const void *data,
                      size_t data_len, uint8_t out[HTTP_SHA256_DIGEST_SIZE]);

/**
 * @brief PBKDF2-HMAC-SHA256 producing exactly one digest of key material.
 *
 * @param iterations  Must be >= 1.
 */
void http_pbkdf2_sha256(const void *password, size_t password_len,
                        const void *salt, size_t salt_len, uint32_t iterations,
                        uint8_t out[HTTP_SHA256_DIGEST_SIZE]);

/** Constant-time comparison; returns 1 when both buffers are identical. */
int http_crypto_equal(const void *a, const void *b, size_t len);

#endif /* HTTP_SHA256_H */
