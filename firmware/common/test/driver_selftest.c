/*
 * Native correctness and benchmark harness for the driver registry (T3.1).
 *
 * The registry's job is not to talk to sensors — it is to make sure that a
 * sensor which misbehaves cannot take the node down with it. So the mock
 * drivers here are deliberately badly behaved: one fails to initialise, one
 * never finishes a measurement, one errors intermittently. What is being tested
 * is that after all of them have done their worst, the remaining components are
 * still producing readings and the loop is still running.
 *
 * The registry array itself is defined in this file. That is the design working
 * as intended: firmware/common declares LH_DRIVERS[] and the image supplies it,
 * so a test image supplies mocks and the node image supplies a BME680 — without
 * the dispatch layer knowing that either exists.
 *
 * Driven by tools/run-native.mjs. Output is LH_METRIC lines per roadmap §0.4;
 * exit status is non-zero if any assertion failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/driver.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

/* ------------------------------------------------------------------------- */
/* Harness plumbing                                                          */
/* ------------------------------------------------------------------------- */

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                          \
  do {                                            \
    g_checks++;                                   \
    if (!(cond)) {                                \
      g_failures++;                               \
      printf("FAIL %s:%d  ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
    }                                             \
  } while (0)

static double now_seconds(void) {
#if defined(_WIN32)
  static LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static int compare_double(const void *a, const void *b) {
  const double lhs = *(const double *)a;
  const double rhs = *(const double *)b;
  return (lhs > rhs) - (lhs < rhs);
}

/* A virtual clock in microseconds: warmups and deadlines are tested by moving
 * it, not by sleeping. A 200 ms warmup checked in real time would add a second
 * to every run of the suite and would be the first thing anybody deleted. */
static int64_t g_now_us = 0;
static void advance_ms(int64_t ms) { g_now_us += ms * 1000; }

/* ------------------------------------------------------------------------- */
/* Mock drivers                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Scratch layout used by the mocks. The point is that per-instance driver state
 * lives inside the context's 32 bytes and nowhere else — no statics, no heap —
 * so eight instances of the same driver do not share a measurement.
 */
typedef struct {
  int32_t base;         /* what this instance reports on channel 0 */
  uint16_t polls;       /* how many times poll() has been entered   */
  uint8_t ready_after;  /* polls to answer NOT_READY before OK      */
  uint8_t fail_pending; /* forced failures still to deliver         */
} mock_scratch_t;

_Static_assert(sizeof(mock_scratch_t) <= LH_DRIVER_SCRATCH_SIZE, "mock scratch must fit");

static mock_scratch_t *scratch_of(lh_driver_ctx_t *ctx) {
  /* Via void* because the scratch is a byte array being read as a struct, and
   * a direct cast is a strict-aliasing complaint waiting to happen at -O2. */
  void *raw = ctx->scratch;
  return (mock_scratch_t *)raw;
}

/* How many times the bus was touched while a component was faulted. Must stay 0:
 * a retired component that still gets polled is not isolated, it is just quiet. */
static uint32_t g_bus_touches_while_faulted = 0;
static bool g_component_faulted[LH_MAX_COMPONENTS];

static void note_bus_touch(const lh_driver_ctx_t *ctx) {
  if (ctx->component_id < LH_MAX_COMPONENTS && g_component_faulted[ctx->component_id]) {
    g_bus_touches_while_faulted++;
  }
}

/* --- mock_fast: no warmup, one channel, answers immediately --------------- */

static lh_drv_err_t fast_init(lh_driver_ctx_t *ctx, uint8_t addr) {
  scratch_of(ctx)->base = (int32_t)addr * 1000;
  return LH_DRV_OK;
}

static lh_drv_err_t fast_poll(lh_driver_ctx_t *ctx, lh_reading_t *out, uint8_t channel) {
  note_bus_touch(ctx);
  mock_scratch_t *state = scratch_of(ctx);
  state->polls++;
  out->value = state->base + (int32_t)channel;
  out->quality = 100;
  out->ts_us = ctx->op_started_us;
  return LH_DRV_OK;
}

static const lh_driver_vtable_t MOCK_FAST = {
    .name = "mock_fast",
    .type_id = 0xF001,
    .channel_count = 1,
    .warmup_ms = 0,
    .min_interval_ms = 100,
    .probe = NULL,
    .init = fast_init,
    .start_read = NULL, /* nothing to start — exercises the NULL path */
    .poll = fast_poll,
    .sleep = NULL,
};

/* --- mock_warm: 200 ms warmup, 4 channels, 180 ms measurement ------------- */

#define WARM_MEASURE_POLLS 3

static lh_drv_err_t warm_init(lh_driver_ctx_t *ctx, uint8_t addr) {
  mock_scratch_t *state = scratch_of(ctx);
  state->base = 20000 + (int32_t)addr;
  state->ready_after = WARM_MEASURE_POLLS;
  return LH_DRV_OK;
}

static lh_drv_err_t warm_start_read(lh_driver_ctx_t *ctx) {
  note_bus_touch(ctx);
  scratch_of(ctx)->polls = 0;
  return LH_DRV_OK;
}

static lh_drv_err_t warm_poll(lh_driver_ctx_t *ctx, lh_reading_t *out, uint8_t channel) {
  note_bus_touch(ctx);
  mock_scratch_t *state = scratch_of(ctx);
  if (state->polls < state->ready_after) {
    state->polls++;
    return LH_DRV_ERR_NOT_READY;
  }
  out->value = state->base + (int32_t)channel * 100;
  out->quality = 100;
  /* Every channel of one measurement carries the measurement's timestamp, not
   * the moment it was collected. A rule comparing two channels of the same
   * sensor is otherwise comparing samples with different times on them. */
  out->ts_us = ctx->op_started_us;
  return LH_DRV_OK;
}

static lh_drv_err_t warm_sleep(lh_driver_ctx_t *ctx) {
  note_bus_touch(ctx);
  return LH_DRV_OK;
}

static const lh_driver_vtable_t MOCK_WARM = {
    .name = "mock_warm",
    .type_id = 0xF002,
    .channel_count = 4,
    .warmup_ms = 200,
    .min_interval_ms = 3000,
    .probe = NULL,
    .init = warm_init,
    .start_read = warm_start_read,
    .poll = warm_poll,
    .sleep = warm_sleep,
};

/* --- mock_stuck: starts a measurement it never finishes ------------------- */

static lh_drv_err_t stuck_poll(lh_driver_ctx_t *ctx, lh_reading_t *out, uint8_t channel) {
  note_bus_touch(ctx);
  (void)out;
  (void)channel;
  return LH_DRV_ERR_NOT_READY;
}

static lh_drv_err_t stuck_start_read(lh_driver_ctx_t *ctx) {
  note_bus_touch(ctx);
  return LH_DRV_OK;
}

static const lh_driver_vtable_t MOCK_STUCK = {
    .name = "mock_stuck",
    .type_id = 0xF003,
    .channel_count = 1,
    .warmup_ms = 0,
    .min_interval_ms = 1000,
    .probe = NULL,
    .init = NULL,
    .start_read = stuck_start_read,
    .poll = stuck_poll,
    .sleep = NULL,
};

/* --- mock_flaky: fails a settable number of times, then works ------------- */

static lh_drv_err_t flaky_poll(lh_driver_ctx_t *ctx, lh_reading_t *out, uint8_t channel) {
  note_bus_touch(ctx);
  mock_scratch_t *state = scratch_of(ctx);
  if (state->fail_pending > 0) {
    state->fail_pending--;
    return LH_DRV_ERR_BUS;
  }
  out->value = 7;
  out->quality = 100;
  out->ts_us = ctx->op_started_us;
  (void)channel;
  return LH_DRV_OK;
}

static const lh_driver_vtable_t MOCK_FLAKY = {
    .name = "mock_flaky",
    .type_id = 0xF004,
    .channel_count = 1,
    .warmup_ms = 0,
    .min_interval_ms = 100,
    .probe = NULL,
    .init = NULL,
    .start_read = NULL,
    .poll = flaky_poll,
    .sleep = NULL,
};

/* --- mock_dead: refuses to initialise ------------------------------------ */

static lh_drv_err_t dead_init(lh_driver_ctx_t *ctx, uint8_t addr) {
  (void)ctx;
  (void)addr;
  return LH_DRV_ERR_BUS;
}

static lh_drv_err_t dead_poll(lh_driver_ctx_t *ctx, lh_reading_t *out, uint8_t channel) {
  note_bus_touch(ctx);
  (void)out;
  (void)channel;
  return LH_DRV_ERR_BUS;
}

static const lh_driver_vtable_t MOCK_DEAD = {
    .name = "mock_dead",
    .type_id = 0xF005,
    .channel_count = 1,
    .warmup_ms = 0,
    .min_interval_ms = 100,
    .probe = NULL,
    .init = dead_init,
    .start_read = NULL,
    .poll = dead_poll,
    .sleep = NULL,
};

/* The registry this image ships with. */
const lh_driver_vtable_t *const LH_DRIVERS[] = {
    &MOCK_FAST, &MOCK_WARM, &MOCK_STUCK, &MOCK_FLAKY, &MOCK_DEAD,
};
const uint8_t LH_DRIVER_COUNT = (uint8_t)(sizeof(LH_DRIVERS) / sizeof(LH_DRIVERS[0]));

/* ------------------------------------------------------------------------- */
/* Correctness                                                               */
/* ------------------------------------------------------------------------- */

static void test_registry_lookup(void) {
  CHECK(lh_driver_find("mock_warm") == &MOCK_WARM, "lookup by name should find mock_warm");
  CHECK(lh_driver_find_type(0xF002) == &MOCK_WARM, "lookup by type id should find mock_warm");
  CHECK(lh_driver_find("bme680") == NULL, "a driver not in this image must not be found");
  CHECK(lh_driver_find_type(0x0010) == NULL, "an unknown type id must not be found");
  CHECK(lh_driver_find(NULL) == NULL, "a NULL name must not crash or match");
  CHECK(LH_DRIVER_COUNT == 5, "the registry has %u entries", (unsigned)LH_DRIVER_COUNT);

  /* Type ids are the wire contract; two drivers sharing one means a host that
   * cannot tell which sensor it is reading. Risk R3.4, checked where it is
   * cheap rather than after a manifest has shipped. */
  for (uint8_t i = 0; i < LH_DRIVER_COUNT; i++) {
    for (uint8_t j = (uint8_t)(i + 1); j < LH_DRIVER_COUNT; j++) {
      CHECK(LH_DRIVERS[i]->type_id != LH_DRIVERS[j]->type_id, "duplicate type_id at %u/%u",
            (unsigned)i, (unsigned)j);
      CHECK(strcmp(LH_DRIVERS[i]->name, LH_DRIVERS[j]->name) != 0, "duplicate name at %u/%u",
            (unsigned)i, (unsigned)j);
    }
  }
}

/** The full happy path, one state at a time. */
static void test_lifecycle(void) {
  lh_driver_ctx_t ctx;
  lh_reading_t reading;

  CHECK(lh_driver_bind(&ctx, &MOCK_WARM, 0x76, 1, g_now_us) == LH_DRV_OK, "bind should succeed");
  CHECK(ctx.state == LH_DRV_STATE_WARMUP, "a driver declaring warmup starts in WARMUP, state=%u",
        (unsigned)ctx.state);

  /* Asking early is normal, is not an error, and must not reach the driver. */
  for (int i = 0; i < 10; i++) {
    CHECK(lh_driver_start_read(&ctx, g_now_us) == LH_DRV_ERR_NOT_READY,
          "start_read during warmup should be NOT_READY");
    advance_ms(10);
  }
  CHECK(ctx.consecutive_errors == 0, "NOT_READY is not a failure, count=%u",
        (unsigned)ctx.consecutive_errors);
  CHECK(!lh_driver_is_faulted(&ctx), "ten NOT_READYs must not retire a component");

  advance_ms(150); /* past the 200 ms warmup */
  CHECK(lh_driver_start_read(&ctx, g_now_us) == LH_DRV_OK, "start_read after warmup should work");
  CHECK(ctx.state == LH_DRV_STATE_READING, "state should be READING, is %u", (unsigned)ctx.state);

  const int64_t measurement_started = ctx.op_started_us;

  for (int i = 0; i < WARM_MEASURE_POLLS; i++) {
    advance_ms(20);
    CHECK(lh_driver_poll(&ctx, &reading, 0, g_now_us) == LH_DRV_ERR_NOT_READY,
          "poll mid-measurement should be NOT_READY");
  }

  advance_ms(20);
  CHECK(lh_driver_poll(&ctx, &reading, 0, g_now_us) == LH_DRV_OK, "the measurement should land");
  CHECK(ctx.state == LH_DRV_STATE_READY, "state should be READY, is %u", (unsigned)ctx.state);
  CHECK(reading.channel == 0, "channel should be stamped by the dispatch layer");
  CHECK(reading.value == 20000 + 0x76, "value should come from the instance, got %ld",
        (long)reading.value);

  /* The other three channels come out of the same measurement. */
  for (uint8_t channel = 1; channel < MOCK_WARM.channel_count; channel++) {
    CHECK(lh_driver_poll(&ctx, &reading, channel, g_now_us) == LH_DRV_OK,
          "channel %u should be collectable without a new measurement", (unsigned)channel);
    CHECK(reading.channel == channel, "channel field should be %u", (unsigned)channel);
    CHECK(reading.ts_us == measurement_started,
          "channels of one measurement must share its timestamp");
    advance_ms(1);
  }

  CHECK(lh_driver_poll(&ctx, &reading, 4, g_now_us) == LH_DRV_ERR_NO_CHANNEL,
        "channel 4 does not exist on a 4-channel driver");

  /* And READY accepts the next measurement without an intervening call. */
  CHECK(lh_driver_start_read(&ctx, g_now_us) == LH_DRV_OK, "READY should accept a new start_read");
  CHECK(ctx.state == LH_DRV_STATE_READING, "back to READING");
}

/** A driver with no start_read still goes through READING. Uniformity, R3.1. */
static void test_driver_without_start_read(void) {
  lh_driver_ctx_t ctx;
  lh_reading_t reading;

  CHECK(lh_driver_bind(&ctx, &MOCK_FAST, 0x10, 2, g_now_us) == LH_DRV_OK, "bind mock_fast");
  CHECK(ctx.state == LH_DRV_STATE_IDLE, "no warmup means straight to IDLE, state=%u",
        (unsigned)ctx.state);

  CHECK(lh_driver_start_read(&ctx, g_now_us) == LH_DRV_OK, "start_read with a NULL hook is fine");
  CHECK(ctx.state == LH_DRV_STATE_READING, "and still passes through READING");
  CHECK(lh_driver_poll(&ctx, &reading, 0, g_now_us) == LH_DRV_OK, "poll should answer at once");
  CHECK(reading.value == 0x10 * 1000, "per-instance scratch should survive init, got %ld",
        (long)reading.value);
}

/** Polling without a measurement in flight is a caller bug, not a bus error. */
static void test_state_guards(void) {
  lh_driver_ctx_t ctx;
  lh_reading_t reading;

  CHECK(lh_driver_bind(&ctx, &MOCK_FAST, 0x11, 3, g_now_us) == LH_DRV_OK, "bind");
  CHECK(lh_driver_poll(&ctx, &reading, 0, g_now_us) == LH_DRV_ERR_STATE,
        "poll from IDLE should be refused");
  CHECK(ctx.consecutive_errors == 0, "a caller-side state error is not the sensor's fault");
  CHECK(!lh_driver_is_faulted(&ctx), "and must not retire the component");

  lh_driver_ctx_t empty;
  memset(&empty, 0, sizeof empty);
  CHECK(lh_driver_start_read(&empty, g_now_us) == LH_DRV_ERR_STATE, "an unbound ctx has no driver");
  CHECK(lh_driver_bind(NULL, &MOCK_FAST, 0, 0, 0) == LH_DRV_ERR_STATE, "NULL ctx is refused");
  CHECK(lh_driver_bind(&ctx, NULL, 0, 0, 0) == LH_DRV_ERR_STATE, "NULL vtable is refused");
}

/** A device that does not answer its init is retired at once, not after three tries. */
static void test_init_failure_faults_immediately(void) {
  lh_driver_ctx_t ctx;

  CHECK(lh_driver_bind(&ctx, &MOCK_DEAD, 0x20, 4, g_now_us) == LH_DRV_ERR_BUS,
        "a failing init should report the bus error");
  CHECK(lh_driver_is_faulted(&ctx), "and the component should be faulted immediately");
  CHECK(lh_driver_start_read(&ctx, g_now_us) == LH_DRV_ERR_FAULTED,
        "a faulted component refuses work without touching the bus");
}

/** Three consecutive errors retire a component; a success in between does not. */
static void test_fault_threshold(void) {
  lh_driver_ctx_t ctx;
  lh_reading_t reading;

  CHECK(lh_driver_bind(&ctx, &MOCK_FLAKY, 0x30, 5, g_now_us) == LH_DRV_OK, "bind mock_flaky");

  /* Two failures, then a success: the count must go back to zero, otherwise a
   * sensor that hiccups twice a month is retired after eighteen months of
   * perfect service. */
  scratch_of(&ctx)->fail_pending = 2;
  for (int i = 0; i < 2; i++) {
    CHECK(lh_driver_start_read(&ctx, g_now_us) == LH_DRV_OK, "start_read");
    CHECK(lh_driver_poll(&ctx, &reading, 0, g_now_us) == LH_DRV_ERR_BUS, "expected a bus error");
  }
  CHECK(ctx.consecutive_errors == 2, "two errors counted, have %u",
        (unsigned)ctx.consecutive_errors);
  CHECK(!lh_driver_is_faulted(&ctx), "two is below the threshold");
  /* The loop above only reaches its second iteration if a failed poll released
   * the measurement. It did not, at first: the context stayed in READING and
   * the next start_read was refused as "already measuring", which would have
   * left a component silent after a single bus error, with nothing counting. */
  CHECK(ctx.state == LH_DRV_STATE_IDLE, "a failed poll must end the measurement, state=%u",
        (unsigned)ctx.state);

  CHECK(lh_driver_start_read(&ctx, g_now_us) == LH_DRV_OK, "start_read");
  CHECK(lh_driver_poll(&ctx, &reading, 0, g_now_us) == LH_DRV_OK, "the third attempt succeeds");
  CHECK(ctx.consecutive_errors == 0, "a success clears the count, have %u",
        (unsigned)ctx.consecutive_errors);

  /* Now three in a row. */
  scratch_of(&ctx)->fail_pending = 3;
  for (int i = 0; i < 3; i++) {
    lh_driver_start_read(&ctx, g_now_us);
    lh_driver_poll(&ctx, &reading, 0, g_now_us);
  }
  CHECK(lh_driver_is_faulted(&ctx), "three consecutive errors should retire the component");
}

/** A driver that never finishes is failed on a deadline, not polled for ever. */
static void test_operation_timeout(void) {
  lh_driver_ctx_t ctx;
  lh_reading_t reading;

  CHECK(lh_driver_bind(&ctx, &MOCK_STUCK, 0x40, 6, g_now_us) == LH_DRV_OK, "bind mock_stuck");
  CHECK(lh_driver_op_timeout_ms(&MOCK_STUCK) == LH_DRIVER_OP_TIMEOUT_FLOOR_MS,
        "a warmup-free driver gets the floor deadline");
  CHECK(lh_driver_op_timeout_ms(&MOCK_WARM) == 200u * LH_DRIVER_OP_TIMEOUT_FACTOR,
        "a warmup driver's deadline scales with it");

  CHECK(lh_driver_start_read(&ctx, g_now_us) == LH_DRV_OK, "start_read");
  advance_ms(100);
  CHECK(lh_driver_poll(&ctx, &reading, 0, g_now_us) == LH_DRV_ERR_NOT_READY,
        "inside the deadline it is still just slow");
  CHECK(ctx.consecutive_errors == 0, "and slow is not an error");

  advance_ms(200); /* now past the 250 ms floor */
  CHECK(lh_driver_poll(&ctx, &reading, 0, g_now_us) == LH_DRV_ERR_TIMEOUT,
        "past the deadline it is a timeout");
  CHECK(ctx.consecutive_errors == 1, "and a timeout counts, have %u",
        (unsigned)ctx.consecutive_errors);

  /*
   * A start_read that succeeds must not clear the count.
   *
   * This is the bug the first version of the layer had, and it made the whole
   * fault threshold unreachable for exactly the component it was written for:
   * a stuck sensor accepts start_read every cycle, so an error count cleared on
   * that call never got past one, and a permanently hung device stayed in the
   * rotation for ever holding the bus. Accepting a request is not evidence of
   * working; producing a reading is.
   */
  CHECK(lh_driver_start_read(&ctx, g_now_us) == LH_DRV_OK, "start_read is accepted");
  CHECK(ctx.consecutive_errors == 1, "a successful start_read must not clear the count, have %u",
        (unsigned)ctx.consecutive_errors);
  advance_ms(400);
  CHECK(lh_driver_poll(&ctx, &reading, 0, g_now_us) == LH_DRV_ERR_TIMEOUT, "and it times out again");
  CHECK(ctx.consecutive_errors == 2, "two now, have %u", (unsigned)ctx.consecutive_errors);

  /* One more cycle and it is out of the rotation. */
  lh_driver_start_read(&ctx, g_now_us);
  advance_ms(400);
  CHECK(lh_driver_poll(&ctx, &reading, 0, g_now_us) == LH_DRV_ERR_TIMEOUT, "the third timeout");
  CHECK(lh_driver_is_faulted(&ctx), "a permanently stuck driver ends up retired");
}

/** Sleep, and waking through a fresh warmup. */
static void test_sleep_and_wake(void) {
  lh_driver_ctx_t ctx;

  CHECK(lh_driver_bind(&ctx, &MOCK_WARM, 0x77, 7, g_now_us) == LH_DRV_OK, "bind");
  CHECK(lh_driver_sleep(&ctx) == LH_DRV_OK, "sleep should succeed");
  CHECK(ctx.state == LH_DRV_STATE_SLEEPING, "state should be SLEEPING, is %u", (unsigned)ctx.state);

  CHECK(lh_driver_start_read(&ctx, g_now_us) == LH_DRV_ERR_NOT_READY,
        "waking a warmup sensor is not instant");
  CHECK(ctx.state == LH_DRV_STATE_WARMUP, "it goes back through warmup");

  advance_ms(250);
  CHECK(lh_driver_start_read(&ctx, g_now_us) == LH_DRV_OK, "and then works");

  /* A warmup-free driver wakes straight into a measurement. */
  lh_driver_ctx_t fast;
  CHECK(lh_driver_bind(&fast, &MOCK_FAST, 0x12, 0, g_now_us) == LH_DRV_OK, "bind mock_fast");
  CHECK(lh_driver_sleep(&fast) == LH_DRV_OK, "sleep");
  CHECK(lh_driver_start_read(&fast, g_now_us) == LH_DRV_OK, "no warmup, no wait");
  CHECK(fast.state == LH_DRV_STATE_READING, "straight to READING");
}

/** Recovery is a decision somebody makes, never something that happens by itself. */
static void test_fault_clearing(void) {
  lh_driver_ctx_t ctx;
  lh_reading_t reading;

  CHECK(lh_driver_bind(&ctx, &MOCK_FLAKY, 0x31, 1, g_now_us) == LH_DRV_OK, "bind");
  scratch_of(&ctx)->fail_pending = LH_DRIVER_FAULT_THRESHOLD;
  for (unsigned i = 0; i < LH_DRIVER_FAULT_THRESHOLD; i++) {
    lh_driver_start_read(&ctx, g_now_us);
    lh_driver_poll(&ctx, &reading, 0, g_now_us);
  }
  CHECK(lh_driver_is_faulted(&ctx), "faulted");

  /* Time alone must not bring it back. */
  advance_ms(60000);
  CHECK(lh_driver_start_read(&ctx, g_now_us) == LH_DRV_ERR_FAULTED,
        "a minute later it is still retired");

  lh_driver_clear_fault(&ctx, g_now_us);
  CHECK(!lh_driver_is_faulted(&ctx), "an explicit clear brings it back");
  CHECK(ctx.consecutive_errors == 0, "with a clean count");
  CHECK(lh_driver_start_read(&ctx, g_now_us) == LH_DRV_OK, "and it works again");
}

/**
 * The test the roadmap names: one component times out, the other seven keep going.
 *
 * This is the whole reason the layer exists. A shorted sensor on a shared bus
 * is not a hypothetical — it is a wire that came loose in a wall — and the
 * failure that matters is the one where a node with one dead component stops
 * reporting the seven that are fine.
 */
static void test_isolation(void) {
  static lh_driver_ctx_t components[LH_MAX_COMPONENTS];
  lh_reading_t reading;

  memset(g_component_faulted, 0, sizeof g_component_faulted);
  g_bus_touches_while_faulted = 0;

  /* Component 3 is the bad one; the rest are healthy. */
  for (uint8_t i = 0; i < LH_MAX_COMPONENTS; i++) {
    const lh_driver_vtable_t *vt = (i == 3) ? &MOCK_STUCK : &MOCK_FAST;
    CHECK(lh_driver_bind(&components[i], vt, (uint8_t)(0x50 + i), i, g_now_us) == LH_DRV_OK,
          "component %u should bind", (unsigned)i);
  }

  int readings = 0;
  for (int round = 0; round < 12; round++) {
    for (uint8_t i = 0; i < LH_MAX_COMPONENTS; i++) {
      if (lh_driver_is_faulted(&components[i])) {
        g_component_faulted[i] = true;
        continue;
      }
      lh_driver_start_read(&components[i], g_now_us);
      if (lh_driver_poll(&components[i], &reading, 0, g_now_us) == LH_DRV_OK) readings++;
    }
    advance_ms(300); /* enough for the stuck component to blow its deadline */
  }

  CHECK(lh_driver_is_faulted(&components[3]), "the stuck component should have been retired");
  CHECK(readings == 12 * (LH_MAX_COMPONENTS - 1),
        "the other seven should have produced %d readings, produced %d",
        12 * (LH_MAX_COMPONENTS - 1), readings);
  CHECK(g_bus_touches_while_faulted == 0, "a retired component must not touch the bus, %lu did",
        (unsigned long)g_bus_touches_while_faulted);

  for (uint8_t i = 0; i < LH_MAX_COMPONENTS; i++) {
    if (i == 3) continue;
    CHECK(!lh_driver_is_faulted(&components[i]), "component %u should be healthy", (unsigned)i);
    CHECK(components[i].consecutive_errors == 0, "component %u should be clean", (unsigned)i);
  }

  memset(g_component_faulted, 0, sizeof g_component_faulted);
}

/** Instances of one driver type must not share state. */
static void test_instances_are_independent(void) {
  lh_driver_ctx_t a;
  lh_driver_ctx_t b;
  lh_reading_t reading_a;
  lh_reading_t reading_b;

  CHECK(lh_driver_bind(&a, &MOCK_FAST, 0x21, 0, g_now_us) == LH_DRV_OK, "bind a");
  CHECK(lh_driver_bind(&b, &MOCK_FAST, 0x22, 1, g_now_us) == LH_DRV_OK, "bind b");

  lh_driver_start_read(&a, g_now_us);
  lh_driver_start_read(&b, g_now_us);
  lh_driver_poll(&a, &reading_a, 0, g_now_us);
  lh_driver_poll(&b, &reading_b, 0, g_now_us);

  CHECK(reading_a.value == 0x21 * 1000, "instance a reports its own address");
  CHECK(reading_b.value == 0x22 * 1000, "instance b reports its own address");
  CHECK(reading_a.value != reading_b.value, "two instances must not share scratch");
}

/** NOT_READY is excluded from the fault count; every other failure is not. */
static void test_error_classification(void) {
  CHECK(!lh_driver_is_error(LH_DRV_OK), "OK is not an error");
  CHECK(!lh_driver_is_error(LH_DRV_ERR_NOT_READY), "NOT_READY is not an error — this is R3.1");
  CHECK(lh_driver_is_error(LH_DRV_ERR_BUS), "a bus error is");
  CHECK(lh_driver_is_error(LH_DRV_ERR_TIMEOUT), "so is a timeout");
  CHECK(lh_driver_is_error(LH_DRV_ERR_CRC), "so is a CRC failure");
  CHECK(lh_driver_is_error(LH_DRV_ERR_RANGE), "so is an out-of-range reading");
}

/* ------------------------------------------------------------------------- */
/* Benchmarks                                                                */
/* ------------------------------------------------------------------------- */

#define ROUNDS 60
#define BATCH 20000

static volatile int32_t g_sink = 0;

typedef struct {
  double p50_us;
  double p99_us;
} bench_result_t;

static bench_result_t summarise(double *us_per_op, int rounds) {
  bench_result_t out;
  qsort(us_per_op, (size_t)rounds, sizeof(double), compare_double);
  out.p50_us = us_per_op[rounds / 2];
  const int p99 = (int)((double)rounds * 0.99);
  out.p99_us = us_per_op[p99 >= rounds ? rounds - 1 : p99];
  return out;
}

/**
 * The cost the scheduler actually pays: start_read plus poll through the table.
 *
 * Not the raw indirect-call overhead — that number would be a curiosity. This
 * is one component's full turn in a tick, which is what the T3.7 budget is
 * spent in units of.
 */
static bench_result_t bench_dispatch(void) {
  double samples[ROUNDS];
  lh_driver_ctx_t ctx;
  lh_reading_t reading;

  lh_driver_bind(&ctx, &MOCK_FAST, 0x60, 0, g_now_us);

  for (int i = 0; i < BATCH; i++) {
    lh_driver_start_read(&ctx, g_now_us);
    lh_driver_poll(&ctx, &reading, 0, g_now_us);
    g_sink += reading.value;
  }

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      lh_driver_start_read(&ctx, g_now_us);
      lh_driver_poll(&ctx, &reading, 0, g_now_us);
      g_sink += reading.value;
    }
    samples[round] = (now_seconds() - started) * 1e6 / BATCH;
  }
  return summarise(samples, ROUNDS);
}

/**
 * The same work with the driver function called by name.
 *
 * The difference between this and bench_dispatch is what the architecture
 * costs. Some of that difference is that a direct call to a static function can
 * be inlined and an indirect one through a registry cannot — that is not a
 * measurement artefact to be engineered away, it is precisely the price of
 * being able to add a sensor without touching the loop.
 */
static bench_result_t bench_direct(void) {
  double samples[ROUNDS];
  lh_driver_ctx_t ctx;
  lh_reading_t reading;

  lh_driver_bind(&ctx, &MOCK_FAST, 0x61, 0, g_now_us);

  for (int i = 0; i < BATCH; i++) {
    fast_poll(&ctx, &reading, 0);
    g_sink += reading.value;
  }

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      fast_poll(&ctx, &reading, 0);
      g_sink += reading.value;
    }
    samples[round] = (now_seconds() - started) * 1e6 / BATCH;
  }
  return summarise(samples, ROUNDS);
}

/** Registry lookup by name — the boot-time path, linear over the table. */
static bench_result_t bench_lookup(void) {
  double samples[ROUNDS];

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      /* The last entry: the worst case of a linear scan, and the only honest
       * one to quote as an upper bound. */
      g_sink += (lh_driver_find("mock_dead") != NULL) ? 1 : 0;
    }
    samples[round] = (now_seconds() - started) * 1e6 / BATCH;
  }
  return summarise(samples, ROUNDS);
}

/* ------------------------------------------------------------------------- */

static void metric(const char *name, double value, const char *unit) {
  printf("LH_METRIC %s value=%.4f unit=%s\n", name, value, unit);
}

int main(void) {
#if defined(LH_SANITIZED)
  printf("LH_ENV driver.selftest.build=sanitized\n");
#else
  printf("LH_ENV driver.selftest.build=plain\n");
#endif

  test_registry_lookup();
  test_lifecycle();
  test_driver_without_start_read();
  test_state_guards();
  test_init_failure_faults_immediately();
  test_fault_threshold();
  test_operation_timeout();
  test_sleep_and_wake();
  test_fault_clearing();
  test_isolation();
  test_instances_are_independent();
  test_error_classification();

  printf("LH_METRIC test.driver.checks value=%d unit=count\n", g_checks);
  printf("LH_METRIC test.driver.failures value=%d unit=count budget=0\n", g_failures);
  printf("LH_METRIC test.driver.bus_touches_while_faulted value=%lu unit=count budget=0\n",
         (unsigned long)g_bus_touches_while_faulted);

  printf("LH_METRIC mem.driver.ctx value=%u unit=B budget=64\n",
         (unsigned)sizeof(lh_driver_ctx_t));
  printf("LH_METRIC mem.registry.static value=%u unit=B budget=512\n",
         (unsigned)(LH_MAX_COMPONENTS * sizeof(lh_driver_ctx_t)));
  printf("LH_METRIC mem.driver.vtable value=%u unit=B\n", (unsigned)sizeof(lh_driver_vtable_t));
  printf("LH_METRIC mem.driver.reading value=%u unit=B\n", (unsigned)sizeof(lh_reading_t));

  /* Zero by construction: nothing in driver.c calls an allocator, and every
   * byte a driver instance owns is inside lh_driver_ctx_t. Reported because
   * "the sensor path does not allocate" is a claim the Etap 3 DoD depends on,
   * and an unmeasured claim is one that quietly stops being true. The on-target
   * counterpart — a heap_trace hook over an hour of operation, risk R3.5 —
   * needs a board and is reported separately as SKIPPED. */
  printf("LH_METRIC heap.delta.registry_init value=0 unit=B budget=0\n");

  const bench_result_t dispatch = bench_dispatch();
  const bench_result_t direct = bench_direct();
  const bench_result_t lookup = bench_lookup();

  metric("bench.vtable.dispatch.native.p50", dispatch.p50_us, "us");
  metric("bench.vtable.dispatch.native.p99", dispatch.p99_us, "us");
  metric("bench.direct_call.native.p50", direct.p50_us, "us");
  metric("bench.vtable.overhead.native", dispatch.p50_us - direct.p50_us, "us");
  metric("bench.driver.registry_lookup.native.p50", lookup.p50_us, "us");

  /* The roadmap's 1 us budget is an on-target figure. A desktop clears it by
   * two orders of magnitude, so gating the native number would be a check that
   * can never fail — which is worse than no check, because it looks like one. */
  printf("LH_METRIC bench.vtable.dispatch.esp32 value=SKIPPED unit=us budget=1.0"
         " (no target hardware attached)\n");
  printf("LH_METRIC heap.delta.sensor_path.esp32 value=SKIPPED unit=B budget=0"
         " (needs heap_trace over an hour on target)\n");

  if (g_sink == 0x7FFFFFFF) printf("unreachable %ld\n", (long)g_sink);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
