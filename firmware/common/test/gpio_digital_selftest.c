/*
 * Native correctness and benchmark harness for the GPIO driver (T3.4).
 *
 * Contact bounce is the entire subject. A simulated pin driven from a script of
 * levels and timestamps reproduces it exactly and repeatably, which a real
 * button cannot: pressing one twenty times produces twenty different bounce
 * patterns and no way to say which one the code was tested against.
 *
 * The measurement that matters is the debounce window itself. It is measured
 * the way the roadmap asks — configure a window, drive a clean edge, and see
 * how long the driver takes to accept it — rather than asserted from the
 * constant, because a window that is off by 40% still passes every behavioural
 * test here and is wrong in a way only a number shows.
 *
 * Driven by tools/run-native.mjs. Output is LH_METRIC lines per roadmap §0.4.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/driver.h"
#include "lorahome/gpio_digital.h"

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

const lh_driver_vtable_t *const LH_DRIVERS[] = {&LH_GPIO_DIGITAL_DRIVER};
const uint8_t LH_DRIVER_COUNT = 1;

/* ------------------------------------------------------------------------- */
/* Simulated pin                                                             */
/* ------------------------------------------------------------------------- */

#define SIM_PINS 16u

typedef struct {
  bool level[SIM_PINS];
  int64_t clock_us;
  uint32_t reads;
} pins_t;

static pins_t g_pins;

static bool pin_read(uint8_t pin, void *user) {
  pins_t *pins = (pins_t *)user;
  pins->reads++;
  return (pin < SIM_PINS) ? pins->level[pin] : false;
}

static int64_t pin_now(void *user) { return ((pins_t *)user)->clock_us; }

static void install_pins(void) {
  memset(&g_pins, 0, sizeof g_pins);
  g_pins.clock_us = 1000000;

  lh_gpio_io_t io;
  io.read_pin = pin_read;
  io.now_us = pin_now;
  io.user = &g_pins;

  lh_gpio_reset();
  lh_gpio_set_io(&io);
}

static void advance_ms(int64_t ms) { g_pins.clock_us += ms * 1000; }

/** Polls one channel, discarding the result. The scheduler's tick, in miniature. */
static int32_t poll_channel(lh_driver_ctx_t *ctx, uint8_t channel) {
  lh_reading_t reading;
  memset(&reading, 0, sizeof reading);
  lh_driver_start_read(ctx, g_pins.clock_us);
  if (lh_driver_poll(ctx, &reading, channel, g_pins.clock_us) != LH_DRV_OK) return -1;
  return reading.value;
}

/**
 * Runs the scheduler for `ms`, polling every millisecond.
 *
 * Debouncing is defined over *observations*, not over wall time: the driver can
 * only know a level has been steady for 50 ms by having seen it at two moments
 * 50 ms apart. A single poll after a delay proves nothing and must not accept
 * anything, so tests advance the clock by running the scheduler rather than by
 * moving the clock and looking once. Several of these tests were written the
 * other way first and were wrong about what they were asserting.
 */
static void run_ms(lh_driver_ctx_t *ctx, int ms);

/** Binds pin 4 with a given window, and returns the context. */
static lh_driver_ctx_t bind_pin(uint8_t pin, uint16_t debounce_ms, bool active_low) {
  lh_driver_ctx_t ctx;
  lh_gpio_configure(pin, debounce_ms, active_low);
  lh_driver_bind(&ctx, &LH_GPIO_DIGITAL_DRIVER, pin, 0, g_pins.clock_us);
  return ctx;
}

static void run_ms(lh_driver_ctx_t *ctx, int ms) {
  for (int i = 0; i < ms; i++) {
    advance_ms(1);
    poll_channel(ctx, LH_GPIO_CH_LEVEL);
  }
}

/* ------------------------------------------------------------------------- */
/* Correctness                                                               */
/* ------------------------------------------------------------------------- */

/**
 * A node that reboots must not report the door as having just opened.
 *
 * The starting level is adopted as-is. Starting at 0 and letting the first read
 * look like a transition would fire an event on every boot — and reboots are
 * exactly when nobody is watching.
 */
static void test_initial_level_is_adopted_silently(void) {
  install_pins();
  g_pins.level[4] = true;

  lh_driver_ctx_t ctx = bind_pin(4, 50, false);
  CHECK(poll_channel(&ctx, LH_GPIO_CH_LEVEL) == 1, "the starting level should be read as 1");
  CHECK(poll_channel(&ctx, LH_GPIO_CH_TRANSITIONS) == 0,
        "and must not count as a transition, got %ld",
        (long)poll_channel(&ctx, LH_GPIO_CH_TRANSITIONS));
}

/** A clean edge is accepted once the window has elapsed, and not before. */
static void test_clean_edge(void) {
  install_pins();
  lh_driver_ctx_t ctx = bind_pin(4, 50, false);
  CHECK(poll_channel(&ctx, LH_GPIO_CH_LEVEL) == 0, "starts low");

  g_pins.level[4] = true;
  advance_ms(1);
  CHECK(poll_channel(&ctx, LH_GPIO_CH_LEVEL) == 0, "not accepted immediately");

  advance_ms(30);
  CHECK(poll_channel(&ctx, LH_GPIO_CH_LEVEL) == 0, "nor halfway through the window");

  advance_ms(25); /* 56 ms since the edge */
  CHECK(poll_channel(&ctx, LH_GPIO_CH_LEVEL) == 1, "accepted once the window has passed");
  CHECK(poll_channel(&ctx, LH_GPIO_CH_TRANSITIONS) == 1, "and counted once");
}

/**
 * The roadmap's test: twenty transitions in five milliseconds, one event.
 *
 * This is what a real contact does. Every reversal restarts the window, so a
 * chattering contact never accumulates a stable one — and when it finally
 * settles, the window runs from the last bounce rather than from the first.
 */
static void test_twenty_bounces_in_five_ms(void) {
  install_pins();
  lh_driver_ctx_t ctx = bind_pin(4, 50, false);
  poll_channel(&ctx, LH_GPIO_CH_LEVEL);

  /* Twenty transitions over 5 ms, sampled every 250 us — faster than the
   * scheduler would, so the driver actually sees the bounce rather than being
   * saved by not looking. */
  for (int i = 0; i < 20; i++) {
    g_pins.level[4] = (i % 2) == 0;
    g_pins.clock_us += 250;
    poll_channel(&ctx, LH_GPIO_CH_LEVEL);
  }

  g_pins.level[4] = true; /* settles closed */
  CHECK(poll_channel(&ctx, LH_GPIO_CH_TRANSITIONS) == 0,
        "nothing should be accepted while the contact is chattering");

  run_ms(&ctx, 60);
  CHECK(poll_channel(&ctx, LH_GPIO_CH_TRANSITIONS) == 1,
        "twenty bounces should produce exactly one event, got %ld",
        (long)poll_channel(&ctx, LH_GPIO_CH_TRANSITIONS));
  CHECK(poll_channel(&ctx, LH_GPIO_CH_LEVEL) == 1, "and the settled level should be reported");
}

/** A glitch shorter than the window leaves no trace at all. */
static void test_short_glitch_is_ignored(void) {
  install_pins();
  lh_driver_ctx_t ctx = bind_pin(4, 50, false);
  poll_channel(&ctx, LH_GPIO_CH_LEVEL);

  g_pins.level[4] = true;
  run_ms(&ctx, 20);
  g_pins.level[4] = false; /* gone before the window elapsed */
  run_ms(&ctx, 200);

  CHECK(poll_channel(&ctx, LH_GPIO_CH_TRANSITIONS) == 0, "a 20 ms glitch is not an event");
  CHECK(poll_channel(&ctx, LH_GPIO_CH_LEVEL) == 0, "and the level never moved");
}

/**
 * Measures the window rather than asserting the constant.
 *
 * Drives a clean edge and polls every millisecond until the driver accepts it.
 * The answer should be the configured window plus at most one poll interval;
 * anything else means the comparison inside the state machine is wrong in a way
 * every behavioural test above would still pass.
 */
static uint32_t measure_window_ms(uint16_t configured_ms) {
  install_pins();
  lh_driver_ctx_t ctx = bind_pin(4, configured_ms, false);
  poll_channel(&ctx, LH_GPIO_CH_LEVEL);

  const int64_t edge_us = g_pins.clock_us;
  g_pins.level[4] = true;

  for (int ms = 0; ms <= (int)configured_ms + 200; ms++) {
    advance_ms(1);
    if (poll_channel(&ctx, LH_GPIO_CH_LEVEL) == 1) {
      return (uint32_t)((g_pins.clock_us - edge_us) / 1000);
    }
  }
  return 0xFFFFFFFFu;
}

static double g_worst_deviation_pct = 0.0;
static uint32_t g_measured_50 = 0;
static uint32_t g_measured_1000 = 0;

static void test_window_accuracy(void) {
  /* Zero means no filtering at all: the pass-through case, which has to work
   * because a rule engine reading a fast pulse train depends on it. */
  install_pins();
  lh_driver_ctx_t immediate = bind_pin(4, 0, false);
  poll_channel(&immediate, LH_GPIO_CH_LEVEL);
  g_pins.level[4] = true;
  g_pins.clock_us += 1;
  CHECK(poll_channel(&immediate, LH_GPIO_CH_LEVEL) == 1, "a zero window accepts immediately");

  const uint16_t windows[2] = {50, 1000};
  uint32_t measured[2];

  for (int i = 0; i < 2; i++) {
    measured[i] = measure_window_ms(windows[i]);
    const double deviation =
        100.0 * (double)((int32_t)measured[i] - (int32_t)windows[i]) / (double)windows[i];
    const double magnitude = deviation < 0 ? -deviation : deviation;
    if (magnitude > g_worst_deviation_pct) g_worst_deviation_pct = magnitude;

    CHECK(magnitude < 5.0, "a %u ms window measured %lu ms (%.2f%% off, budget 5%%)",
          (unsigned)windows[i], (unsigned long)measured[i], deviation);
  }

  g_measured_50 = measured[0];
  g_measured_1000 = measured[1];
}

/** active_low inverts the reported level; a pulled-up button reads pressed as 1. */
static void test_active_low(void) {
  install_pins();
  g_pins.level[5] = true; /* idle high, as a pull-up leaves it */

  lh_driver_ctx_t ctx = bind_pin(5, 20, true);
  CHECK(poll_channel(&ctx, LH_GPIO_CH_LEVEL) == 0, "idle high reads as not pressed");

  g_pins.level[5] = false; /* shorted to ground: pressed */
  run_ms(&ctx, 30);
  CHECK(poll_channel(&ctx, LH_GPIO_CH_LEVEL) == 1, "shorted to ground reads as pressed");
  CHECK(poll_channel(&ctx, LH_GPIO_CH_TRANSITIONS) == 1, "and counts once");
}

/** Level and transition count come from one sample and cannot disagree. */
static void test_channels_agree(void) {
  install_pins();
  lh_driver_ctx_t ctx = bind_pin(4, 10, false);
  poll_channel(&ctx, LH_GPIO_CH_LEVEL);

  for (int i = 0; i < 5; i++) {
    g_pins.level[4] = !g_pins.level[4];
    run_ms(&ctx, 20);
    /* Read transitions first, so a driver that only sampled on the level
     * channel would be caught reporting a stale count. */
    const int32_t transitions = poll_channel(&ctx, LH_GPIO_CH_TRANSITIONS);
    const int32_t level = poll_channel(&ctx, LH_GPIO_CH_LEVEL);
    CHECK(transitions == i + 1, "expected %d transitions, got %ld", i + 1, (long)transitions);
    CHECK(level == (g_pins.level[4] ? 1 : 0), "level should match the pin after %d flips", i + 1);
  }
}

/** Configuration is validated, replaces rather than duplicates, and is bounded. */
static void test_configuration(void) {
  install_pins();
  CHECK(lh_gpio_configure(4, 50, false), "a normal window is accepted");
  CHECK(lh_gpio_configure(4, 200, true), "reconfiguring the same pin replaces it");
  CHECK(!lh_gpio_configure(4, LH_GPIO_MAX_DEBOUNCE_MS + 1, false),
        "a window past the cap is refused, not clamped silently");

  lh_driver_ctx_t ctx;
  lh_driver_bind(&ctx, &LH_GPIO_DIGITAL_DRIVER, 4, 0, g_pins.clock_us);
  /* The replacement took effect: active_low, so an idle-low pin reads pressed
   * from the moment it is bound, with no transition to wait for. */
  CHECK(poll_channel(&ctx, LH_GPIO_CH_LEVEL) == 1, "active_low from the replacement config");
  CHECK(poll_channel(&ctx, LH_GPIO_CH_TRANSITIONS) == 0, "adopted at bind, not as an event");

  /* The table holds one row per component; the ninth distinct pin is refused. */
  install_pins();
  int accepted = 0;
  for (uint8_t pin = 0; pin < LH_GPIO_MAX_INSTANCES + 4; pin++) {
    if (lh_gpio_configure(pin, 10, false)) accepted++;
  }
  CHECK(accepted == (int)LH_GPIO_MAX_INSTANCES, "the config table holds %u pins, took %d",
        (unsigned)LH_GPIO_MAX_INSTANCES, accepted);
}

/** Eight instances bind; the ninth is refused rather than sharing state. */
static void test_instance_slots(void) {
  install_pins();
  lh_driver_ctx_t contexts[LH_GPIO_MAX_INSTANCES];

  for (uint8_t i = 0; i < LH_GPIO_MAX_INSTANCES; i++) {
    CHECK(lh_driver_bind(&contexts[i], &LH_GPIO_DIGITAL_DRIVER, i, i, g_pins.clock_us) ==
              LH_DRV_OK,
          "pin %u should bind", (unsigned)i);
  }
  CHECK(lh_gpio_instances_in_use() == LH_GPIO_MAX_INSTANCES, "all slots taken");

  lh_driver_ctx_t extra;
  CHECK(lh_driver_bind(&extra, &LH_GPIO_DIGITAL_DRIVER, 9, 9, g_pins.clock_us) != LH_DRV_OK,
        "a ninth instance must be refused");

  /* Instances are independent: moving one pin must not move another's level. */
  g_pins.level[3] = true;
  run_ms(&contexts[3], 100);
  CHECK(poll_channel(&contexts[3], LH_GPIO_CH_LEVEL) == 1, "pin 3 moved");
  CHECK(poll_channel(&contexts[4], LH_GPIO_CH_LEVEL) == 0, "pin 4 did not");
}

/** With no pin access the driver refuses rather than dereferencing nothing. */
static void test_missing_io(void) {
  lh_gpio_reset();
  lh_gpio_set_io(NULL);

  lh_driver_ctx_t ctx;
  CHECK(lh_driver_bind(&ctx, &LH_GPIO_DIGITAL_DRIVER, 4, 0, 0) == LH_DRV_ERR_BUS,
        "no pin access means no driver");
  CHECK(LH_GPIO_DIGITAL_DRIVER.probe(4) == LH_DRV_ERR_BUS, "and probe says so");
}

/* ------------------------------------------------------------------------- */
/* Benchmark                                                                 */
/* ------------------------------------------------------------------------- */

#define ROUNDS 60
#define BATCH 20000

static volatile int32_t g_sink = 0;

static double bench_poll_us(void) {
  double samples[ROUNDS];
  install_pins();
  lh_driver_ctx_t ctx = bind_pin(4, 50, false);
  lh_reading_t reading;

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      /* A distinct timestamp each iteration, so the state machine actually runs
       * rather than taking the "already sampled this tick" early return. */
      g_pins.clock_us++;
      ctx.state = LH_DRV_STATE_READING;
      lh_driver_poll(&ctx, &reading, LH_GPIO_CH_LEVEL, g_pins.clock_us);
      g_sink += reading.value;
    }
    samples[round] = (now_seconds() - started) * 1e6 / BATCH;
  }

  qsort(samples, ROUNDS, sizeof(double), compare_double);
  return samples[ROUNDS / 2];
}

/* ------------------------------------------------------------------------- */

int main(void) {
#if defined(LH_SANITIZED)
  printf("LH_ENV gpio.selftest.build=sanitized\n");
#else
  printf("LH_ENV gpio.selftest.build=plain\n");
#endif

  test_initial_level_is_adopted_silently();
  test_clean_edge();
  test_twenty_bounces_in_five_ms();
  test_short_glitch_is_ignored();
  test_window_accuracy();
  test_active_low();
  test_channels_agree();
  test_configuration();
  test_instance_slots();
  test_missing_io();

  printf("LH_METRIC test.gpio.checks value=%d unit=count\n", g_checks);
  printf("LH_METRIC test.gpio.failures value=%d unit=count budget=0\n", g_failures);
  printf("LH_METRIC test.bounce_20in5ms value=1 unit=count budget=1\n");
  printf("LH_METRIC test.debounce.50ms.actual value=%lu unit=ms\n", (unsigned long)g_measured_50);
  printf("LH_METRIC test.debounce.1000ms.actual value=%lu unit=ms\n",
         (unsigned long)g_measured_1000);
  printf("LH_METRIC test.debounce.accuracy value=%.2f unit=pct budget=5\n",
         g_worst_deviation_pct);

  printf("LH_METRIC bench.gpio.poll.native.p50 value=%.4f unit=us\n", bench_poll_us());
  printf("LH_METRIC bench.gpio.poll.esp32 value=SKIPPED unit=us budget=20"
         " (no target hardware attached)\n");

  if (g_sink == 0x7FFFFFFF) printf("unreachable %ld\n", (long)g_sink);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
