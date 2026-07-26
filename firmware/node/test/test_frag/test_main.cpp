/*
 * On-target fragmentation tests (T2.3).
 *
 * The host harness in firmware/common/test/frag_selftest.c does the volume work
 * — a thousand shuffled deliveries, forged fragments, sanitizers. These are the
 * parts that only mean something on the target: the reassembler is the largest
 * single allocation in the Node's static RAM budget, and its size is decided by
 * the target's alignment rules, not by the host's.
 *
 * Run with `pio test -e esp32dev -f test_frag`.
 */
#include <unity.h>

#include <string.h>

#include "lorahome/frag.h"

static lh_reassembler_t g_reasm;
static uint8_t g_config[LH_FRAG_CONFIG_MAX];
static uint8_t g_fragments[LH_FRAG_MAX_FRAGMENTS][LH_FRAG_HDR_SIZE + LH_FRAG_PAYLOAD_MAX];
static uint16_t g_fragment_len[LH_FRAG_MAX_FRAGMENTS];

void setUp(void) { lh_frag_reset(&g_reasm); }
void tearDown(void) {}

static uint8_t make_config(uint16_t cfg_id, uint16_t total_len, uint8_t seed) {
  for (uint16_t i = 0; i < total_len; i++) g_config[i] = (uint8_t)(i * 31u + seed);

  const uint8_t total = lh_frag_count(total_len);
  for (uint8_t index = 0; index < total; index++) {
    const int written = lh_frag_build(cfg_id, index, g_config, total_len, g_fragments[index],
                                      (uint16_t)sizeof(g_fragments[index]));
    g_fragment_len[index] = written < 0 ? 0 : (uint16_t)written;
  }
  return total;
}

/** 1664 B of the 2304 B reliability budget, asserted where a reader will see it. */
void test_reassembler_fits_budget(void) {
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(1664, (uint32_t)sizeof(lh_reassembler_t));
}

void test_full_size_round_trip(void) {
  const uint8_t total = make_config(0x1234, LH_FRAG_CONFIG_MAX, 7);
  TEST_ASSERT_EQUAL_UINT8(LH_FRAG_MAX_FRAGMENTS, total);

  lh_frag_result_t result = LH_FRAG_NEED_MORE;
  for (uint8_t index = 0; index < total; index++) {
    result = lh_frag_feed(&g_reasm, g_fragments[index], g_fragment_len[index], 1000);
  }

  TEST_ASSERT_EQUAL_INT(LH_FRAG_COMPLETE, result);
  TEST_ASSERT_EQUAL_UINT16(LH_FRAG_CONFIG_MAX, g_reasm.total_len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(g_config, g_reasm.buf, LH_FRAG_CONFIG_MAX);
}

/** Reverse order is the cheap proxy for "any order" on a device. */
void test_reverse_order(void) {
  const uint8_t total = make_config(0x2222, 700, 3);
  TEST_ASSERT_EQUAL_UINT8(4, total);

  lh_frag_result_t result = LH_FRAG_NEED_MORE;
  for (uint8_t i = total; i > 0; i--) {
    result = lh_frag_feed(&g_reasm, g_fragments[i - 1], g_fragment_len[i - 1], 1000);
  }

  TEST_ASSERT_EQUAL_INT(LH_FRAG_COMPLETE, result);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(g_config, g_reasm.buf, 700);
}

void test_duplicate_is_idempotent(void) {
  make_config(0x3333, 600, 5);
  lh_frag_feed(&g_reasm, g_fragments[0], g_fragment_len[0], 1000);
  const uint8_t mask = g_reasm.received_mask;

  TEST_ASSERT_EQUAL_INT(LH_FRAG_DUPLICATE,
                        lh_frag_feed(&g_reasm, g_fragments[0], g_fragment_len[0], 1100));
  TEST_ASSERT_EQUAL_UINT8(mask, g_reasm.received_mask);
  TEST_ASSERT_EQUAL_UINT32(1, g_reasm.stat_duplicates);
}

/** R2.3: a stray fragment must not destroy a transaction one frame from done. */
void test_foreign_fragment_is_refused(void) {
  make_config(0x4444, 400, 9);
  uint8_t keep[LH_FRAG_HDR_SIZE + LH_FRAG_PAYLOAD_MAX];
  memcpy(keep, g_fragments[1], g_fragment_len[1]);
  const uint16_t keep_len = g_fragment_len[1];
  uint8_t original[400];
  memcpy(original, g_config, sizeof(original));

  lh_frag_feed(&g_reasm, g_fragments[0], g_fragment_len[0], 1000);

  make_config(0x5555, 400, 200);
  TEST_ASSERT_EQUAL_INT(LH_FRAG_ERR_FOREIGN,
                        lh_frag_feed(&g_reasm, g_fragments[1], g_fragment_len[1], 1100));
  TEST_ASSERT_EQUAL_UINT16(0x4444, g_reasm.cfg_id);

  TEST_ASSERT_EQUAL_INT(LH_FRAG_COMPLETE, lh_frag_feed(&g_reasm, keep, keep_len, 1200));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(original, g_reasm.buf, 400);
}

/** R2.4: a lost fragment costs thirty seconds, not the device's configurability. */
void test_timeout_releases_the_slot(void) {
  const int64_t start = 5000000;
  make_config(0x9999, 600, 6);
  lh_frag_feed(&g_reasm, g_fragments[0], g_fragment_len[0], start);

  TEST_ASSERT_FALSE(lh_frag_tick(&g_reasm, start + (int64_t)LH_FRAG_TIMEOUT_MS * 1000 - 1));
  TEST_ASSERT_TRUE(lh_frag_tick(&g_reasm, start + (int64_t)LH_FRAG_TIMEOUT_MS * 1000));
  TEST_ASSERT_FALSE(g_reasm.active);
  TEST_ASSERT_EQUAL_UINT32(1, g_reasm.stat_timeouts);
}

/** Big-endian on the wire, like every other multi-byte field in this protocol. */
void test_header_is_big_endian(void) {
  const lh_frag_hdr_t hdr = {0x1234, 2, 5, 0x0ABC, 0xBEEF};
  uint8_t wire[LH_FRAG_HDR_SIZE];
  lh_frag_hdr_encode(&hdr, wire);

  const uint8_t expected[LH_FRAG_HDR_SIZE] = {0x12, 0x34, 0x02, 0x05, 0x0A, 0xBC, 0xBE, 0xEF};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, wire, LH_FRAG_HDR_SIZE);

  lh_frag_hdr_t decoded;
  lh_frag_hdr_decode(wire, &decoded);
  TEST_ASSERT_EQUAL_UINT16(0x1234, decoded.cfg_id);
  TEST_ASSERT_EQUAL_UINT16(0x0ABC, decoded.total_len);
  TEST_ASSERT_EQUAL_UINT16(0xBEEF, decoded.crc_total);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_reassembler_fits_budget);
  RUN_TEST(test_full_size_round_trip);
  RUN_TEST(test_reverse_order);
  RUN_TEST(test_duplicate_is_idempotent);
  RUN_TEST(test_foreign_fragment_is_refused);
  RUN_TEST(test_timeout_releases_the_slot);
  RUN_TEST(test_header_is_big_endian);
  return UNITY_END();
}
