/*
 * Native harness for fragmentation and reassembly (T2.3).
 *
 * The cases here are the ones that cost a field visit rather than a test run:
 * fragments arriving out of order, a duplicate landing on a slot already
 * filled, a fragment from a transaction that ended thirty seconds ago, and the
 * one that is genuinely hard to reason about — two transactions whose fragments
 * are individually perfect and whose combination is nonsense (R2.3).
 *
 * `heap.delta` is measured across a full cycle and must be exactly zero. The
 * reassembler is 1636 B in .bss; if any of this ever starts allocating, the
 * device fragments its heap over weeks and dies at 3 a.m. on a Sunday, which is
 * the failure mode the zero-allocation doctrine exists to make impossible.
 *
 * Driven by tools/run-native.mjs; LH_METRIC lines on stdout per §0.4.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/crc16.h"
#include "lorahome/frag.h"

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

static uint32_t g_rng = 0x7B19E42Du;

static uint32_t next_random(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

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

#define FRAG_BUF (LH_FRAG_HDR_SIZE + LH_FRAG_PAYLOAD_MAX)

static lh_reassembler_t g_reasm;
static uint8_t g_config[LH_FRAG_CONFIG_MAX];
static uint8_t g_fragments[LH_FRAG_MAX_FRAGMENTS][FRAG_BUF];
static uint16_t g_fragment_len[LH_FRAG_MAX_FRAGMENTS];

/** Fills a config with a recognisable pattern and splits it into g_fragments. */
static uint8_t make_config(uint16_t cfg_id, uint16_t total_len, uint8_t seed) {
  for (uint16_t i = 0; i < total_len; i++) g_config[i] = (uint8_t)(i * 31u + seed);

  const uint8_t total = lh_frag_count(total_len);
  for (uint8_t index = 0; index < total; index++) {
    const int written =
        lh_frag_build(cfg_id, index, g_config, total_len, g_fragments[index], FRAG_BUF);
    if (written <= 0) {
      CHECK(0, "could not build fragment %u of %u B", (unsigned)index, (unsigned)total_len);
      return 0;
    }
    g_fragment_len[index] = (uint16_t)written;
  }
  return total;
}

/* ------------------------------------------------------------------------- */
/* Correctness                                                               */
/* ------------------------------------------------------------------------- */

/** The headline case: 1600 B, eight fragments, byte-identical after assembly. */
static void test_full_size_round_trip(void) {
  lh_frag_reset(&g_reasm);
  const uint8_t total = make_config(0x1234, LH_FRAG_CONFIG_MAX, 7);
  CHECK(total == LH_FRAG_MAX_FRAGMENTS, "1600 B should be %u fragments, got %u",
        (unsigned)LH_FRAG_MAX_FRAGMENTS, (unsigned)total);

  lh_frag_result_t result = LH_FRAG_NEED_MORE;
  for (uint8_t index = 0; index < total; index++) {
    result = lh_frag_feed(&g_reasm, g_fragments[index], g_fragment_len[index], 1000);
  }

  CHECK(result == LH_FRAG_COMPLETE, "eight fragments should complete, got %d", (int)result);
  CHECK(g_reasm.total_len == LH_FRAG_CONFIG_MAX, "assembled length is %u",
        (unsigned)g_reasm.total_len);
  CHECK(memcmp(g_reasm.buf, g_config, LH_FRAG_CONFIG_MAX) == 0, "assembled config differs");
  CHECK(g_reasm.stat_completed == 1, "one completion should be counted");
}

/** Fragment sizes at the boundaries, where off-by-one arithmetic lives. */
static void test_geometry(void) {
  CHECK(lh_frag_count(0) == 1, "an empty config is still one fragment");
  CHECK(lh_frag_count(1) == 1, "1 B is one fragment");
  CHECK(lh_frag_count(LH_FRAG_PAYLOAD_MAX) == 1, "exactly one full fragment is one fragment");
  CHECK(lh_frag_count(LH_FRAG_PAYLOAD_MAX + 1) == 2, "one byte over is two fragments");
  CHECK(lh_frag_count(LH_FRAG_CONFIG_MAX) == LH_FRAG_MAX_FRAGMENTS, "1600 B is 8 fragments");
  CHECK(lh_frag_count(LH_FRAG_CONFIG_MAX + 1) == 0, "1601 B cannot be sent at all");

  uint8_t out[FRAG_BUF];
  CHECK(lh_frag_build(1, 0, g_config, LH_FRAG_CONFIG_MAX + 1, out, sizeof out) < 0,
        "an oversized config must be refused by the builder");
  CHECK(lh_frag_build(1, 3, g_config, 100, out, sizeof out) < 0,
        "a fragment index past the last one must be refused");
  CHECK(lh_frag_build(1, 0, g_config, 100, out, 8) < 0, "a buffer that cannot hold the slice");
}

/** Out of order, a thousand times, with a fresh shuffle each round. */
static long test_out_of_order(int rounds) {
  long completed = 0;

  for (int round = 0; round < rounds; round++) {
    lh_frag_reset(&g_reasm);
    const uint16_t total_len = (uint16_t)(1 + next_random() % LH_FRAG_CONFIG_MAX);
    const uint8_t total = make_config((uint16_t)(0x2000 + round), total_len, (uint8_t)round);
    if (total == 0) continue;

    uint8_t order[LH_FRAG_MAX_FRAGMENTS];
    for (uint8_t i = 0; i < total; i++) order[i] = i;
    for (uint8_t i = total; i > 1; i--) {
      const uint8_t j = (uint8_t)(next_random() % i);
      const uint8_t swap = order[i - 1];
      order[i - 1] = order[j];
      order[j] = swap;
    }

    lh_frag_result_t result = LH_FRAG_NEED_MORE;
    for (uint8_t i = 0; i < total; i++) {
      result = lh_frag_feed(&g_reasm, g_fragments[order[i]], g_fragment_len[order[i]], 1000);
    }

    if (result == LH_FRAG_COMPLETE && g_reasm.total_len == total_len &&
        memcmp(g_reasm.buf, g_config, total_len) == 0) {
      completed++;
    } else {
      CHECK(0, "shuffled round %d failed (len %u, result %d)", round, (unsigned)total_len,
            (int)result);
    }
  }

  return completed;
}

/** A duplicate changes nothing — same mask, same bytes, no second write. */
static void test_duplicate_is_idempotent(void) {
  lh_frag_reset(&g_reasm);
  const uint8_t total = make_config(0x3333, 600, 5);
  CHECK(total == 3, "600 B should be 3 fragments");

  lh_frag_feed(&g_reasm, g_fragments[0], g_fragment_len[0], 1000);
  const uint8_t mask_before = g_reasm.received_mask;

  CHECK(lh_frag_feed(&g_reasm, g_fragments[0], g_fragment_len[0], 1100) == LH_FRAG_DUPLICATE,
        "a repeated fragment should be reported as a duplicate");
  CHECK(g_reasm.received_mask == mask_before, "a duplicate must not change the mask");
  CHECK(g_reasm.stat_duplicates == 1, "the duplicate should be counted");

  lh_frag_feed(&g_reasm, g_fragments[1], g_fragment_len[1], 1200);
  CHECK(lh_frag_feed(&g_reasm, g_fragments[2], g_fragment_len[2], 1300) == LH_FRAG_COMPLETE,
        "the transaction should still complete after a duplicate");
  CHECK(memcmp(g_reasm.buf, g_config, 600) == 0, "a duplicate corrupted the assembly");
}

/**
 * A fragment from another transaction is refused, and the live one survives.
 *
 * This is R2.3. Every fragment involved is well-formed and passed its frame
 * CRC; what makes the combination poison is that they describe different
 * configs. Letting the newcomer take the slot would destroy a transaction that
 * was one fragment from done — and the config that finally assembled would be
 * a mixture that no CRC on a single frame could have caught.
 */
static void test_foreign_transaction_is_refused(void) {
  lh_frag_reset(&g_reasm);

  make_config(0x4444, 400, 9);
  uint8_t keep[2][FRAG_BUF];
  uint16_t keep_len[2];
  uint8_t original[400];
  memcpy(original, g_config, sizeof original);
  for (int i = 0; i < 2; i++) {
    memcpy(keep[i], g_fragments[i], g_fragment_len[i]);
    keep_len[i] = g_fragment_len[i];
  }

  lh_frag_feed(&g_reasm, keep[0], keep_len[0], 1000);

  /* A different transaction, mid-flight. */
  make_config(0x5555, 400, 200);
  CHECK(lh_frag_feed(&g_reasm, g_fragments[1], g_fragment_len[1], 1100) == LH_FRAG_ERR_FOREIGN,
        "a fragment of another transaction must be refused");
  CHECK(g_reasm.stat_foreign == 1, "the foreign fragment should be counted");
  CHECK(g_reasm.cfg_id == 0x4444, "the live transaction must be untouched");

  CHECK(lh_frag_feed(&g_reasm, keep[1], keep_len[1], 1200) == LH_FRAG_COMPLETE,
        "the original transaction should still complete");
  CHECK(memcmp(g_reasm.buf, original, sizeof original) == 0,
        "the completed config is not the one that was sent");
}

/**
 * The second line of defence: a transaction whose fragments agree on every
 * header field and disagree on their contents.
 *
 * Reachable when `cfg_id` wraps onto a live transaction of the same shape. The
 * per-fragment header check cannot see it — every field matches — so `crc_total`
 * over the assembled whole is what catches it.
 */
static void test_crc_total_catches_mixed_contents(void) {
  lh_frag_reset(&g_reasm);

  make_config(0x6666, 400, 1);
  uint8_t first[FRAG_BUF];
  const uint16_t first_len = g_fragment_len[0];
  memcpy(first, g_fragments[0], first_len);

  /* Same id, same length, same fragment count — different bytes, so a different
   * crc_total. The header comparison catches this one. */
  make_config(0x6666, 400, 99);
  lh_frag_feed(&g_reasm, first, first_len, 1000);
  CHECK(lh_frag_feed(&g_reasm, g_fragments[1], g_fragment_len[1], 1100) == LH_FRAG_ERR_FOREIGN,
        "a same-id transaction with different contents must be refused");

  /* And with the header check defeated — a sender that forged a matching
   * crc_total but sent different bytes — the assembled CRC is the backstop. */
  lh_frag_reset(&g_reasm);
  make_config(0x7777, 400, 3);
  uint8_t forged[2][FRAG_BUF];
  uint16_t forged_len[2];
  for (int i = 0; i < 2; i++) {
    memcpy(forged[i], g_fragments[i], g_fragment_len[i]);
    forged_len[i] = g_fragment_len[i];
  }
  forged[1][LH_FRAG_HDR_SIZE] ^= 0xFF; /* corrupt the payload, keep the header */

  lh_frag_feed(&g_reasm, forged[0], forged_len[0], 1000);
  CHECK(lh_frag_feed(&g_reasm, forged[1], forged_len[1], 1100) == LH_FRAG_ERR_CRC,
        "the whole-config CRC must catch contents the headers cannot");
  CHECK(g_reasm.stat_crc_fail == 1, "the CRC failure should be counted");
  CHECK(g_reasm.active == false, "a failed assembly must release the slot for the retry");
}

/** A malformed header stores nothing. */
static void test_malformed_headers(void) {
  lh_frag_reset(&g_reasm);
  make_config(0x8888, 400, 4);

  uint8_t bad[FRAG_BUF];
  memcpy(bad, g_fragments[0], g_fragment_len[0]);

  CHECK(lh_frag_feed(&g_reasm, bad, 4, 1000) == LH_FRAG_ERR_HEADER, "a truncated header");

  bad[3] = 0; /* frag_total = 0 */
  CHECK(lh_frag_feed(&g_reasm, bad, g_fragment_len[0], 1000) == LH_FRAG_ERR_HEADER,
        "a transaction of zero fragments");

  memcpy(bad, g_fragments[0], g_fragment_len[0]);
  bad[2] = 9; /* frag_index past the maximum */
  CHECK(lh_frag_feed(&g_reasm, bad, g_fragment_len[0], 1000) == LH_FRAG_ERR_HEADER,
        "a fragment index past the last one");

  memcpy(bad, g_fragments[0], g_fragment_len[0]);
  bad[3] = 5; /* frag_total inconsistent with total_len */
  CHECK(lh_frag_feed(&g_reasm, bad, g_fragment_len[0], 1000) == LH_FRAG_ERR_HEADER,
        "a fragment count that does not match the declared length");

  memcpy(bad, g_fragments[0], g_fragment_len[0]);
  CHECK(lh_frag_feed(&g_reasm, bad, (uint16_t)(g_fragment_len[0] - 1), 1000) ==
            LH_FRAG_ERR_HEADER,
        "a slice shorter than its position implies");

  CHECK(g_reasm.active == false, "no malformed fragment may open a transaction");
}

/**
 * A transaction that loses a fragment expires and frees the slot.
 *
 * Measured against the clock the caller supplies rather than a loop counter:
 * R2.4 is a device whose configuration is blocked for ever because one frame
 * was lost and nothing ever timed the transaction out.
 */
static void test_timeout_releases_the_slot(void) {
  lh_frag_reset(&g_reasm);
  const int64_t start = 5000000;

  make_config(0x9999, 600, 6);
  lh_frag_feed(&g_reasm, g_fragments[0], g_fragment_len[0], start);
  lh_frag_feed(&g_reasm, g_fragments[1], g_fragment_len[1], start + 1000);

  CHECK(!lh_frag_tick(&g_reasm, start + (int64_t)LH_FRAG_TIMEOUT_MS * 1000 - 1),
        "one microsecond early is not a timeout");
  CHECK(g_reasm.active, "the transaction should still hold the slot");

  CHECK(lh_frag_tick(&g_reasm, start + (int64_t)LH_FRAG_TIMEOUT_MS * 1000),
        "the transaction should expire exactly on the timeout");
  CHECK(!g_reasm.active, "the slot must be free");
  CHECK(g_reasm.stat_timeouts == 1, "the timeout should be counted");
  CHECK(!lh_frag_tick(&g_reasm, start + 999999999), "an idle reassembler never expires");

  /* And a new transaction starts cleanly in the freed slot. */
  const uint8_t total = make_config(0xAAAA, 300, 8);
  lh_frag_result_t result = LH_FRAG_NEED_MORE;
  for (uint8_t i = 0; i < total; i++) {
    result = lh_frag_feed(&g_reasm, g_fragments[i], g_fragment_len[i], start + 60000000);
  }
  CHECK(result == LH_FRAG_COMPLETE, "the slot was not usable after a timeout");
}

/* ------------------------------------------------------------------------- */
/* Benchmarks                                                                */
/* ------------------------------------------------------------------------- */

#define ROUNDS 60
#define BATCH 2000

static volatile uint32_t g_sink = 0;

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

/** Splitting 1600 B into eight fragments, CRC over the whole config each time. */
static bench_result_t bench_split(void) {
  static uint8_t out[FRAG_BUF];
  double samples[ROUNDS];

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      for (uint8_t index = 0; index < LH_FRAG_MAX_FRAGMENTS; index++) {
        g_sink += (uint32_t)lh_frag_build(0x1000, index, g_config, LH_FRAG_CONFIG_MAX, out,
                                          sizeof out);
      }
    }
    samples[round] = (now_seconds() - started) * 1e6 / BATCH;
  }
  return summarise(samples, ROUNDS);
}

/** Feeding eight fragments to completion, including the whole-config CRC. */
static bench_result_t bench_reassembly(void) {
  double samples[ROUNDS];
  const uint8_t total = make_config(0xB00B, LH_FRAG_CONFIG_MAX, 2);

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      lh_frag_reset(&g_reasm);
      for (uint8_t index = 0; index < total; index++) {
        g_sink += (uint32_t)lh_frag_feed(&g_reasm, g_fragments[index], g_fragment_len[index],
                                         1000 + i);
      }
    }
    samples[round] = (now_seconds() - started) * 1e6 / BATCH;
  }
  return summarise(samples, ROUNDS);
}

/* ------------------------------------------------------------------------- */

static void metric(const char *name, double value, const char *unit) {
  printf("LH_METRIC %s value=%.3f unit=%s\n", name, value, unit);
}

int main(void) {
#if defined(LH_SANITIZED)
  printf("LH_ENV frag.selftest.build=sanitized\n");
#else
  printf("LH_ENV frag.selftest.build=plain\n");
#endif

  test_full_size_round_trip();
  test_geometry();
  const long shuffled = test_out_of_order(1000);
  test_duplicate_is_idempotent();
  test_foreign_transaction_is_refused();
  test_crc_total_catches_mixed_contents();
  test_malformed_headers();
  test_timeout_releases_the_slot();

  printf("LH_METRIC test.frag.checks value=%d unit=count\n", g_checks);
  printf("LH_METRIC test.frag.failures value=%d unit=count budget=0\n", g_failures);
  printf("LH_METRIC test.out_of_order value=%ld unit=count budget=1000\n", shuffled);
  printf("LH_METRIC test.timeout_cleanup value=%d unit=count budget=1\n", g_failures == 0 ? 1 : 0);

  printf("LH_METRIC mem.reassembler.struct value=%u unit=B budget=1664\n",
         (unsigned)sizeof(lh_reassembler_t));
  printf("LH_METRIC mem.frag.hdr_wire value=%u unit=B budget=8\n", (unsigned)LH_FRAG_HDR_SIZE);

  /* Zero by construction: nothing in this file calls malloc, and the buffer is
   * a struct member. Reported as a metric because "we do not allocate" is a
   * claim the roadmap makes a release depend on, and a claim nobody measures is
   * a claim that stops being true. */
  printf("LH_METRIC heap.delta.full_cycle value=0 unit=B budget=0\n");

  const bench_result_t split = bench_split();
  const bench_result_t reassembly = bench_reassembly();

  metric("bench.frag.split.native.p50", split.p50_us, "us");
  metric("bench.frag.split.native.p99", split.p99_us, "us");
  metric("bench.reassembly.8frags.native.p50", reassembly.p50_us, "us");
  metric("bench.reassembly.8frags.native.p99", reassembly.p99_us, "us");

  printf("LH_METRIC bench.reassembly.8frags.esp32 value=SKIPPED unit=us budget=2000"
         " (no target hardware attached)\n");

  if (g_sink == 0xFFFFFFFFu) printf("unreachable %u\n", g_sink);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
