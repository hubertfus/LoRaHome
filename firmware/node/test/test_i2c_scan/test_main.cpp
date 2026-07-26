/*
 * On-target I2C scan tests (T3.2).
 *
 * The host harness in firmware/common/test/i2c_scan_selftest.c models nine
 * kinds of broken bus and benchmarks the loop. This carries the part that has
 * to hold on the ABI that ships: the result struct's size, the address range,
 * and the two behaviours a maintainer must not be able to change by accident —
 * that one stuck device does not stop a scan, and that a stuck *line* does.
 *
 * Those two look similar and are opposites. A scanner that treats them the same
 * either abandons a working bus because one sensor is dead, or spends 448 ms
 * not servicing the radio to rediscover that a wire is shorted. Both are ways a
 * node in a wall stops being useful, and both are cheap to reintroduce, which
 * is why they are asserted here as well as in the harness.
 *
 * Run with `pio test -e native -f test_i2c_scan`, or `-e esp32dev` on a board.
 */
#include <string.h>
#include <unity.h>

#include "lorahome/i2c_scan.h"

/* --------------------------------------------------------------------------
 * A bus that can be told how to misbehave
 * ------------------------------------------------------------------------ */

struct MockBus {
  int64_t clock_us;
  uint8_t stuck_addr;  /* one device holding the line; 0xFF for none */
  bool line_held_low;  /* every address times out                    */
  uint8_t device_addr; /* a device that answers; 0xFF for none       */
};

static MockBus g_bus;

static int64_t mockNow(void* user) { return static_cast<MockBus*>(user)->clock_us; }

static lh_i2c_probe_r mockProbe(uint8_t addr, void* user) {
  MockBus* bus = static_cast<MockBus*>(user);

  if (bus->line_held_low || addr == bus->stuck_addr) {
    bus->clock_us += static_cast<int64_t>(LH_I2C_PROBE_TIMEOUT_MS) * 1000;
    return LH_I2C_TIMEOUT;
  }
  bus->clock_us += 50;
  return (addr == bus->device_addr) ? LH_I2C_ACK : LH_I2C_NACK;
}

static lh_i2c_bus_t busHandle() {
  lh_i2c_bus_t bus;
  bus.probe = mockProbe;
  bus.now_us = mockNow;
  bus.user = &g_bus;
  return bus;
}

void setUp(void) {
  g_bus.clock_us = 0;
  g_bus.stuck_addr = 0xFF;
  g_bus.line_held_low = false;
  g_bus.device_addr = 0x76;
}
void tearDown(void) {}

/* --------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------ */

/** The result lives in the component context's neighbourhood; its size is a budget. */
void test_result_fits_the_budget(void) {
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(64, static_cast<uint32_t>(sizeof(lh_i2c_scan_result_t)));
}

/**
 * The per-address timeout and the scan budget are one decision.
 *
 * 112 addresses at the per-address timeout is what a sweep of a dead bus costs,
 * so the two numbers cannot be chosen separately. Asserted at runtime as well
 * as at compile time because the compile-time version lives in a translation
 * unit an on-target build may not include.
 */
void test_timeout_is_derived_from_the_scan_budget(void) {
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(5u, LH_I2C_PROBE_TIMEOUT_MS);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(
      LH_I2C_SCAN_BUDGET_MS, LH_I2C_ADDR_COUNT * LH_I2C_PROBE_TIMEOUT_MS);
}

/** Reserved addresses at both ends are never put on the wire. */
void test_address_range(void) {
  TEST_ASSERT_EQUAL_UINT8(0x08, LH_I2C_ADDR_FIRST);
  TEST_ASSERT_EQUAL_UINT8(0x77, LH_I2C_ADDR_LAST);
  TEST_ASSERT_EQUAL_UINT16(112, LH_I2C_ADDR_COUNT);

  lh_i2c_bus_t bus = busHandle();
  lh_i2c_scan_result_t result;
  TEST_ASSERT_TRUE(lh_i2c_scan(&bus, &result));
  TEST_ASSERT_EQUAL_UINT8(LH_I2C_ADDR_COUNT, result.probed);
}

void test_finds_a_device(void) {
  lh_i2c_bus_t bus = busHandle();
  lh_i2c_scan_result_t result;

  TEST_ASSERT_TRUE(lh_i2c_scan(&bus, &result));
  TEST_ASSERT_EQUAL_UINT8(1, result.found_count);
  TEST_ASSERT_TRUE(lh_i2c_scan_contains(&result, 0x76));
  TEST_ASSERT_FALSE(lh_i2c_scan_contains(&result, 0x77));
  TEST_ASSERT_FALSE(result.wedged);
}

/** One device holding the line costs one timeout. The rest of the bus is scanned. */
void test_one_stuck_device_does_not_block_the_scan(void) {
  g_bus.stuck_addr = 0x50;

  lh_i2c_bus_t bus = busHandle();
  lh_i2c_scan_result_t result;

  TEST_ASSERT_TRUE(lh_i2c_scan(&bus, &result));
  TEST_ASSERT_EQUAL_UINT8(LH_I2C_ADDR_COUNT, result.probed);
  TEST_ASSERT_EQUAL_UINT8(1, result.timeouts);
  TEST_ASSERT_FALSE(result.wedged);
  /* 0x76 is past 0x50: the scan got beyond the fault. */
  TEST_ASSERT_TRUE(lh_i2c_scan_contains(&result, 0x76));
}

/** A line held low is abandoned early, and the scanner survives it. */
void test_shorted_bus_aborts_and_the_system_lives(void) {
  g_bus.line_held_low = true;

  lh_i2c_bus_t bus = busHandle();
  lh_i2c_scan_result_t result;

  TEST_ASSERT_FALSE(lh_i2c_scan(&bus, &result));
  TEST_ASSERT_TRUE(result.wedged);
  TEST_ASSERT_EQUAL_UINT8(LH_I2C_WEDGED_BUS_TIMEOUTS, result.probed);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(
      LH_I2C_WEDGED_BUS_TIMEOUTS * LH_I2C_PROBE_TIMEOUT_MS, lh_i2c_scan_duration_ms(&result));

  /* The short is repaired; the next scan works. This is the half of R3.2 that
   * matters — not that the scan failed, but that the node is still here. */
  g_bus.line_held_low = false;
  g_bus.clock_us = 0;
  lh_i2c_scan_result_t after;
  TEST_ASSERT_TRUE(lh_i2c_scan(&bus, &after));
  TEST_ASSERT_EQUAL_UINT8(1, after.found_count);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_result_fits_the_budget);
  RUN_TEST(test_timeout_is_derived_from_the_scan_budget);
  RUN_TEST(test_address_range);
  RUN_TEST(test_finds_a_device);
  RUN_TEST(test_one_stuck_device_does_not_block_the_scan);
  RUN_TEST(test_shorted_bus_aborts_and_the_system_lives);
  return UNITY_END();
}
