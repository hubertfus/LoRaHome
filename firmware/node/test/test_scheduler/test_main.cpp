/*
 * On-target scheduler tests (T3.7).
 *
 * The host harness in firmware/common/test/scheduler_selftest.c runs 200000
 * timed ticks against eight components and measures the maximum. This carries
 * the two things that are properties of the target rather than of the code:
 * the struct's size, which is a RAM budget decided by alignment rules, and the
 * structural property the whole tick budget rests on — at most one driver call
 * per component per tick.
 *
 * That second one is worth asserting on the ABI that ships because it is what
 * makes the on-target number predictable at all. On a host an I2C read is
 * nanoseconds and any scheduler looks fast; on a board it is milliseconds, and
 * the difference between "one step each" and "see it through" is the difference
 * between a 3 ms tick and a 30 ms one — which is a closed receive window and
 * frames lost only while a sensor happens to be busy (R3.1).
 *
 * Run with `pio test -e native -f test_scheduler`, or `-e esp32dev` on a board.
 */
#include <string.h>
#include <unity.h>

#include "lorahome/driver.h"
#include "lorahome/scheduler.h"

static uint32_t g_calls;

static lh_drv_err_t countingInit(lh_driver_ctx_t* ctx, uint8_t addr) {
  (void)ctx;
  (void)addr;
  return LH_DRV_OK;
}

static lh_drv_err_t countingStart(lh_driver_ctx_t* ctx) {
  (void)ctx;
  g_calls++;
  return LH_DRV_OK;
}

static lh_drv_err_t countingPoll(lh_driver_ctx_t* ctx, lh_reading_t* out, uint8_t channel) {
  g_calls++;
  out->value = 100 + static_cast<int32_t>(channel);
  out->quality = 100;
  out->ts_us = ctx->op_started_us;
  return LH_DRV_OK;
}

static const lh_driver_vtable_t MOCK = {
    /* .name            = */ "mock",
    /* .type_id         = */ 0xE001,
    /* .channel_count   = */ 2,
    /* .warmup_ms       = */ 0,
    /* .min_interval_ms = */ 100,
    /* .probe           = */ nullptr,
    /* .init            = */ countingInit,
    /* .start_read      = */ countingStart,
    /* .poll            = */ countingPoll,
    /* .sleep           = */ nullptr,
};

const lh_driver_vtable_t* const LH_DRIVERS[] = {&MOCK};
const uint8_t LH_DRIVER_COUNT = 1;

static lh_scheduler_t g_sched;
static int64_t g_now_us;

void setUp(void) {
  lh_sched_init(&g_sched);
  g_now_us = 1000000;
  g_calls = 0;
}
void tearDown(void) {}

/** The Etap 3 RAM budget, asserted on the ABI it is spent on. */
void test_scheduler_fits_the_ram_budget(void) {
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(1536, static_cast<uint32_t>(sizeof(lh_scheduler_t)));
  TEST_ASSERT_EQUAL_UINT32(5000, LH_SCHED_TICK_BUDGET_US);
  TEST_ASSERT_EQUAL_UINT32(1, LH_SCHED_STEPS_PER_TICK);
}

/**
 * At most one driver call per component per tick.
 *
 * The property the tick budget rests on: the worst tick is bounded by the
 * slowest single bus operation, not by the sum of eight of them.
 */
void test_one_call_per_component_per_tick(void) {
  for (uint8_t i = 0; i < LH_MAX_COMPONENTS; i++) {
    lh_sched_add(&g_sched, &MOCK, i, i, g_now_us);
  }

  uint32_t worst = 0;
  for (int tick = 0; tick < 500; tick++) {
    g_calls = 0;
    lh_sched_tick(&g_sched, g_now_us);
    if (g_calls > worst) worst = g_calls;
    g_now_us += 10000;
  }

  TEST_ASSERT_LESS_OR_EQUAL_UINT32(LH_MAX_COMPONENTS, worst);
  TEST_ASSERT_TRUE(g_sched.stat_readings > 0);
  TEST_ASSERT_EQUAL_UINT32(0, g_sched.stat_budget_overruns);
}

void test_readings_are_collected_for_every_channel(void) {
  lh_sched_add(&g_sched, &MOCK, 0x10, 0, g_now_us);

  for (int tick = 0; tick < 50; tick++) {
    lh_sched_tick(&g_sched, g_now_us);
    g_now_us += 10000;
  }

  lh_reading_t reading;
  for (uint8_t channel = 0; channel < 2; channel++) {
    TEST_ASSERT_TRUE(lh_sched_latest(&g_sched, 0, channel, &reading));
    TEST_ASSERT_EQUAL_INT32(100 + channel, reading.value);
  }
}

/** Nothing is readable before a component has produced anything. */
void test_nothing_is_readable_before_the_first_measurement(void) {
  lh_sched_add(&g_sched, &MOCK, 0x10, 0, g_now_us);

  lh_reading_t reading;
  TEST_ASSERT_FALSE(lh_sched_latest(&g_sched, 0, 0, &reading));
  TEST_ASSERT_FALSE(lh_sched_latest(&g_sched, 9, 0, &reading));
}

/** Eight components is the node's contract; the ninth is refused. */
void test_capacity(void) {
  for (uint8_t i = 0; i < LH_MAX_COMPONENTS; i++) {
    TEST_ASSERT_EQUAL_INT(i, lh_sched_add(&g_sched, &MOCK, i, i, g_now_us));
  }
  TEST_ASSERT_EQUAL_INT(LH_DRV_ERR_STATE, lh_sched_add(&g_sched, &MOCK, 99, 99, g_now_us));
  TEST_ASSERT_EQUAL_UINT8(LH_MAX_COMPONENTS, lh_sched_component_count(&g_sched));
}

/** A tick over budget is counted, so the DoD condition is observable on target. */
void test_budget_overruns_are_recorded(void) {
  lh_sched_add(&g_sched, &MOCK, 0x10, 0, g_now_us);

  lh_sched_record_tick(&g_sched, LH_SCHED_TICK_BUDGET_US);
  TEST_ASSERT_EQUAL_UINT32(0, g_sched.stat_budget_overruns);

  lh_sched_record_tick(&g_sched, LH_SCHED_TICK_BUDGET_US + 1);
  TEST_ASSERT_EQUAL_UINT32(1, g_sched.stat_budget_overruns);
  TEST_ASSERT_EQUAL_UINT32(LH_SCHED_TICK_BUDGET_US + 1, g_sched.stat_tick_max_us);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_scheduler_fits_the_ram_budget);
  RUN_TEST(test_one_call_per_component_per_tick);
  RUN_TEST(test_readings_are_collected_for_every_channel);
  RUN_TEST(test_nothing_is_readable_before_the_first_measurement);
  RUN_TEST(test_capacity);
  RUN_TEST(test_budget_overruns_are_recorded);
  return UNITY_END();
}
