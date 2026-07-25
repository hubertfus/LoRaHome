/*
 * On-target tests for the UART ring (T1.2).
 *
 * The host harness (firmware/common/test/ring_selftest.c) covers the algebra at
 * volume — 10 MB through the ring, four passes over the uint16_t rollover. What
 * it cannot cover is this ABI: `volatile uint16_t` accesses, the width of the
 * struct, and whether the fences compile to anything on Xtensa. That is what
 * these are for.
 *
 * Run with `pio test -e esp32dev -f test_ring`.
 */
#include <unity.h>

#include "lorahome/ring.h"

static lh_ring_t g_ring;

void setUp(void) { lh_ring_init(&g_ring); }
void tearDown(void) {}

/** 2 kB of payload plus a handful of bookkeeping, and no hidden padding. */
void test_struct_size_is_budgeted(void) {
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(LH_UART_RING_SIZE + 16u, (uint32_t)sizeof(lh_ring_t));
}

void test_fresh_ring_is_empty(void) {
  uint8_t byte = 0;
  TEST_ASSERT_EQUAL_UINT16(0, lh_ring_count(&g_ring));
  TEST_ASSERT_EQUAL_UINT16(LH_UART_RING_SIZE, lh_ring_free_space(&g_ring));
  TEST_ASSERT_FALSE(lh_ring_pop(&g_ring, &byte));
}

/**
 * Full and empty must be distinguishable, and every byte of capacity usable.
 * With wrapped indices this state is ambiguous; with free-running ones it is
 * not, and that is the one design decision this type turns on.
 */
void test_full_ring_is_not_mistaken_for_empty(void) {
  for (uint16_t i = 0; i < LH_UART_RING_SIZE; i++) {
    TEST_ASSERT_TRUE(lh_ring_push(&g_ring, (uint8_t)i));
  }
  TEST_ASSERT_EQUAL_UINT16(LH_UART_RING_SIZE, lh_ring_count(&g_ring));
  TEST_ASSERT_EQUAL_UINT16(0, lh_ring_free_space(&g_ring));

  TEST_ASSERT_FALSE(lh_ring_push(&g_ring, 0xEE));
  TEST_ASSERT_EQUAL_UINT32(1, g_ring.stat_overrun);

  for (uint16_t i = 0; i < LH_UART_RING_SIZE; i++) {
    uint8_t byte = 0;
    TEST_ASSERT_TRUE(lh_ring_pop(&g_ring, &byte));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)i, byte);
  }
  TEST_ASSERT_EQUAL_UINT16(0, lh_ring_count(&g_ring));
}

/** Crossing the uint16_t counter rollover must be invisible. */
void test_index_wraparound(void) {
  uint32_t seq = 0;
  // Just past 65536 bytes, so the counters roll over mid-run.
  while (seq < 70000u) {
    for (int i = 0; i < 250 && lh_ring_push(&g_ring, (uint8_t)(seq & 0xFF)); i++) seq++;

    uint8_t byte = 0;
    for (int i = 0; i < 250 && lh_ring_count(&g_ring) > 0; i++) {
      TEST_ASSERT_TRUE(lh_ring_pop(&g_ring, &byte));
    }
  }
  TEST_ASSERT_GREATER_THAN_UINT32(65536u, seq);
}

/** The bulk path an ISR actually uses, including the split across the end. */
void test_bulk_push_pop_across_the_seam(void) {
  uint8_t src[300];
  uint8_t dst[300];
  for (size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(i * 7 + 3);

  // Advance close to the end of the buffer so the next bulk write is split.
  for (uint16_t i = 0; i < LH_UART_RING_SIZE - 100; i++) lh_ring_push(&g_ring, 0);
  uint8_t scratch;
  for (uint16_t i = 0; i < LH_UART_RING_SIZE - 100; i++) lh_ring_pop(&g_ring, &scratch);

  TEST_ASSERT_EQUAL_UINT16(sizeof(src), lh_ring_push_bytes(&g_ring, src, sizeof(src)));
  TEST_ASSERT_EQUAL_UINT16(sizeof(dst), lh_ring_pop_bytes(&g_ring, dst, sizeof(dst)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(src, dst, sizeof(src));
}

/** Bulk overrun is counted per dropped byte, not per rejected call. */
void test_bulk_overrun_accounting(void) {
  static uint8_t big[LH_UART_RING_SIZE + 64];
  TEST_ASSERT_EQUAL_UINT16(LH_UART_RING_SIZE,
                           lh_ring_push_bytes(&g_ring, big, (uint16_t)sizeof(big)));
  TEST_ASSERT_EQUAL_UINT32(64, g_ring.stat_overrun);
}

void test_high_water_mark_tracks_peak(void) {
  for (uint16_t i = 0; i < 500; i++) lh_ring_push(&g_ring, (uint8_t)i);
  TEST_ASSERT_EQUAL_UINT16(500, g_ring.stat_hwm);

  uint8_t scratch;
  for (uint16_t i = 0; i < 400; i++) lh_ring_pop(&g_ring, &scratch);
  // Draining does not lower the peak — that is what makes it a warning light.
  TEST_ASSERT_EQUAL_UINT16(500, g_ring.stat_hwm);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_struct_size_is_budgeted);
  RUN_TEST(test_fresh_ring_is_empty);
  RUN_TEST(test_full_ring_is_not_mistaken_for_empty);
  RUN_TEST(test_index_wraparound);
  RUN_TEST(test_bulk_push_pop_across_the_seam);
  RUN_TEST(test_bulk_overrun_accounting);
  RUN_TEST(test_high_water_mark_tracks_peak);
  return UNITY_END();
}
