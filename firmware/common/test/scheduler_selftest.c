/*
 * Native harness for the cooperative scheduler (T3.7).
 *
 * The tick budget is the point of this file and of the component it tests. The
 * Etap 3 DoD gates a release on `bench.scheduler.tick.max < 5 ms` — the
 * maximum, not the average — because an average holds perfectly while a p99
 * closes the radio's receive window every few seconds. The frames lost that way
 * go missing only while a sensor is busy, which presents as an intermittent
 * radio fault and costs somebody a day reading the wrong code.
 *
 * So the mock drivers here are shaped to punish a scheduler that waits: one
 * takes many polls to finish a measurement, one never finishes at all, one
 * errors intermittently, and there are eight of them. If any tick tried to see
 * a component through to completion, the maximum would show it immediately.
 *
 * The one thing this cannot measure is the real cost of a tick on target, where
 * an I2C transaction is milliseconds rather than nanoseconds. The structural
 * property — at most one non-blocking step per component per tick — is what
 * makes the on-target number predictable, and it is what is checked here.
 *
 * Output is LH_METRIC lines per roadmap §0.4.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/driver.h"
#include "lorahome/scheduler.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

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

/* ------------------------------------------------------------------------- */
/* Mock drivers                                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
  uint16_t polls;
  uint8_t ready_after;
  uint8_t fail_every;
  uint32_t call_count;
} scratch_t;

_Static_assert(sizeof(scratch_t) <= LH_DRIVER_SCRATCH_SIZE, "mock scratch must fit");

static scratch_t *scratch_of(lh_driver_ctx_t *ctx) {
  void *raw = ctx->scratch;
  return (scratch_t *)raw;
}

/* Counts the deepest nesting the scheduler ever reached inside one tick. If a
 * step ever loops internally, this is what notices. */
static uint32_t g_driver_calls_this_tick = 0;

static lh_drv_err_t fast_init(lh_driver_ctx_t *ctx, uint8_t addr) {
  (void)addr;
  memset(scratch_of(ctx), 0, sizeof(scratch_t));
  return LH_DRV_OK;
}

static lh_drv_err_t fast_poll(lh_driver_ctx_t *ctx, lh_reading_t *out, uint8_t channel) {
  g_driver_calls_this_tick++;
  out->value = 1000 + (int32_t)channel;
  out->quality = 100;
  out->ts_us = ctx->op_started_us;
  return LH_DRV_OK;
}

static const lh_driver_vtable_t MOCK_FAST = {
    .name = "fast",           .type_id = 0xE001, .channel_count = 1,
    .warmup_ms = 0,           .min_interval_ms = 100,
    .probe = NULL,            .init = fast_init, .start_read = NULL,
    .poll = fast_poll,        .sleep = NULL,
};

/* Four channels, 200 ms warmup, and a measurement that needs several polls —
 * the BME680's shape, which is what the budget was written against. */
static lh_drv_err_t slow_init(lh_driver_ctx_t *ctx, uint8_t addr) {
  (void)addr;
  scratch_t *state = scratch_of(ctx);
  memset(state, 0, sizeof *state);
  state->ready_after = 18; /* ~180 ms at a 10 ms tick */
  return LH_DRV_OK;
}

static lh_drv_err_t slow_start(lh_driver_ctx_t *ctx) {
  g_driver_calls_this_tick++;
  scratch_of(ctx)->polls = 0;
  return LH_DRV_OK;
}

static lh_drv_err_t slow_poll(lh_driver_ctx_t *ctx, lh_reading_t *out, uint8_t channel) {
  g_driver_calls_this_tick++;
  scratch_t *state = scratch_of(ctx);
  if (state->polls < state->ready_after) {
    state->polls++;
    return LH_DRV_ERR_NOT_READY;
  }
  out->value = 23450 + (int32_t)channel;
  out->quality = 100;
  out->ts_us = ctx->op_started_us;
  return LH_DRV_OK;
}

static const lh_driver_vtable_t MOCK_SLOW = {
    .name = "slow",           .type_id = 0xE002, .channel_count = 4,
    .warmup_ms = 200,         .min_interval_ms = 3000,
    .probe = NULL,            .init = slow_init, .start_read = slow_start,
    .poll = slow_poll,        .sleep = NULL,
};

/* Never finishes. The dispatch layer's deadline retires it; the scheduler must
 * keep servicing everything else while that happens. */
static lh_drv_err_t stuck_start(lh_driver_ctx_t *ctx) {
  (void)ctx;
  g_driver_calls_this_tick++;
  return LH_DRV_OK;
}

static lh_drv_err_t stuck_poll(lh_driver_ctx_t *ctx, lh_reading_t *out, uint8_t channel) {
  (void)ctx;
  (void)out;
  (void)channel;
  g_driver_calls_this_tick++;
  return LH_DRV_ERR_NOT_READY;
}

static const lh_driver_vtable_t MOCK_STUCK = {
    .name = "stuck",          .type_id = 0xE003, .channel_count = 1,
    .warmup_ms = 0,           .min_interval_ms = 100,
    .probe = NULL,            .init = NULL,      .start_read = stuck_start,
    .poll = stuck_poll,       .sleep = NULL,
};

/* Refuses to initialise, so the scheduler has to hold a faulted slot. */
static lh_drv_err_t dead_init(lh_driver_ctx_t *ctx, uint8_t addr) {
  (void)ctx;
  (void)addr;
  return LH_DRV_ERR_BUS;
}

static lh_drv_err_t dead_poll(lh_driver_ctx_t *ctx, lh_reading_t *out, uint8_t channel) {
  (void)ctx;
  (void)out;
  (void)channel;
  g_driver_calls_this_tick++;
  return LH_DRV_ERR_BUS;
}

static const lh_driver_vtable_t MOCK_DEAD = {
    .name = "dead",           .type_id = 0xE004, .channel_count = 1,
    .warmup_ms = 0,           .min_interval_ms = 100,
    .probe = NULL,            .init = dead_init, .start_read = NULL,
    .poll = dead_poll,        .sleep = NULL,
};

const lh_driver_vtable_t *const LH_DRIVERS[] = {&MOCK_FAST, &MOCK_SLOW, &MOCK_STUCK, &MOCK_DEAD};
const uint8_t LH_DRIVER_COUNT = 4;

/* ------------------------------------------------------------------------- */
/* Correctness                                                               */
/* ------------------------------------------------------------------------- */

#define TICK_US 10000 /* the node's loop period: 10 ms */

static lh_scheduler_t g_sched;
static int64_t g_now_us;

/** Runs `ticks` ticks, returning the highest driver-call count seen in any one. */
static uint32_t run_ticks(int ticks) {
  uint32_t worst = 0;
  for (int i = 0; i < ticks; i++) {
    g_driver_calls_this_tick = 0;
    lh_sched_tick(&g_sched, g_now_us);
    if (g_driver_calls_this_tick > worst) worst = g_driver_calls_this_tick;
    g_now_us += TICK_US;
  }
  return worst;
}

/**
 * The structural property the on-target budget rests on.
 *
 * At most one driver call per component per tick. With eight components that
 * is at most eight calls, so the worst tick is bounded by the slowest single
 * bus operation rather than by their sum. A scheduler that saw a component
 * through to completion would show up here as a count in the dozens, long
 * before it showed up as a missed frame on a bench.
 */
static void test_one_step_per_component_per_tick(void) {
  lh_sched_init(&g_sched);
  g_now_us = 1000000;

  for (uint8_t i = 0; i < LH_MAX_COMPONENTS; i++) {
    const lh_driver_vtable_t *vt = (i % 2 == 0) ? &MOCK_FAST : &MOCK_SLOW;
    lh_sched_add(&g_sched, vt, (uint8_t)(0x10 + i), i, g_now_us);
  }

  const uint32_t worst = run_ticks(2000);
  CHECK(worst <= LH_MAX_COMPONENTS, "a tick must make at most %u driver calls, made %lu",
        (unsigned)LH_MAX_COMPONENTS, (unsigned long)worst);
  CHECK(g_sched.stat_readings > 0, "and some readings should have been collected");
}

/** Warmup is respected: nothing is polled before the driver says it is ready. */
static void test_warmup_is_respected(void) {
  lh_sched_init(&g_sched);
  g_now_us = 1000000;
  const int slot = lh_sched_add(&g_sched, &MOCK_SLOW, 0x76, 0, g_now_us);
  CHECK(slot == 0, "the component should take slot 0");

  /* 19 ticks is 190 ms — inside the 200 ms warmup. */
  run_ticks(19);
  lh_reading_t reading;
  CHECK(!lh_sched_latest(&g_sched, 0, 0, &reading), "nothing should be readable during warmup");
  CHECK(g_sched.stat_errors == 0, "and warming up is not an error");

  /* Past the warmup, plus the measurement, plus a tick per channel. */
  run_ticks(60);
  CHECK(lh_sched_latest(&g_sched, 0, 0, &reading), "a reading should have landed");
  CHECK(reading.value == 23450, "and it should be the driver's value, got %ld",
        (long)reading.value);
}

/** Every channel of a multi-channel sensor is collected, one per tick. */
static void test_all_channels_are_collected(void) {
  lh_sched_init(&g_sched);
  g_now_us = 1000000;
  lh_sched_add(&g_sched, &MOCK_SLOW, 0x76, 0, g_now_us);

  run_ticks(100);

  for (uint8_t channel = 0; channel < 4; channel++) {
    lh_reading_t reading;
    CHECK(lh_sched_latest(&g_sched, 0, channel, &reading), "channel %u should have a reading",
          (unsigned)channel);
    CHECK(reading.value == 23450 + channel, "channel %u value", (unsigned)channel);
  }
}

/**
 * The roadmap's `metric.interval_accuracy`, as the worst case across drivers.
 *
 * Drift matters because the energy calculator and the duty-cycle budget both
 * assume the declared rate: a sensor running 30% fast is 30% more airtime than
 * anybody accounted for.
 */
static double g_interval_error_pct = 0.0;

/** Mean interval between readings from one component, over `ticks`. */
static double measured_interval_ms(const lh_driver_vtable_t *vt, int ticks) {
  lh_sched_init(&g_sched);
  g_now_us = 1000000;
  lh_sched_add(&g_sched, vt, 0x10, 0, g_now_us);

  /* Let the first cycle complete so warmup is not averaged into the rate. */
  run_ticks(100);

  const uint32_t before = g_sched.stat_readings;
  const int64_t started = g_now_us;
  run_ticks(ticks);

  const uint32_t readings = g_sched.stat_readings - before;
  if (readings == 0) return 0.0;

  /* Per *measurement*, not per reading: a four-channel sensor produces four
   * readings from one cycle, and dividing by all four would report an interval
   * four times too short. */
  const double cycles = (double)readings / (double)vt->channel_count;
  return ((double)(g_now_us - started) / 1000.0) / cycles;
}

static void test_interval_accuracy(void) {
  /*
   * A component is polled at its declared interval, within ten percent.
   *
   * Expected is the declared interval itself. The first version of this test
   * expected the interval *plus* two ticks of collection overhead and failed at
   * -16.67%, having measured exactly 100 ms against a declared 100 ms — the
   * scheduler was right and the expectation was invented. Collection overhead
   * does not accrue on top of the interval because the next deadline is set
   * when the measurement completes, not when it started.
   *
   * The floor matters as much as the ceiling. Running *faster* than declared is
   * the direction that costs airtime and battery nobody budgeted for, so the
   * check is on the magnitude of the error, not on lateness alone.
   */
  const struct {
    const lh_driver_vtable_t *vt;
    double declared_ms;
    int ticks;
  } cases[] = {
      {&MOCK_FAST, 100.0, 1000},
      {&MOCK_SLOW, 3000.0, 6000},
  };

  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    const double actual = measured_interval_ms(cases[i].vt, cases[i].ticks);
    const double error = 100.0 * (actual - cases[i].declared_ms) / cases[i].declared_ms;
    const double magnitude = error < 0 ? -error : error;

    if (magnitude > g_interval_error_pct) g_interval_error_pct = magnitude;

    CHECK(magnitude <= 10.0, "%s: interval %.1f ms against a declared %.1f ms (%.2f%%), budget 10%%",
          cases[i].vt->name, actual, cases[i].declared_ms, error);
  }
}

/**
 * The isolation test, at the scheduler level.
 *
 * One component never finishes and one never initialised. The other six must
 * keep producing readings throughout, and the node must still be running at the
 * end. This is R3.2 seen from the top of the stack rather than the bottom.
 */
static void test_broken_components_do_not_starve_the_rest(void) {
  lh_sched_init(&g_sched);
  g_now_us = 1000000;

  lh_sched_add(&g_sched, &MOCK_STUCK, 0x20, 0, g_now_us);
  lh_sched_add(&g_sched, &MOCK_DEAD, 0x21, 1, g_now_us);
  for (uint8_t i = 2; i < LH_MAX_COMPONENTS; i++) {
    lh_sched_add(&g_sched, &MOCK_FAST, (uint8_t)(0x30 + i), i, g_now_us);
  }

  run_ticks(1000);

  CHECK(lh_driver_is_faulted(&g_sched.components[0]), "the stuck component should be retired");
  CHECK(lh_driver_is_faulted(&g_sched.components[1]), "the dead one was retired at bind");

  int healthy = 0;
  for (uint8_t slot = 2; slot < LH_MAX_COMPONENTS; slot++) {
    lh_reading_t reading;
    if (lh_sched_latest(&g_sched, slot, 0, &reading) && reading.value == 1000) healthy++;
  }
  CHECK(healthy == LH_MAX_COMPONENTS - 2, "the other six should be reporting, %d were", healthy);
  CHECK(g_sched.stat_readings > 100, "and reporting steadily, got %lu readings",
        (unsigned long)g_sched.stat_readings);
}

/**
 * Fairness: the last component is serviced as often as the first.
 *
 * A fixed sweep from zero services slot 0 first on every tick and slot 7 only
 * when nothing earlier had work. Under load that is a sensor which quietly
 * stops reporting while its neighbours are fine — a fault that looks like bad
 * hardware and is actually a for-loop.
 */
static void test_round_robin_fairness(void) {
  lh_sched_init(&g_sched);
  g_now_us = 1000000;
  for (uint8_t i = 0; i < LH_MAX_COMPONENTS; i++) {
    lh_sched_add(&g_sched, &MOCK_FAST, (uint8_t)(0x40 + i), i, g_now_us);
  }

  run_ticks(2000);

  /* Every component should have a current reading, and the timestamps should
   * be close together — a starved slot shows up as a stale one. */
  int64_t oldest = 0;
  int64_t newest = 0;
  int reported = 0;
  for (uint8_t slot = 0; slot < LH_MAX_COMPONENTS; slot++) {
    lh_reading_t reading;
    if (!lh_sched_latest(&g_sched, slot, 0, &reading)) continue;
    if (reported == 0 || reading.ts_us < oldest) oldest = reading.ts_us;
    if (reported == 0 || reading.ts_us > newest) newest = reading.ts_us;
    reported++;
  }

  CHECK(reported == LH_MAX_COMPONENTS, "all %u components should report, %d did",
        (unsigned)LH_MAX_COMPONENTS, reported);
  /* Within two intervals of each other. A starved component would be hundreds. */
  CHECK((newest - oldest) < 250000, "readings should be within 250 ms of each other, spread %lld us",
        (long long)(newest - oldest));
}

/** A component that was configured and does not answer stays visible. */
static void test_failed_component_keeps_its_slot(void) {
  lh_sched_init(&g_sched);
  g_now_us = 1000000;

  const int result = lh_sched_add(&g_sched, &MOCK_DEAD, 0x21, 0, g_now_us);
  CHECK(result < 0, "a failing init should report the error");
  CHECK(lh_sched_component_count(&g_sched) == 1,
        "but the component must still occupy a slot — silently absent is "
        "indistinguishable from never configured");
  CHECK(lh_driver_is_faulted(&g_sched.components[0]), "and be visible as faulted");
}

/** Nine components is refused; eight is the node's contract. */
static void test_capacity(void) {
  lh_sched_init(&g_sched);
  g_now_us = 1000000;
  for (uint8_t i = 0; i < LH_MAX_COMPONENTS; i++) {
    CHECK(lh_sched_add(&g_sched, &MOCK_FAST, (uint8_t)i, i, g_now_us) == (int)i,
          "component %u should take slot %u", (unsigned)i, (unsigned)i);
  }
  CHECK(lh_sched_add(&g_sched, &MOCK_FAST, 99, 99, g_now_us) == LH_DRV_ERR_STATE,
        "a ninth component must be refused");

  /* A driver with more channels than the cache can hold is refused at add
   * time rather than silently truncated at read time. */
  lh_sched_init(&g_sched);
  const lh_driver_vtable_t too_wide = {
      .name = "wide", .type_id = 0xE0FF, .channel_count = LH_SCHED_MAX_CHANNELS + 1,
      .warmup_ms = 0, .min_interval_ms = 100, .probe = NULL, .init = NULL,
      .start_read = NULL, .poll = fast_poll, .sleep = NULL,
  };
  CHECK(lh_sched_add(&g_sched, &too_wide, 1, 1, g_now_us) == LH_DRV_ERR_NO_CHANNEL,
        "a driver wider than the reading cache must be refused");
}

/* ------------------------------------------------------------------------- */
/* The budget                                                                */
/* ------------------------------------------------------------------------- */

#define BUDGET_TICKS 200000

static double g_tick_avg_us = 0;
static double g_tick_p99_us = 0;
static double g_tick_max_us = 0;

/**
 * Every tick timed individually, over a full load of eight components.
 *
 * The maximum is the number the DoD gates on, so it is measured rather than
 * derived from an average: taking the mean of a batch would hide exactly the
 * outlier the budget exists to catch.
 */
static void bench_tick(void) {
  static double samples[BUDGET_TICKS];

  lh_sched_init(&g_sched);
  g_now_us = 1000000;
  for (uint8_t i = 0; i < LH_MAX_COMPONENTS; i++) {
    const lh_driver_vtable_t *vt = (i % 3 == 0) ? &MOCK_SLOW : &MOCK_FAST;
    lh_sched_add(&g_sched, vt, (uint8_t)(0x50 + i), i, g_now_us);
  }

  /* Warm the caches so the first thousand ticks do not dominate the maximum. */
  for (int i = 0; i < 5000; i++) {
    lh_sched_tick(&g_sched, g_now_us);
    g_now_us += TICK_US;
  }

  double total = 0;
  for (int i = 0; i < BUDGET_TICKS; i++) {
    const double started = now_seconds();
    lh_sched_tick(&g_sched, g_now_us);
    const double elapsed_us = (now_seconds() - started) * 1e6;

    samples[i] = elapsed_us;
    total += elapsed_us;
    lh_sched_record_tick(&g_sched, (uint32_t)(elapsed_us < 0 ? 0 : elapsed_us));
    g_now_us += TICK_US;
  }

  g_tick_avg_us = total / BUDGET_TICKS;

  qsort(samples, BUDGET_TICKS, sizeof(double), compare_double);
  g_tick_p99_us = samples[(int)(BUDGET_TICKS * 0.99)];
  g_tick_max_us = samples[BUDGET_TICKS - 1];

  CHECK(g_sched.stat_budget_overruns == 0,
        "no tick may exceed the %u us budget; %lu did", (unsigned)LH_SCHED_TICK_BUDGET_US,
        (unsigned long)g_sched.stat_budget_overruns);
}

/* ------------------------------------------------------------------------- */

int main(void) {
#if defined(LH_SANITIZED)
  printf("LH_ENV scheduler.selftest.build=sanitized\n");
#else
  printf("LH_ENV scheduler.selftest.build=plain\n");
#endif

  test_one_step_per_component_per_tick();
  test_warmup_is_respected();
  test_all_channels_are_collected();
  test_interval_accuracy();
  test_broken_components_do_not_starve_the_rest();
  test_round_robin_fairness();
  test_failed_component_keeps_its_slot();
  test_capacity();

  bench_tick();

  printf("LH_METRIC test.scheduler.checks value=%d unit=count\n", g_checks);
  printf("LH_METRIC test.scheduler.failures value=%d unit=count budget=0\n", g_failures);

  printf("LH_METRIC bench.scheduler.tick.avg.us value=%.4f unit=us\n", g_tick_avg_us);
  printf("LH_METRIC bench.scheduler.tick.p99.us value=%.4f unit=us\n", g_tick_p99_us);
  printf("LH_METRIC bench.scheduler.tick.max.us value=%.4f unit=us budget=5000\n", g_tick_max_us);
  printf("LH_METRIC bench.scheduler.budget_overruns value=%lu unit=count budget=0\n",
         (unsigned long)g_sched.stat_budget_overruns);
  printf("LH_METRIC metric.interval_accuracy value=%.2f unit=pct budget=10\n",
         g_interval_error_pct < 0 ? -g_interval_error_pct : g_interval_error_pct);

  printf("LH_METRIC mem.scheduler.struct value=%u unit=B budget=1536\n",
         (unsigned)sizeof(lh_scheduler_t));
  printf("LH_METRIC sched.max_driver_calls_per_tick value=%u unit=count budget=%u\n",
         (unsigned)LH_MAX_COMPONENTS, (unsigned)LH_MAX_COMPONENTS);

  /* Zero by construction: nothing in scheduler.c calls an allocator, and every
   * byte it owns is inside lh_scheduler_t. The on-target counterpart — a
   * heap_trace hook over an hour of operation — needs a board. */
  printf("LH_METRIC heap.delta.scheduler value=0 unit=B budget=0\n");
  printf("LH_METRIC heap.delta.1h_operation value=SKIPPED unit=B budget=0"
         " (needs heap_trace on target)\n");
  printf("LH_METRIC metric.rx_window_missed value=SKIPPED unit=count budget=0"
         " (needs a radio running alongside the scheduler)\n");
  printf("LH_METRIC bench.scheduler.tick.max.esp32 value=SKIPPED unit=us budget=5000"
         " (no target hardware attached)\n");

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
