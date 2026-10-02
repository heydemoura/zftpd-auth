#include "http_sha256.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static void hex(const uint8_t *in, size_t len, char *out) {
  for (size_t i = 0; i < len; i++) (void)sprintf(out + i * 2U, "%02x", in[i]);
}

int main(void) {
  uint8_t digest[HTTP_SHA256_DIGEST_SIZE];
  char text[65];

  /* FIPS 180-4 vectors */
  http_sha256("abc", 3U, digest);
  hex(digest, sizeof(digest), text);
  CHECK(strcmp(text, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

  http_sha256("", 0U, digest);
  hex(digest, sizeof(digest), text);
  CHECK(strcmp(text, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

  static const char two_blocks[] =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  http_sha256(two_blocks, sizeof(two_blocks) - 1U, digest);
  hex(digest, sizeof(digest), text);
  CHECK(strcmp(text, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);

  /* Incremental updates must match the one-shot digest. */
  http_sha256_t ctx;
  http_sha256_init(&ctx);
  http_sha256_update(&ctx, two_blocks, 10U);
  http_sha256_update(&ctx, two_blocks + 10, sizeof(two_blocks) - 11U);
  http_sha256_final(&ctx, digest);
  hex(digest, sizeof(digest), text);
  CHECK(strcmp(text, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);

  /* One million 'a' (FIPS 180-4) */
  http_sha256_init(&ctx);
  char block[1000];
  memset(block, 'a', sizeof(block));
  for (int i = 0; i < 1000; i++) http_sha256_update(&ctx, block, sizeof(block));
  http_sha256_final(&ctx, digest);
  hex(digest, sizeof(digest), text);
  CHECK(strcmp(text, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);

  /* RFC 4231 test case 2 */
  http_hmac_sha256("Jefe", 4U, "what do ya want for nothing?", 28U, digest);
  hex(digest, sizeof(digest), text);
  CHECK(strcmp(text, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0);

  /* RFC 4231 test case 6: key longer than a block */
  uint8_t long_key[131];
  memset(long_key, 0xaa, sizeof(long_key));
  static const char tc6[] = "Test Using Larger Than Block-Size Key - Hash Key First";
  http_hmac_sha256(long_key, sizeof(long_key), tc6, sizeof(tc6) - 1U, digest);
  hex(digest, sizeof(digest), text);
  CHECK(strcmp(text, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54") == 0);

  /* RFC 7914 / PBKDF2-HMAC-SHA256 vectors (first 32 bytes) */
  http_pbkdf2_sha256("password", 8U, "salt", 4U, 1U, digest);
  hex(digest, sizeof(digest), text);
  CHECK(strcmp(text, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b") == 0);

  http_pbkdf2_sha256("password", 8U, "salt", 4U, 4096U, digest);
  hex(digest, sizeof(digest), text);
  CHECK(strcmp(text, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a") == 0);

  CHECK(http_crypto_equal("abc", "abc", 3U) == 1);
  CHECK(http_crypto_equal("abc", "abd", 3U) == 0);

  puts("test_http_sha256: ok");
  return 0;
}
