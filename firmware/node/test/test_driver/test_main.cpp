/*
 * On-target driver registry tests (T3.1).
 *
 * The host harness in firmware/common/test/driver_selftest.c does the volume
 * work — every state transition, the fault threshold, the isolation scenario,
 * benchmarks, sanitizers. What it cannot do is answer the two questions that
 * are properties of the target rather than of the code:
 *
 *   1. How big is lh_driver_ctx_t here? It holds an int64_t next to four bytes,
 *      so its size is decided by the target's alignment rules, and eight of
 *      them is a RAM budget the roadmap gates a release on.
 *   2. Does the dispatch layer behave the same when compiled as C++ by the
 *      Arduino toolchain? Designated initialisers on the vtable and the
 *      enum-to-uint8_t narrowing in the state field are both places where C and
 *      C++ can legitimately differ.
 *
 * The registry array is supplied here, with mock drivers. That is the whole
 * point of the design: the image decides what drivers exist, and a test image
 * decides they are mocks.
 *
 * Run with `pio test -e native -f test_driver`, or `-e esp32dev` on a board.
 */
#include <string.h>
#include <unity.h>

#include "lorahome/driver.h"

/* --------------------------------------------------------------------------
 * Mock drivers and the registry this image ships with
 * ------------------------------------------------------------------------ */

struct MockScratch {
  int32_t base;
  uint16_t polls;
  uint8_t ready_after;
  uint8_t fail_pending;
};
static_assert(sizeof(MockScratch) <= LH_DRIVER_SCRATCH_SIZE, "mock scratch must fit");

static MockScratch* scratchOf(lh_driver_ctx_t* ctx) {
  void* raw = ctx->scratch;
  return static_cast<MockScratch*>(raw);
}

static lh_drv_err_t instantInit(lh_driver_ctx_t* ctx, uint8_t addr) {
  scratchOf(ctx)->base = static_cast<int32_t>(addr) * 10;
  return LH_DRV_OK;
}

static lh_drv_err_t instantPoll(lh_driver_ctx_t* ctx, lh_reading_t* out, uint8_t channel) {
  MockScratch* state = scratchOf(ctx);
  if (state->fail_pending > 0) {
    state->fail_pending--;
    return LH_DRV_ERR_BUS;
  }
  out->value = state->base + static_cast<int32_t>(channel);
  out->quality = 100;
  out->ts_us = ctx->op_started_us;
  return LH_DRV_OK;
}

static const lh_driver_vtable_t MOCK_INSTANT = {
    /* .name            = */ "mock_instant",
    /* .type_id         = */ 0xF001,
    /* .channel_count   = */ 2,
    /* .warmup_ms       = */ 0,
    /* .min_interval_ms = */ 100,
    /* .probe           = */ nullptr,
    /* .init            = */ instantInit,
    /* .start_read      = */ nullptr,
    /* .poll            = */ instantPoll,
    /* .sleep           = */ nullptr,
};

/** Declares a 200 ms warmup and then answers. The BME680's shape, without a bus. */
static lh_drv_err_t warmPoll(lh_driver_ctx_t* ctx, lh_reading_t* out, uint8_t channel) {
  MockScratch* state = scratchOf(ctx);
  if (state->polls < state->ready_after) {
    state->polls++;
    return LH_DRV_ERR_NOT_READY;
  }
  out->value = 23450; /* milli-degrees: integer-scaled, never float. R3.3. */
  out->quality = 100;
  out->ts_us = ctx->op_started_us;
  (void)channel;
  return LH_DRV_OK;
}

static lh_drv_err_t warmStartRead(lh_driver_ctx_t* ctx) {
  MockScratch* state = scratchOf(ctx);
  state->polls = 0;
  state->ready_after = 2;
  return LH_DRV_OK;
}

static const lh_driver_vtable_t MOCK_WARM = {
    /* .name            = */ "mock_warm",
    /* .type_id         = */ 0xF002,
    /* .channel_count   = */ 1,
    /* .warmup_ms       = */ 200,
    /* .min_interval_ms = */ 3000,
    /* .probe           = */ nullptr,
    /* .init            = */ nullptr,
    /* .start_read      = */ warmStartRead,
    /* .poll            = */ warmPoll,
    /* .sleep           = */ nullptr,
};

const lh_driver_vtable_t* const LH_DRIVERS[] = {&MOCK_INSTANT, &MOCK_WARM};
const uint8_t LH_DRIVER_COUNT = static_cast<uint8_t>(sizeof(LH_DRIVERS) / sizeof(LH_DRIVERS[0]));

/* --------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------ */

static lh_driver_ctx_t g_ctx;
static int64_t g_now_us;

void setUp(void) {
  g_now_us = 0;
  memset(&g_ctx, 0, sizeof g_ctx);
}
void tearDown(void) {}

static void advanceMs(int64_t ms) { g_now_us += ms * 1000; }

/**
 * The Etap 3 RAM budget, asserted on the ABI it is spent on.
 *
 * Eight components is the hard limit from CONTRIBUTING.md §1.1, and 512 B is
 * what the roadmap allows the whole table. This is the check that fails when
 * somebody adds a field to the context, in the log of whoever is chasing bytes.
 */
void test_context_fits_the_ram_budget(void) {
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(64, static_cast<uint32_t>(sizeof(lh_driver_ctx_t)));
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(
      512, static_cast<uint32_t>(LH_MAX_COMPONENTS * sizeof(lh_driver_ctx_t)));
  TEST_ASSERT_EQUAL_UINT32(32, static_cast<uint32_t>(LH_DRIVER_SCRATCH_SIZE));
}

void test_registry_lookup(void) {
  TEST_ASSERT_EQUAL_PTR(&MOCK_WARM, lh_driver_find("mock_warm"));
  TEST_ASSERT_EQUAL_PTR(&MOCK_WARM, lh_driver_find_type(0xF002));
  TEST_ASSERT_NULL(lh_driver_find("not_in_this_image"));
  TEST_ASSERT_NULL(lh_driver_find_type(0x1234));
}

void test_bind_and_read(void) {
  TEST_ASSERT_EQUAL_INT(LH_DRV_OK, lh_driver_bind(&g_ctx, &MOCK_INSTANT, 0x76, 0, g_now_us));
  TEST_ASSERT_EQUAL_UINT8(LH_DRV_STATE_IDLE, g_ctx.state);

  lh_reading_t reading;
  TEST_ASSERT_EQUAL_INT(LH_DRV_OK, lh_driver_start_read(&g_ctx, g_now_us));
  TEST_ASSERT_EQUAL_UINT8(LH_DRV_STATE_READING, g_ctx.state);
  TEST_ASSERT_EQUAL_INT(LH_DRV_OK, lh_driver_poll(&g_ctx, &reading, 0, g_now_us));
  TEST_ASSERT_EQUAL_UINT8(LH_DRV_STATE_READY, g_ctx.state);
  TEST_ASSERT_EQUAL_INT32(0x76 * 10, reading.value);
  TEST_ASSERT_EQUAL_UINT8(0, reading.channel);
}

/**
 * Warmup is enforced by the dispatch layer, and the driver is never told.
 *
 * CONTRIBUTING.md §1.3 calls this data correctness rather than an optimisation:
 * a BME680 polled before it is warm returns numbers, and they are wrong. A rule
 * that every driver has to re-implement is a rule one driver gets wrong.
 */
void test_warmup_is_enforced_before_dispatch(void) {
  TEST_ASSERT_EQUAL_INT(LH_DRV_OK, lh_driver_bind(&g_ctx, &MOCK_WARM, 0x77, 1, g_now_us));
  TEST_ASSERT_EQUAL_UINT8(LH_DRV_STATE_WARMUP, g_ctx.state);

  TEST_ASSERT_EQUAL_INT(LH_DRV_ERR_NOT_READY, lh_driver_start_read(&g_ctx, g_now_us));
  advanceMs(199);
  TEST_ASSERT_EQUAL_INT(LH_DRV_ERR_NOT_READY, lh_driver_start_read(&g_ctx, g_now_us));
  TEST_ASSERT_EQUAL_UINT8(0, g_ctx.consecutive_errors);

  advanceMs(2);
  TEST_ASSERT_EQUAL_INT(LH_DRV_OK, lh_driver_start_read(&g_ctx, g_now_us));
}

/** NOT_READY must never count as a failure — risk R3.1 in one assertion. */
void test_not_ready_is_not_a_failure(void) {
  TEST_ASSERT_FALSE(lh_driver_is_error(LH_DRV_ERR_NOT_READY));
  TEST_ASSERT_FALSE(lh_driver_is_error(LH_DRV_OK));
  TEST_ASSERT_TRUE(lh_driver_is_error(LH_DRV_ERR_BUS));
  TEST_ASSERT_TRUE(lh_driver_is_error(LH_DRV_ERR_TIMEOUT));

  TEST_ASSERT_EQUAL_INT(LH_DRV_OK, lh_driver_bind(&g_ctx, &MOCK_WARM, 0x77, 1, g_now_us));
  advanceMs(200);
  TEST_ASSERT_EQUAL_INT(LH_DRV_OK, lh_driver_start_read(&g_ctx, g_now_us));

  lh_reading_t reading;
  for (int i = 0; i < 2; i++) {
    TEST_ASSERT_EQUAL_INT(LH_DRV_ERR_NOT_READY, lh_driver_poll(&g_ctx, &reading, 0, g_now_us));
    advanceMs(50);
  }
  TEST_ASSERT_EQUAL_UINT8(0, g_ctx.consecutive_errors);
  TEST_ASSERT_EQUAL_INT(LH_DRV_OK, lh_driver_poll(&g_ctx, &reading, 0, g_now_us));
  TEST_ASSERT_EQUAL_INT32(23450, reading.value);
}

/** Three consecutive errors retire a component; it then refuses work outright. */
void test_fault_threshold_retires_the_component(void) {
  TEST_ASSERT_EQUAL_INT(LH_DRV_OK, lh_driver_bind(&g_ctx, &MOCK_INSTANT, 0x10, 2, g_now_us));
  scratchOf(&g_ctx)->fail_pending = LH_DRIVER_FAULT_THRESHOLD;

  lh_reading_t reading;
  for (unsigned i = 0; i < LH_DRIVER_FAULT_THRESHOLD; i++) {
    lh_driver_start_read(&g_ctx, g_now_us);
    lh_driver_poll(&g_ctx, &reading, 0, g_now_us);
  }

  TEST_ASSERT_TRUE(lh_driver_is_faulted(&g_ctx));
  TEST_ASSERT_EQUAL_INT(LH_DRV_ERR_FAULTED, lh_driver_start_read(&g_ctx, g_now_us));
  TEST_ASSERT_EQUAL_INT(LH_DRV_ERR_FAULTED, lh_driver_poll(&g_ctx, &reading, 0, g_now_us));

  /* And it does not recover on its own — recovery is the host's decision. */
  advanceMs(3600000);
  TEST_ASSERT_EQUAL_INT(LH_DRV_ERR_FAULTED, lh_driver_start_read(&g_ctx, g_now_us));

  lh_driver_clear_fault(&g_ctx, g_now_us);
  TEST_ASSERT_FALSE(lh_driver_is_faulted(&g_ctx));
  TEST_ASSERT_EQUAL_INT(LH_DRV_OK, lh_driver_start_read(&g_ctx, g_now_us));
}

/** Asking for a channel the driver has not got is refused, not clamped. */
void test_channel_bounds(void) {
  TEST_ASSERT_EQUAL_INT(LH_DRV_OK, lh_driver_bind(&g_ctx, &MOCK_INSTANT, 0x11, 3, g_now_us));
  lh_reading_t reading;
  lh_driver_start_read(&g_ctx, g_now_us);
  TEST_ASSERT_EQUAL_INT(LH_DRV_OK, lh_driver_poll(&g_ctx, &reading, 1, g_now_us));
  TEST_ASSERT_EQUAL_INT(LH_DRV_ERR_NO_CHANNEL, lh_driver_poll(&g_ctx, &reading, 2, g_now_us));
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_context_fits_the_ram_budget);
  RUN_TEST(test_registry_lookup);
  RUN_TEST(test_bind_and_read);
  RUN_TEST(test_warmup_is_enforced_before_dispatch);
  RUN_TEST(test_not_ready_is_not_a_failure);
  RUN_TEST(test_fault_threshold_retires_the_component);
  RUN_TEST(test_channel_bounds);
  return UNITY_END();
}
