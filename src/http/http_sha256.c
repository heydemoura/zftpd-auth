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

/** @file http_sha256.c @brief SHA-256 / HMAC / PBKDF2 for password storage. */

#include "http_sha256.h"
#include <string.h>

static const uint32_t k_round[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

static uint32_t rotr(uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32U - bits));
}

static uint32_t load_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void sha256_block(uint32_t state[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (size_t i = 0U; i < 16U; i++) w[i] = load_be32(block + i * 4U);
  for (size_t i = 16U; i < 64U; i++) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (size_t i = 0U; i < 64U; i++) {
    uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + s1 + ch + k_round[i] + w[i];
    uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

void http_sha256_init(http_sha256_t *ctx) {
  static const uint32_t k_init[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                     0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                     0x1f83d9abU, 0x5be0cd19U};
  memcpy(ctx->state, k_init, sizeof(k_init));
  ctx->total = 0U;
  ctx->buffered = 0U;
}

void http_sha256_update(http_sha256_t *ctx, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  ctx->total += (uint64_t)len;
  if (ctx->buffered > 0U) {
    size_t take = HTTP_SHA256_BLOCK_SIZE - ctx->buffered;
    if (take > len) take = len;
    memcpy(ctx->buffer + ctx->buffered, p, take);
    ctx->buffered += take;
    p += take;
    len -= take;
    if (ctx->buffered < HTTP_SHA256_BLOCK_SIZE) return;
    sha256_block(ctx->state, ctx->buffer);
    ctx->buffered = 0U;
  }
  while (len >= HTTP_SHA256_BLOCK_SIZE) {
    sha256_block(ctx->state, p);
    p += HTTP_SHA256_BLOCK_SIZE;
    len -= HTTP_SHA256_BLOCK_SIZE;
  }
  if (len > 0U) {
    memcpy(ctx->buffer, p, len);
    ctx->buffered = len;
  }
}

void http_sha256_final(http_sha256_t *ctx, uint8_t out[HTTP_SHA256_DIGEST_SIZE]) {
  uint64_t bits = ctx->total * 8U;
  uint8_t pad = 0x80U;
  http_sha256_update(ctx, &pad, 1U);
  ctx->total -= 1U;
  static const uint8_t zero[HTTP_SHA256_BLOCK_SIZE] = {0};
  while (ctx->buffered != 56U) {
    size_t gap = ctx->buffered < 56U ? 56U - ctx->buffered
                                     : HTTP_SHA256_BLOCK_SIZE - ctx->buffered;
    http_sha256_update(ctx, zero, gap);
    ctx->total -= (uint64_t)gap;
  }
  uint8_t length[8];
  for (size_t i = 0U; i < 8U; i++)
    length[i] = (uint8_t)(bits >> (8U * (7U - i)));
  http_sha256_update(ctx, length, sizeof(length));
  for (size_t i = 0U; i < 8U; i++) store_be32(out + i * 4U, ctx->state[i]);
  memset(ctx, 0, sizeof(*ctx));
}

void http_sha256(const void *data, size_t len,
                 uint8_t out[HTTP_SHA256_DIGEST_SIZE]) {
  http_sha256_t ctx;
  http_sha256_init(&ctx);
  http_sha256_update(&ctx, data, len);
  http_sha256_final(&ctx, out);
}

void http_hmac_sha256(const void *key, size_t key_len, const void *data,
                      size_t data_len, uint8_t out[HTTP_SHA256_DIGEST_SIZE]) {
  uint8_t block[HTTP_SHA256_BLOCK_SIZE];
  memset(block, 0, sizeof(block));
  if (key_len > HTTP_SHA256_BLOCK_SIZE) {
    http_sha256(key, key_len, block);
  } else {
    memcpy(block, key, key_len);
  }

  uint8_t ipad[HTTP_SHA256_BLOCK_SIZE];
  uint8_t opad[HTTP_SHA256_BLOCK_SIZE];
  for (size_t i = 0U; i < HTTP_SHA256_BLOCK_SIZE; i++) {
    ipad[i] = (uint8_t)(block[i] ^ 0x36U);
    opad[i] = (uint8_t)(block[i] ^ 0x5cU);
  }

  uint8_t inner[HTTP_SHA256_DIGEST_SIZE];
  http_sha256_t ctx;
  http_sha256_init(&ctx);
  http_sha256_update(&ctx, ipad, sizeof(ipad));
  http_sha256_update(&ctx, data, data_len);
  http_sha256_final(&ctx, inner);

  http_sha256_init(&ctx);
  http_sha256_update(&ctx, opad, sizeof(opad));
  http_sha256_update(&ctx, inner, sizeof(inner));
  http_sha256_final(&ctx, out);

  memset(block, 0, sizeof(block));
  memset(ipad, 0, sizeof(ipad));
  memset(opad, 0, sizeof(opad));
}

void http_pbkdf2_sha256(const void *password, size_t password_len,
                        const void *salt, size_t salt_len, uint32_t iterations,
                        uint8_t out[HTTP_SHA256_DIGEST_SIZE]) {
  /* Block index 1, big endian, appended to the salt for the first round. */
  uint8_t seed[256 + 4];
  if (salt_len > 256U) salt_len = 256U;
  memcpy(seed, salt, salt_len);
  seed[salt_len] = 0U;
  seed[salt_len + 1U] = 0U;
  seed[salt_len + 2U] = 0U;
  seed[salt_len + 3U] = 1U;

  uint8_t u[HTTP_SHA256_DIGEST_SIZE];
  http_hmac_sha256(password, password_len, seed, salt_len + 4U, u);
  memcpy(out, u, HTTP_SHA256_DIGEST_SIZE);
  if (iterations == 0U) iterations = 1U;
  for (uint32_t i = 1U; i < iterations; i++) {
    http_hmac_sha256(password, password_len, u, sizeof(u), u);
    for (size_t j = 0U; j < HTTP_SHA256_DIGEST_SIZE; j++) out[j] ^= u[j];
  }
  memset(u, 0, sizeof(u));
}

int http_crypto_equal(const void *a, const void *b, size_t len) {
  const uint8_t *x = (const uint8_t *)a;
  const uint8_t *y = (const uint8_t *)b;
  uint8_t diff = 0U;
  for (size_t i = 0U; i < len; i++) diff |= (uint8_t)(x[i] ^ y[i]);
  return diff == 0U;
}
