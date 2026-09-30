/*
 * The offsets module is PS5 data only and carries no PS5 headers, so it can be
 * compiled straight into this host test instead of the target build.
 */
#include "../src/platform/ps5/ps5_fw_offsets.c"

#include <stdio.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static int test_normalize(void) {
  CHECK(ps5_fw_normalize(0x13600000U) == 1360U);
  CHECK(ps5_fw_normalize(0x13420000U) == 1342U);
  CHECK(ps5_fw_normalize(0x13400000U) == 1340U);
  CHECK(ps5_fw_normalize(0x10000000U) == 1000U);
  CHECK(ps5_fw_normalize(0x09050000U) == 905U);
  CHECK(ps5_fw_normalize(0x04030000U) == 403U);
  CHECK(ps5_fw_normalize(0x02250000U) == 225U);
  CHECK(ps5_fw_normalize(0x01140000U) == 114U);
  CHECK(ps5_fw_normalize(0U) == 0U);
  return 0;
}

static int test_self_pager_rows(void) {
  const ps5_fw_offsets_t *row = NULL;

  row = ps5_fw_offsets_lookup(0x13600000U);
  CHECK(row != NULL && row->pager_table_off == 0xE03910U);
  row = ps5_fw_offsets_lookup(0x13420000U);
  CHECK(row != NULL && row->pager_table_off == 0xE03910U);
  row = ps5_fw_offsets_lookup(0x13400000U);
  CHECK(row != NULL && row->pager_table_off == 0xE03910U);

  row = ps5_fw_offsets_lookup(0x13200000U);
  CHECK(row != NULL && row->pager_table_off == 0xE038D0U);
  row = ps5_fw_offsets_lookup(0x13000000U);
  CHECK(row != NULL && row->pager_table_off == 0xE038D0U);

  row = ps5_fw_offsets_lookup(0x12700000U);
  CHECK(row != NULL && row->pager_table_off == 0xDF2860U);
  row = ps5_fw_offsets_lookup(0x01000000U);
  CHECK(row != NULL && row->pager_table_off == 0xC27C40U);
  return 0;
}

static int test_sysent_rows(void) {
  const ps5_fw_offsets_t *row = NULL;

  row = ps5_fw_offsets_lookup(0x13600000U);
  CHECK(row != NULL && row->sysent_off == 0x1B6E50U);
  row = ps5_fw_offsets_lookup(0x13420000U);
  CHECK(row != NULL && row->sysent_off == 0x1B6D30U);
  row = ps5_fw_offsets_lookup(0x13400000U);
  CHECK(row != NULL && row->sysent_off == 0x1B6D30U);
  row = ps5_fw_offsets_lookup(0x13200000U);
  CHECK(row != NULL && row->sysent_off == 0x1B6AC0U);
  row = ps5_fw_offsets_lookup(0x13000000U);
  CHECK(row != NULL && row->sysent_off == 0x1B6A60U);

  row = ps5_fw_offsets_lookup(0x12700000U);
  CHECK(row != NULL && row->sysent_off == 0x1AF4D0U);
  row = ps5_fw_offsets_lookup(0x04030000U);
  CHECK(row != NULL && row->sysent_off == 0x1709C0U);
  return 0;
}

/*
 * A firmware with a pager offset but no verified sysent offset must stay
 * refused by the net filter rather than inherit a neighbour's address.
 */
static int test_unverified_sysent_stays_zero(void) {
  const ps5_fw_offsets_t *row = NULL;

  row = ps5_fw_offsets_lookup(0x04000000U);
  CHECK(row != NULL && row->pager_table_off == 0xD20840U && row->sysent_off == 0U);
  row = ps5_fw_offsets_lookup(0x07010000U);
  CHECK(row != NULL && row->sysent_off == 0U);
  row = ps5_fw_offsets_lookup(0x07200000U);
  CHECK(row != NULL && row->sysent_off == 0U);
  row = ps5_fw_offsets_lookup(0x07600000U);
  CHECK(row != NULL && row->sysent_off == 0U);
  row = ps5_fw_offsets_lookup(0x03000000U);
  CHECK(row != NULL && row->sysent_off == 0U);
  return 0;
}

static int test_unknown_firmware(void) {
  CHECK(ps5_fw_offsets_lookup(0U) == NULL);
  CHECK(ps5_fw_offsets_lookup(0x00010000U) == NULL);
  CHECK(ps5_fw_offsets_lookup(0x13500000U) == NULL);
  CHECK(ps5_fw_offsets_lookup(0x14000000U) == NULL);
  return 0;
}

static int test_table_invariants(void) {
  for (size_t i = 0U; i < PS5_FW_OFFSETS_COUNT; i++) {
    CHECK(g_ps5_fw_offsets[i].fw != 0U);
    CHECK(g_ps5_fw_offsets[i].pager_table_off != 0U);
    if (i > 0U) {
      CHECK(g_ps5_fw_offsets[i].fw > g_ps5_fw_offsets[i - 1U].fw);
    }
  }
  return 0;
}

static int test_struct_field_offsets(void) {
  CHECK(PS5_OFF_THREAD_TD_PROC == 0x008U);
  CHECK(PS5_OFF_PROC_P_PID == 0x0BCU);
  return 0;
}

int main(void) {
  CHECK(test_normalize() == 0);
  CHECK(test_self_pager_rows() == 0);
  CHECK(test_sysent_rows() == 0);
  CHECK(test_unverified_sysent_stays_zero() == 0);
  CHECK(test_unknown_firmware() == 0);
  CHECK(test_table_invariants() == 0);
  CHECK(test_struct_field_offsets() == 0);
  puts("test_ps5_fw_offsets: ok");
  return 0;
}
