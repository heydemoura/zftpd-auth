#include "http_api.h"
#include "http_response.h"
#include "pal_limits.h"
#include "../src/http/games/games_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { \
  if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return -1; \
  } \
} while (0)

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8U);
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8U);
  p[2] = (uint8_t)(v >> 16U);
  p[3] = (uint8_t)(v >> 24U);
}
static int test_sfo(void) {
  uint8_t sfo[64] = {0};
  put32(sfo + 0x00, 0x46535000U);
  put32(sfo + 0x08, 0x24U);
  put32(sfo + 0x0C, 0x2AU);
  put32(sfo + 0x10, 1U);
  put16(sfo + 0x14, 0U);
  put16(sfo + 0x16, 0x0204U);
  put32(sfo + 0x18, 6U);
  put32(sfo + 0x1C, 6U);
  put32(sfo + 0x20, 0U);
  memcpy(sfo + 0x24, "TITLE", 6U);
  memcpy(sfo + 0x2A, "Hello", 6U);

  char out[16];
  CHECK(games_sfo_get_string(sfo, 0x30U, "TITLE", out, sizeof(out)) == 0);
  CHECK(strcmp(out, "Hello") == 0);
  CHECK(games_sfo_get_string(sfo, 0x30U, "MISSING", out, sizeof(out)) != 0);

  put32(sfo + 0x10, UINT32_MAX);
  CHECK(games_sfo_get_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) != 0);
  return 0;
}
static int test_json_and_ids(void) {
  char out[64];
  CHECK(games_json_get_string("{\"title\":\"A\\nB\"}", "title",
                              out, sizeof(out)) == 0);
  CHECK(strcmp(out, "A\nB") == 0);
  CHECK(games_json_get_string("{\"title\":\"abcdef\"}", "title",
                              out, 4U) != 0);
  CHECK(games_json_get_string("{\"title\":\"broken}", "title",
                              out, sizeof(out)) != 0);

  CHECK(games_title_id_from_content_id(
            "UP0000-CUSA12345_00-0000000000000000", out, sizeof(out)) == 0);
  CHECK(strcmp(out, "CUSA12345") == 0);
  CHECK(games_title_id_from_content_id("no-title-id", out, sizeof(out)) != 0);

  CHECK(games_is_valid_title_id("CUSA12345") == 1);
  CHECK(games_is_valid_title_id("ABCD-1234") == 1);
  CHECK(games_is_valid_title_id("../CUSA12345") == 0);
  CHECK(games_is_valid_title_id("A/B") == 0);
  return 0;
}
static int test_install_state(void) {
  games_install_snapshot_t state;
  games_install_state_begin(42, "CUSA12345", "/data/test.pkg");
  games_install_state_snapshot(&state);
  CHECK(state.active == 1);
  CHECK(state.task_id == 42);
  CHECK(strcmp(state.title_id, "CUSA12345") == 0);
  CHECK(strcmp(state.path, "/data/test.pkg") == 0);

  CHECK(games_install_state_refresh(&state) == 0);
#if !defined(PLATFORM_PS4) && !defined(PLATFORM_PS5)
  CHECK(state.active == 1);
  CHECK(state.last_percent == 0);
#endif
  return 0;
}

static int response_has_status(const http_response_t *resp, int status) {
  if (resp == NULL) return 0;
  char prefix[32];
  int n = snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d", status);
  return n > 0 && (size_t)n < sizeof(prefix) &&
         strncmp(resp->data, prefix, (size_t)n) == 0;
}

static void init_request(http_request_t *req, const char *uri) {
  memset(req, 0, sizeof(*req));
  req->method = HTTP_METHOD_GET;
  (void)snprintf(req->uri, sizeof(req->uri), "%s", uri);
}

static int test_game_routes(void) {
  http_request_t req;
  http_response_t *resp;

  init_request(&req, "/api/game/metax");
  CHECK(http_games_metadata_handle(&req) == NULL);

  init_request(&req, "/api/game/meta?path=/definitely-missing-zftpd-image");
  resp = http_games_metadata_handle(&req);
  CHECK(resp != NULL);
  http_response_destroy(resp);

  init_request(&req, "/api/admin/games/install_status_extra");
  CHECK(http_games_admin_handle(&req) == NULL);
  init_request(&req, "/api/admin/games/install_status");
  resp = http_games_admin_handle(&req);
  CHECK(response_has_status(resp, 200));
  http_response_destroy(resp);

  init_request(&req, "/api/admin/launch_extra");
  CHECK(http_games_admin_handle(&req) == NULL);

  init_request(&req, "/api/admin/launch");
  resp = http_games_admin_handle(&req);
  CHECK(response_has_status(resp, 400));
  http_response_destroy(resp);

  init_request(&req, "/api/admin/launch?xid=CUSA12345");
  resp = http_games_admin_handle(&req);
  CHECK(response_has_status(resp, 400));
  http_response_destroy(resp);

  init_request(&req, "/api/admin/launch?id=%ZZ");
  resp = http_games_admin_handle(&req);
  CHECK(response_has_status(resp, 400));
  http_response_destroy(resp);

  init_request(&req, "/api/stream/status");
  resp = http_api_handle(&req);
  CHECK(response_has_status(resp, 200));
  http_response_destroy(resp);

  init_request(&req, "/api/stream/statusx");
  resp = http_api_handle(&req);
  CHECK(response_has_status(resp, 404));
  http_response_destroy(resp);
  return 0;
}
/*
 * Icon endpoint: 200 + PNG bytes when the app directory holds
 * sce_sys/icon0.png, and 404 on everything else so the web UI can fall back
 * to its placeholder instead of rendering a blank image.
 */
static int test_icon_route(void) {
  char root[] = "/tmp/zftpd-games-icon-XXXXXX";
  CHECK(mkdtemp(root) != NULL);

  char app[PAL_PATH_MAX];
  char sce[PAL_PATH_MAX];
  char icon[PAL_PATH_MAX];
  CHECK(snprintf(app, sizeof(app), "%s/CUSA00001", root) > 0);
  CHECK(mkdir(app, 0700) == 0);
  CHECK(snprintf(sce, sizeof(sce), "%s/sce_sys", app) > 0);
  CHECK(mkdir(sce, 0700) == 0);
  CHECK(snprintf(icon, sizeof(icon), "%s/icon0.png", sce) > 0);

  static const uint8_t png[] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  FILE *fp = fopen(icon, "wb");
  CHECK(fp != NULL);
  CHECK(fwrite(png, 1U, sizeof(png), fp) == sizeof(png));
  CHECK(fclose(fp) == 0);

  http_request_t req;
  http_response_t *resp;

  init_request(&req, "/api/admin/games/icon");
  resp = http_games_admin_handle(&req);
  CHECK(response_has_status(resp, 404));
  http_response_destroy(resp);

  char uri[PAL_PATH_MAX];
  CHECK(snprintf(uri, sizeof(uri), "/api/admin/games/icon?path=%s", app) > 0);
  init_request(&req, uri);
  resp = http_games_admin_handle(&req);
  CHECK(response_has_status(resp, 200));
  CHECK(strstr(resp->data, "Content-Type: image/png") != NULL);
  CHECK(resp->mem_body != NULL);
  CHECK(resp->mem_length == sizeof(png));
  CHECK(memcmp(resp->mem_body, png, sizeof(png)) == 0);
  http_response_destroy(resp);

  init_request(&req, "/api/admin/games/icon?path=/tmp/zftpd-no-such-app");
  resp = http_games_admin_handle(&req);
  CHECK(response_has_status(resp, 404));
  http_response_destroy(resp);

  /* A hint outside the canonical root is dropped, then the appmeta lookup
   * misses on this host: still a 404, never an arbitrary file read. */
  init_request(&req, "/api/admin/games/icon?path=/tmp/../../etc/passwd");
  resp = http_games_admin_handle(&req);
  CHECK(response_has_status(resp, 404));
  http_response_destroy(resp);

  CHECK(unlink(icon) == 0);
  CHECK(rmdir(sce) == 0);
  CHECK(rmdir(app) == 0);
  CHECK(rmdir(root) == 0);
  return 0;
}

/* ── Single-package installs: the title name lives inside app.pkg ────────── */

static void put32be(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24U);
  p[1] = (uint8_t)(v >> 16U);
  p[2] = (uint8_t)(v >> 8U);
  p[3] = (uint8_t)v;
}

/** Build a minimal SFO with @p count string parameters. */
static size_t build_sfo(uint8_t *out, size_t cap, const char *const *keys,
                        const char *const *values, size_t count) {
  const size_t index_size = 0x14U + count * 16U;
  size_t keys_len = 0U;
  for (size_t i = 0U; i < count; i++) keys_len += strlen(keys[i]) + 1U;
  const size_t key_table = index_size;
  const size_t data_table = key_table + keys_len;
  size_t total = data_table;
  for (size_t i = 0U; i < count; i++) total += strlen(values[i]) + 1U;
  if (total > cap) return 0U;

  memset(out, 0, total);
  put32(out + 0x00U, 0x46535000U);
  put32(out + 0x04U, 0x00000101U);
  put32(out + 0x08U, (uint32_t)key_table);
  put32(out + 0x0CU, (uint32_t)data_table);
  put32(out + 0x10U, (uint32_t)count);

  size_t key_cursor = 0U, data_cursor = 0U;
  for (size_t i = 0U; i < count; i++) {
    uint8_t *entry = out + 0x14U + i * 16U;
    size_t key_len = strlen(keys[i]) + 1U;
    size_t value_len = strlen(values[i]) + 1U;
    put16(entry + 0x00U, (uint16_t)key_cursor);
    put16(entry + 0x02U, 0x0204U);
    put32(entry + 0x04U, (uint32_t)value_len);
    put32(entry + 0x08U, (uint32_t)value_len);
    put32(entry + 0x0CU, (uint32_t)data_cursor);
    memcpy(out + key_table + key_cursor, keys[i], key_len);
    memcpy(out + data_table + data_cursor, values[i], value_len);
    key_cursor += key_len;
    data_cursor += value_len;
  }
  return total;
}

/** Minimal PKG image with one plaintext param.sfo entry. */
static int write_min_app_pkg(const char *path, const uint8_t *sfo,
                             size_t sfo_len) {
  uint8_t header[0x1000];
  uint8_t record[32];
  memset(header, 0, sizeof(header));
  memset(record, 0, sizeof(record));

  put32be(header + 0x00U, 0x7F434E54U); /* PKG_MAGIC_CNT */
  put32be(header + 0x10U, 1U);          /* entry_count */
  put32be(header + 0x18U, 0x1000U);     /* table_offset */
  memcpy(header + 0x40U, "IV0000-APOL00004_00-APOLLO0000000PS4", 36U);

  put32be(record + 0x00U, 0x1000U);              /* id: param.sfo  */
  put32be(record + 0x08U, 0U);                   /* not encrypted  */
  put32be(record + 0x10U, 0x1020U);              /* data offset    */
  put32be(record + 0x14U, (uint32_t)sfo_len);    /* data size      */

  FILE *fp = fopen(path, "wb");
  if (fp == NULL) return -1;
  int ok = fwrite(header, 1U, sizeof(header), fp) == sizeof(header) &&
           fwrite(record, 1U, sizeof(record), fp) == sizeof(record) &&
           fwrite(sfo, 1U, sfo_len, fp) == sfo_len;
  if (fclose(fp) != 0) ok = 0;
  return ok ? 0 : -1;
}

/*
 * Titles delivered as a single package keep no sce_sys/param.sfo on disk: the
 * Games view showed the title id twice because the name lookup only knew the
 * directory layout.
 */
static int test_app_pkg_install_name(void) {
  char root[] = "/tmp/zftpd-games-pkg-XXXXXX";
  CHECK(mkdtemp(root) != NULL);

  char app[PAL_PATH_MAX];
  char pkg[PAL_PATH_MAX];
  CHECK(snprintf(app, sizeof(app), "%s/APOL00004", root) > 0);
  CHECK(mkdir(app, 0700) == 0);
  CHECK(snprintf(pkg, sizeof(pkg), "%s/app.pkg", app) > 0);

  static const char *const keys[] = {"TITLE_ID", "TITLE"};
  static const char *const values[] = {"APOL00004", "Apollo Save Tool"};
  uint8_t sfo[512];
  size_t sfo_len = build_sfo(sfo, sizeof(sfo), keys, values, 2U);
  CHECK(sfo_len > 0U);
  CHECK(write_min_app_pkg(pkg, sfo, sfo_len) == 0);

  char body[1024];
  size_t pos = 0U;
  int first = 1;
  size_t added = 0U;
  CHECK(games_append_installed_entries(root, body, sizeof(body), &pos, &first,
                                       &added) == 0);
  CHECK(added == 1U);
  body[pos] = '\0';
  CHECK(strstr(body, "\"name\":\"Apollo Save Tool\"") != NULL);
  CHECK(strstr(body, "\"id\":\"APOL00004\"") != NULL);

  CHECK(unlink(pkg) == 0);
  CHECK(rmdir(app) == 0);
  CHECK(rmdir(root) == 0);
  return 0;
}

int main(void) {
  CHECK(test_sfo() == 0);
  CHECK(test_json_and_ids() == 0);
  CHECK(test_install_state() == 0);
  CHECK(test_game_routes() == 0);
  CHECK(test_icon_route() == 0);
  CHECK(test_app_pkg_install_name() == 0);
  puts("test_http_games: ok");
  return 0;
}
