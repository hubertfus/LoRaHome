/*
 * Native correctness and benchmark harness for the I2C scanner (T3.2).
 *
 * Every interesting case here is a broken bus, and every one of them is
 * simulated. That is not a compromise: shorting SDA to ground on a real board
 * proves the scanner survives *that* short, once, on the day somebody bothers.
 * A modelled bus proves it survives a device stuck low, a line held low, an
 * arbitration loss, a probe that ignores its own timeout, and seventeen devices
 * where eight were expected — every run, on every commit, including the ones
 * nobody would think to try with a wire.
 *
 * The one thing simulation cannot do is tell you what a real probe costs, so no
 * number here is passed off as one. Durations below come from a model whose
 * per-address costs are stated in the code; the on-target measurements they
 * stand in for are reported SKIPPED with their budgets attached.
 *
 * Driven by tools/run-native.mjs. Output is LH_METRIC lines per roadmap §0.4.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/i2c_scan.h"

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

/* ------------------------------------------------------------------------- */
/* The modelled bus                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Per-address costs, in microseconds.
 *
 * An address phase is nine bits. At the 400 kHz the node runs its bus, that is
 * about 23 us of signalling, and the ESP32's driver overhead dominates it — 50
 * us is the round number in the right place. These figures decide only what the
 * *model* says a scan costs; they are stated here rather than buried so that a
 * duration derived from them is obviously derived.
 */
#define COST_ACK_US 50
#define COST_NACK_US 50
#define COST_TIMEOUT_US ((int64_t)LH_I2C_PROBE_TIMEOUT_MS * 1000)

typedef enum {
  BUS_EMPTY,        /* nothing on it                                        */
  BUS_POPULATED,    /* a BME680 and an expander                             */
  BUS_ONE_STUCK,    /* one device holds the line; everything else is normal */
  BUS_SDA_SHORTED,  /* the line is low: nothing answers, nothing NAKs       */
  BUS_INTERMITTENT, /* timeouts scattered, never eight in a row             */
  BUS_CONTENDED,    /* arbitration losses, not timeouts                     */
  BUS_CROWDED,      /* more devices than the result can hold                */
  BUS_SLOW_PROBE,   /* a probe that ignores its own timeout                 */
  BUS_RUNAWAY,      /* every probe takes 20 ms — the whole budget goes      */
} bus_model_t;

typedef struct {
  bus_model_t model;
  int64_t clock_us;
  uint32_t probe_calls;
} mock_bus_t;

static int64_t mock_now(void *user) { return ((mock_bus_t *)user)->clock_us; }

/** True for the two addresses a populated bench bus actually has on it. */
static bool is_populated_device(uint8_t addr) { return addr == 0x76 || addr == 0x20; }

static lh_i2c_probe_r mock_probe(uint8_t addr, void *user) {
  mock_bus_t *bus = (mock_bus_t *)user;
  bus->probe_calls++;

  lh_i2c_probe_r result = LH_I2C_NACK;
  int64_t cost = COST_NACK_US;

  switch (bus->model) {
    case BUS_EMPTY:
      break;

    case BUS_POPULATED:
      if (is_populated_device(addr)) {
        result = LH_I2C_ACK;
        cost = COST_ACK_US;
      }
      break;

    case BUS_ONE_STUCK:
      if (addr == 0x50) {
        result = LH_I2C_TIMEOUT;
        cost = COST_TIMEOUT_US;
      } else if (is_populated_device(addr)) {
        result = LH_I2C_ACK;
        cost = COST_ACK_US;
      }
      break;

    case BUS_SDA_SHORTED:
      result = LH_I2C_TIMEOUT;
      cost = COST_TIMEOUT_US;
      break;

    case BUS_INTERMITTENT:
      /* Seven timeouts, then something that is not one, repeatedly. The point
       * is that the wedged detector counts consecutively: a bus this bad is
       * still a bus, and abandoning the scan would hide whatever is on it. */
      if ((addr % 8u) != 0u) {
        result = LH_I2C_TIMEOUT;
        cost = COST_TIMEOUT_US;
      }
      break;

    case BUS_CONTENDED:
      result = LH_I2C_BUS_ERROR;
      break;

    case BUS_CROWDED:
      result = LH_I2C_ACK;
      cost = COST_ACK_US;
      break;

    case BUS_SLOW_PROBE:
      /* Answers correctly, just far too slowly — an ESP32 Wire with no timeout
       * configured. Nothing about the result is wrong; only the duration is. */
      cost = COST_TIMEOUT_US * 3;
      break;

    case BUS_RUNAWAY:
      cost = 20000;
      break;

    default:
      break;
  }

  bus->clock_us += cost;
  return result;
}

static lh_i2c_bus_t bus_for(mock_bus_t *state, bus_model_t model) {
  state->model = model;
  state->clock_us = 1000000; /* not zero, so a missing start_us shows up */
  state->probe_calls = 0;

  lh_i2c_bus_t bus;
  bus.probe = mock_probe;
  bus.now_us = mock_now;
  bus.user = state;
  return bus;
}

/* ------------------------------------------------------------------------- */
/* Correctness                                                               */
/* ------------------------------------------------------------------------- */

static uint32_t g_empty_scan_ms = 0;
static uint32_t g_wedged_scan_ms = 0;
static uint32_t g_worst_scan_ms = 0;

/** An empty bus: 112 NAKs, nothing found, no hang, comfortably inside budget. */
static void test_empty_bus(void) {
  mock_bus_t state;
  lh_i2c_bus_t bus = bus_for(&state, BUS_EMPTY);
  lh_i2c_scan_result_t result;

  CHECK(lh_i2c_scan(&bus, &result), "an empty bus should scan to completion");
  CHECK(result.found_count == 0, "nothing should be found, got %u", (unsigned)result.found_count);
  CHECK(result.probed == LH_I2C_ADDR_COUNT, "all %u addresses should be probed, %u were",
        (unsigned)LH_I2C_ADDR_COUNT, (unsigned)result.probed);
  CHECK(state.probe_calls == LH_I2C_ADDR_COUNT, "the bus should be addressed exactly once each");
  CHECK(!result.wedged && !result.over_budget, "an empty bus is not a fault");
  CHECK(result.timeouts == 0 && result.bus_errors == 0, "and produces no errors");

  g_empty_scan_ms = lh_i2c_scan_duration_ms(&result);
  CHECK(g_empty_scan_ms < 300, "the roadmap wants an empty bus under 300 ms, model says %lu",
        (unsigned long)g_empty_scan_ms);
}

/** The addressable range is exactly 0x08..0x77 — no reserved addresses touched. */
static void test_address_range(void) {
  mock_bus_t state;
  lh_i2c_bus_t bus = bus_for(&state, BUS_CROWDED);
  lh_i2c_scan_result_t result;

  lh_i2c_scan(&bus, &result);
  CHECK(result.found[0] == LH_I2C_ADDR_FIRST, "the first address should be 0x08, is 0x%02X",
        (unsigned)result.found[0]);
  CHECK(result.probed == LH_I2C_ADDR_COUNT, "112 addresses, not 128");

  /* Reserved ranges are not merely unlisted; they are never addressed. Putting
   * a general-call or a 10-bit prefix on the wire is how a scan wakes something
   * that was minding its own business. */
  CHECK(LH_I2C_ADDR_FIRST == 0x08, "0x00..0x07 are reserved");
  CHECK(LH_I2C_ADDR_LAST == 0x77, "0x78..0x7F are reserved");
}

static void test_populated_bus(void) {
  mock_bus_t state;
  lh_i2c_bus_t bus = bus_for(&state, BUS_POPULATED);
  lh_i2c_scan_result_t result;

  CHECK(lh_i2c_scan(&bus, &result), "a healthy bus should scan to completion");
  CHECK(result.found_count == 2, "two devices expected, found %u", (unsigned)result.found_count);
  CHECK(lh_i2c_scan_contains(&result, 0x76), "the BME680 at 0x76 should be found");
  CHECK(lh_i2c_scan_contains(&result, 0x20), "the expander at 0x20 should be found");
  CHECK(!lh_i2c_scan_contains(&result, 0x77), "0x77 is empty on this bus");
  CHECK(result.found[0] == 0x20 && result.found[1] == 0x76, "results should be in address order");
}

/**
 * The roadmap's headline requirement: one stuck address must not block the scan.
 *
 * A single device holding the line costs one timeout and nothing else. The
 * devices after it are still found, which is the whole point — a node with one
 * dead sensor must still be able to report the others.
 */
static void test_one_stuck_device_does_not_block(void) {
  mock_bus_t state;
  lh_i2c_bus_t bus = bus_for(&state, BUS_ONE_STUCK);
  lh_i2c_scan_result_t result;

  CHECK(lh_i2c_scan(&bus, &result), "one stuck device must not abort the scan");
  CHECK(result.probed == LH_I2C_ADDR_COUNT, "every address should still be probed, %u were",
        (unsigned)result.probed);
  CHECK(result.timeouts == 1, "exactly one timeout expected, counted %u",
        (unsigned)result.timeouts);
  CHECK(!result.wedged, "one timeout is a device fault, not a bus fault");
  CHECK(result.found_count == 2, "the healthy devices are still found, got %u",
        (unsigned)result.found_count);
  /* 0x76 sits after the stuck address at 0x50: the scan got past it. */
  CHECK(lh_i2c_scan_contains(&result, 0x76), "a device beyond the stuck one is still found");
}

/**
 * SDA shorted to ground — the test the roadmap says everyone forgets.
 *
 * Every address times out. The scan must end quickly, say why, and leave a
 * system that is still running. What it must not do is spend 448 ms not
 * servicing the radio in order to learn what the first eight addresses already
 * said.
 */
static void test_shorted_bus_recovery(void) {
  mock_bus_t state;
  lh_i2c_bus_t bus = bus_for(&state, BUS_SDA_SHORTED);
  lh_i2c_scan_result_t result;

  CHECK(!lh_i2c_scan(&bus, &result), "a wedged bus should report failure");
  CHECK(result.wedged, "and should say the bus is wedged, not merely that it failed");
  CHECK(!result.over_budget, "it aborted on the signature, not on the clock");
  CHECK(result.probed == LH_I2C_WEDGED_BUS_TIMEOUTS, "it should stop after %u addresses, took %u",
        (unsigned)LH_I2C_WEDGED_BUS_TIMEOUTS, (unsigned)result.probed);
  CHECK(result.found_count == 0, "nothing answers a shorted bus");

  g_wedged_scan_ms = lh_i2c_scan_duration_ms(&result);
  CHECK(g_wedged_scan_ms <= LH_I2C_WEDGED_BUS_TIMEOUTS * LH_I2C_PROBE_TIMEOUT_MS,
        "the wedged case is bounded at %u ms, model says %lu",
        (unsigned)(LH_I2C_WEDGED_BUS_TIMEOUTS * LH_I2C_PROBE_TIMEOUT_MS),
        (unsigned long)g_wedged_scan_ms);

  /* And the system is still here: a second scan on a repaired bus works. */
  lh_i2c_bus_t repaired = bus_for(&state, BUS_POPULATED);
  lh_i2c_scan_result_t after;
  CHECK(lh_i2c_scan(&repaired, &after), "the scanner survives a wedged bus");
  CHECK(after.found_count == 2, "and finds the devices once the short is gone");
}

/**
 * Timeouts scattered but never eight consecutive: the scan must finish.
 *
 * This is the case that decides whether the abort rule is written on
 * consecutive timeouts or on a total. A total would abandon this bus, and this
 * bus has working devices on it — a marginal pull-up on a long cable looks
 * exactly like this and is somebody's actual installation.
 */
static void test_intermittent_timeouts_do_not_abort(void) {
  mock_bus_t state;
  lh_i2c_bus_t bus = bus_for(&state, BUS_INTERMITTENT);
  lh_i2c_scan_result_t result;

  CHECK(lh_i2c_scan(&bus, &result), "scattered timeouts must not abort the scan");
  CHECK(!result.wedged, "this bus is bad, not wedged");
  CHECK(result.probed == LH_I2C_ADDR_COUNT, "all addresses probed, %u were",
        (unsigned)result.probed);
  CHECK(result.timeouts > 0, "and the timeouts are still counted");

  g_worst_scan_ms = lh_i2c_scan_duration_ms(&result);
}

/** An arbitration loss is not a stuck line, and must not be counted as one. */
static void test_bus_errors_are_not_wedging(void) {
  mock_bus_t state;
  lh_i2c_bus_t bus = bus_for(&state, BUS_CONTENDED);
  lh_i2c_scan_result_t result;

  CHECK(lh_i2c_scan(&bus, &result), "arbitration losses should not abort the scan");
  CHECK(!result.wedged, "somebody else driving the bus is a different fault from a stuck line");
  CHECK(result.bus_errors > 0, "and it is counted separately, got %u",
        (unsigned)result.bus_errors);
  CHECK(result.timeouts == 0, "with no timeouts attributed to it");
}

/** More devices than the result can hold: counted, never silently dropped. */
static void test_crowded_bus_overflow(void) {
  mock_bus_t state;
  lh_i2c_bus_t bus = bus_for(&state, BUS_CROWDED);
  lh_i2c_scan_result_t result;

  CHECK(lh_i2c_scan(&bus, &result), "a crowded bus still scans");
  CHECK(result.found_count == LH_I2C_SCAN_MAX_FOUND, "the list fills to %u, has %u",
        (unsigned)LH_I2C_SCAN_MAX_FOUND, (unsigned)result.found_count);
  CHECK(result.overflow > 0, "and the rest are counted, not hidden");
}

/** A probe that ignores its own timeout is caught by measurement, not by trust. */
static void test_slow_probe_is_detected(void) {
  mock_bus_t state;
  lh_i2c_bus_t bus = bus_for(&state, BUS_SLOW_PROBE);
  lh_i2c_scan_result_t result;

  lh_i2c_scan(&bus, &result);
  CHECK(result.slow_probes > 0, "a probe overrunning its timeout should be counted");
  CHECK(result.found_count == 0, "the answers themselves were fine — only the duration was not");
}

/**
 * The backstop: a platform whose probe honours nothing at all.
 *
 * Every guarantee this file makes about duration rests on a function it does
 * not own. Without a total deadline, the budget in the header is a comment.
 */
static void test_total_budget_backstop(void) {
  mock_bus_t state;
  lh_i2c_bus_t bus = bus_for(&state, BUS_RUNAWAY);
  lh_i2c_scan_result_t result;

  CHECK(!lh_i2c_scan(&bus, &result), "a runaway probe should abort the scan");
  CHECK(result.over_budget, "and should say it was the clock, not the bus");
  CHECK(!result.wedged, "nothing timed out; the probes answered, slowly");
  CHECK(result.probed < LH_I2C_ADDR_COUNT, "it stopped early, at %u", (unsigned)result.probed);
  CHECK(lh_i2c_scan_duration_ms(&result) <= LH_I2C_SCAN_BUDGET_MS + 20,
        "and stopped within one probe of the budget, at %lu ms",
        (unsigned long)lh_i2c_scan_duration_ms(&result));
}

/** A caller that passes nothing gets an answer, not a crash. */
static void test_defensive_arguments(void) {
  lh_i2c_scan_result_t result;
  CHECK(!lh_i2c_scan(NULL, &result), "a NULL bus is refused");
  CHECK(!lh_i2c_scan_contains(NULL, 0x76), "a NULL result contains nothing");
  CHECK(lh_i2c_scan_duration_ms(NULL) == 0, "and has no duration");

  lh_i2c_bus_t empty;
  memset(&empty, 0, sizeof empty);
  CHECK(!lh_i2c_scan(&empty, &result), "a bus with no probe function is refused");
  CHECK(result.bus_errors == 1, "and says so");

  mock_bus_t state;
  lh_i2c_bus_t bus = bus_for(&state, BUS_EMPTY);
  CHECK(!lh_i2c_scan(&bus, NULL), "a NULL result is refused");
}

/* ------------------------------------------------------------------------- */
/* Benchmark                                                                 */
/* ------------------------------------------------------------------------- */

#define ROUNDS 60
#define BATCH 200

static volatile uint32_t g_sink = 0;

/** A probe that costs nothing, so what is left is the scanner's own overhead. */
static lh_i2c_probe_r free_probe(uint8_t addr, void *user) {
  (void)user;
  return (addr == 0x76) ? LH_I2C_ACK : LH_I2C_NACK;
}

static int64_t free_clock(void *user) { return ++(*(int64_t *)user); }

/**
 * What the scan costs beyond the bus transactions themselves.
 *
 * The useful number, because the transaction cost is the platform's and this is
 * the only part the code here decides. If it is not negligible against a 50 us
 * address phase, the loop is doing something it should not be.
 */
static double bench_scan_overhead_us_per_addr(void) {
  double samples[ROUNDS];
  int64_t clock = 0;
  lh_i2c_bus_t bus;
  lh_i2c_scan_result_t result;

  bus.probe = free_probe;
  bus.now_us = free_clock;
  bus.user = &clock;

  for (int i = 0; i < BATCH; i++) {
    lh_i2c_scan(&bus, &result);
    g_sink += result.found_count;
  }

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      lh_i2c_scan(&bus, &result);
      g_sink += result.found_count;
    }
    samples[round] = (now_seconds() - started) * 1e6 / (BATCH * LH_I2C_ADDR_COUNT);
  }

  qsort(samples, ROUNDS, sizeof(double), compare_double);
  return samples[ROUNDS / 2];
}

/* ------------------------------------------------------------------------- */

int main(void) {
#if defined(LH_SANITIZED)
  printf("LH_ENV i2c_scan.selftest.build=sanitized\n");
#else
  printf("LH_ENV i2c_scan.selftest.build=plain\n");
#endif

  test_empty_bus();
  test_address_range();
  test_populated_bus();
  test_one_stuck_device_does_not_block();
  test_shorted_bus_recovery();
  test_intermittent_timeouts_do_not_abort();
  test_bus_errors_are_not_wedging();
  test_crowded_bus_overflow();
  test_slow_probe_is_detected();
  test_total_budget_backstop();
  test_defensive_arguments();

  printf("LH_METRIC test.i2c_scan.checks value=%d unit=count\n", g_checks);
  printf("LH_METRIC test.i2c_scan.failures value=%d unit=count budget=0\n", g_failures);
  printf("LH_METRIC test.shorted_bus_recovery value=%d unit=count budget=1\n",
         g_failures == 0 ? 1 : 0);

  printf("LH_METRIC mem.i2c_scan.result value=%u unit=B budget=64\n",
         (unsigned)sizeof(lh_i2c_scan_result_t));
  printf("LH_METRIC i2c.scan.addresses value=%u unit=count budget=112\n",
         (unsigned)LH_I2C_ADDR_COUNT);

  /*
   * Analytic bounds, not measurements. These follow from the per-address
   * timeout and the abort rule alone, so they hold on any platform whose probe
   * honours LH_I2C_PROBE_TIMEOUT_MS — which is the property the slow-probe
   * counter and the total deadline exist to check. Emitted with budgets because
   * they are the part of the roadmap's 500 ms figure that can be guaranteed
   * without a board on the desk.
   */
  printf("LH_METRIC i2c.scan.bound.worst_ms value=%u unit=ms budget=%u\n",
         (unsigned)(LH_I2C_ADDR_COUNT * LH_I2C_PROBE_TIMEOUT_MS), (unsigned)LH_I2C_SCAN_BUDGET_MS);
  printf("LH_METRIC i2c.scan.bound.wedged_ms value=%u unit=ms budget=%u\n",
         (unsigned)(LH_I2C_WEDGED_BUS_TIMEOUTS * LH_I2C_PROBE_TIMEOUT_MS),
         (unsigned)LH_I2C_SCAN_BUDGET_MS);
  printf("LH_METRIC i2c.scan.probe_timeout_ms value=%u unit=ms budget=5\n",
         (unsigned)LH_I2C_PROBE_TIMEOUT_MS);

  /* Modelled durations. The per-address costs are the ones declared at the top
   * of this file, not anything measured on a wire. */
  printf("LH_METRIC i2c.scan.model.empty_ms value=%lu unit=ms\n", (unsigned long)g_empty_scan_ms);
  printf("LH_METRIC i2c.scan.model.wedged_ms value=%lu unit=ms\n", (unsigned long)g_wedged_scan_ms);
  printf("LH_METRIC i2c.scan.model.intermittent_ms value=%lu unit=ms\n",
         (unsigned long)g_worst_scan_ms);

  printf("LH_METRIC bench.i2c.scan.overhead.native.p50 value=%.4f unit=us\n",
         bench_scan_overhead_us_per_addr());

  printf("LH_METRIC bench.i2c.scan.full.esp32 value=SKIPPED unit=ms budget=500"
         " (needs a board and a real bus)\n");
  printf("LH_METRIC bench.i2c.scan.per_addr.esp32 value=SKIPPED unit=us budget=5000"
         " (needs a board and a real bus)\n");
  printf("LH_METRIC stack.hwm.scan_task value=SKIPPED unit=B budget=2048"
         " (no target hardware attached)\n");

  if (g_sink == 0xFFFFFFFFu) printf("unreachable %u\n", g_sink);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
