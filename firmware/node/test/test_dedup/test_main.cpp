/*
 * On-target dedup tests (T2.1).
 *
 * The host harness in firmware/common/test/dedup_selftest.c does the volume
 * work — a hundred thousand events against an identity oracle, sanitizers,
 * benchmarks. What it cannot do is run on the ABI the code ships to, and this
 * component has two things that are ABI-sensitive: an int8_t cast that decides
 * whether sequence 0 is newer or older than 255, and a struct whose size is a
 * RAM budget. Both are checked here, on the target, in the log an engineer
 * reads when the board is on the desk.
 *
 * Run with `pio test -e esp32dev -f test_dedup`.
 */
#include <unity.h>

#include "lorahome/dedup.h"

static lh_dedup_t g_dedup;
static int64_t g_clock_us;

void setUp(void) {
  lh_dedup_init(&g_dedup);
  g_clock_us = 0;
}
void tearDown(void) {}

static int64_t tick(void) { return (g_clock_us += 1000); }

static bool mark(uint16_t src, uint8_t seq) {
  return lh_dedup_check_and_mark(&g_dedup, src, seq, tick());
}

/** The Etap 2 RAM budget, asserted where somebody chasing bytes will see it. */
void test_struct_fits_budget(void) {
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(256, (uint32_t)sizeof(lh_dedup_t));
}

void test_first_frame_is_new(void) {
  TEST_ASSERT_TRUE(mark(0x0101, 0));
  TEST_ASSERT_EQUAL_UINT32(1, g_dedup.stat_accepted);
  TEST_ASSERT_EQUAL_UINT8(1, g_dedup.peer_count);
}

void test_immediate_replay_is_dropped(void) {
  TEST_ASSERT_TRUE(mark(0x0101, 42));
  TEST_ASSERT_FALSE(mark(0x0101, 42));
  TEST_ASSERT_EQUAL_UINT32(1, g_dedup.stat_dupes_dropped);
}

/**
 * Sequence wrap — risk R2.1.
 *
 * On a target where the compiler treats plain `char` as unsigned and the
 * modular cast were written differently, this is the test that fails. 0xFF
 * followed by 0x00 is one frame later, not 255 frames earlier.
 */
void test_wrap_is_forward_progress(void) {
  TEST_ASSERT_TRUE(mark(0x0202, 0xFE));
  TEST_ASSERT_TRUE(mark(0x0202, 0xFF));
  TEST_ASSERT_TRUE(mark(0x0202, 0x00));
  TEST_ASSERT_TRUE(mark(0x0202, 0x01));
  TEST_ASSERT_FALSE(mark(0x0202, 0xFF)); /* still inside the window */
  TEST_ASSERT_EQUAL_UINT32(4, g_dedup.stat_accepted);
  TEST_ASSERT_EQUAL_UINT32(1, g_dedup.stat_dupes_dropped);
}

void test_out_of_order_inside_window(void) {
  TEST_ASSERT_TRUE(mark(0x0303, 10));
  TEST_ASSERT_TRUE(mark(0x0303, 8)); /* late, but remembered */
  TEST_ASSERT_FALSE(mark(0x0303, 8));
  TEST_ASSERT_TRUE(mark(0x0303, 9)); /* the gap was never filled */
}

void test_older_than_window_is_counted_apart(void) {
  TEST_ASSERT_TRUE(mark(0x0404, 100));
  TEST_ASSERT_TRUE(mark(0x0404, 100 - (LH_DEDUP_WINDOW - 1)));
  TEST_ASSERT_FALSE(mark(0x0404, 100 - LH_DEDUP_WINDOW));
  TEST_ASSERT_EQUAL_UINT32(1, g_dedup.stat_too_old);
  TEST_ASSERT_EQUAL_UINT32(0, g_dedup.stat_dupes_dropped);
}

void test_peers_do_not_share_a_window(void) {
  TEST_ASSERT_TRUE(mark(0x0501, 5));
  TEST_ASSERT_TRUE(mark(0x0502, 5));
  TEST_ASSERT_EQUAL_UINT32(0, g_dedup.stat_dupes_dropped);
}

/** The ninth sender evicts the one heard from longest ago, and says so. */
void test_lru_eviction(void) {
  for (uint16_t peer = 0; peer < LH_DEDUP_PEERS; peer++) mark((uint16_t)(0x0600 + peer), 1);
  for (uint16_t peer = 1; peer < LH_DEDUP_PEERS; peer++) mark((uint16_t)(0x0600 + peer), 2);

  TEST_ASSERT_TRUE(mark(0x9999, 1));
  TEST_ASSERT_EQUAL_UINT32(1, g_dedup.stat_peer_evicted);
  TEST_ASSERT_EQUAL_UINT8(LH_DEDUP_PEERS, g_dedup.peer_count);
  TEST_ASSERT_NULL(lh_dedup_find_peer(&g_dedup, 0x0600));
  TEST_ASSERT_NOT_NULL(lh_dedup_find_peer(&g_dedup, 0x0601));
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_struct_fits_budget);
  RUN_TEST(test_first_frame_is_new);
  RUN_TEST(test_immediate_replay_is_dropped);
  RUN_TEST(test_wrap_is_forward_progress);
  RUN_TEST(test_out_of_order_inside_window);
  RUN_TEST(test_older_than_window_is_counted_apart);
  RUN_TEST(test_peers_do_not_share_a_window);
  RUN_TEST(test_lru_eviction);
  return UNITY_END();
}
