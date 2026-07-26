/*
 * Native correctness, property and benchmark harness for the dedup window (T2.1).
 *
 * The Unity suite in firmware/node/test/test_dedup proves the code behaves on
 * the ABI it ships to. This proves it behaves at volume: a hundred thousand
 * frames with retransmissions, reordering and sequence wrap mixed in, checked
 * against an oracle that knows which frames are genuinely distinct.
 *
 * That oracle is the reason this file exists. "No frame is processed twice" is
 * not observable from the dedup window's own state — it is a property of the
 * stream, and the only way to check it is to give every frame an identity the
 * window cannot see (a monotonically increasing id, of which the sequence
 * number is the low 8 bits) and then assert that no id is ever accepted twice.
 * That number is the Etap 2 hard invariant, `chaos.double_processed == 0`,
 * measured at the layer that is supposed to guarantee it.
 *
 * Driven by tools/run-native.mjs. Output is LH_METRIC lines per roadmap §0.4;
 * exit status is non-zero if any assertion failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/dedup.h"

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

/** xorshift32, seeded identically on every run — see slip_selftest.c on why. */
static uint32_t g_rng = 0x5D3C7A19u;

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

/* A clock that only has to be monotonic: eviction is LRU, not expiry. */
static int64_t g_clock_us = 1000;
static int64_t tick(void) { return (g_clock_us += 1000); }

/* ------------------------------------------------------------------------- */
/* Correctness                                                               */
/* ------------------------------------------------------------------------- */

/**
 * The wrap case, which is the entire reason sequence comparison is modular.
 *
 * 256 frames in order, then 64 more that start again at 0. Every one of them is
 * a new frame. An implementation comparing `seq > last_seq` passes the first
 * 256 and then rejects everything for ever — in the field, three weeks in,
 * looking exactly like a radio fault.
 */
static void test_sequence_wrap(void) {
  lh_dedup_t dedup;
  lh_dedup_init(&dedup);

  int accepted = 0;
  for (int i = 0; i < 320; i++) {
    if (lh_dedup_check_and_mark(&dedup, 0x0101, (uint8_t)(i & 0xFF), tick())) accepted++;
  }

  CHECK(accepted == 320, "320 in-order frames across a wrap should all be new, got %d", accepted);
  CHECK(dedup.stat_dupes_dropped == 0, "no duplicates were sent, %lu counted",
        (unsigned long)dedup.stat_dupes_dropped);
  CHECK(dedup.stat_too_old == 0, "nothing was late, %lu counted",
        (unsigned long)dedup.stat_too_old);
}

/** Every one of the last 32 sequence numbers, replayed, must be refused. */
static void test_window_rejects_replays(void) {
  lh_dedup_t dedup;
  lh_dedup_init(&dedup);

  for (int i = 0; i < 200; i++) lh_dedup_check_and_mark(&dedup, 0x0202, (uint8_t)i, tick());

  const uint8_t newest = 199;
  int refused = 0;
  for (uint8_t age = 0; age < LH_DEDUP_WINDOW; age++) {
    if (!lh_dedup_check_and_mark(&dedup, 0x0202, (uint8_t)(newest - age), tick())) refused++;
  }

  CHECK(refused == (int)LH_DEDUP_WINDOW, "all %u window entries should be refused, %d were",
        (unsigned)LH_DEDUP_WINDOW, refused);
  CHECK(dedup.stat_dupes_dropped == LH_DEDUP_WINDOW, "expected %u duplicates, counted %lu",
        (unsigned)LH_DEDUP_WINDOW, (unsigned long)dedup.stat_dupes_dropped);
}

/**
 * A frame older than the window is refused, and counted separately.
 *
 * The distinction matters operationally: duplicates rising means the ARQ is
 * retransmitting, which is normal. `too_old` rising means frames are arriving
 * more than 32 behind the newest, which is a link reordering far deeper than
 * anything the design assumes, and no amount of retransmission will fix it.
 */
static void test_older_than_window(void) {
  lh_dedup_t dedup;
  lh_dedup_init(&dedup);

  for (int i = 0; i < 100; i++) lh_dedup_check_and_mark(&dedup, 0x0303, (uint8_t)i, tick());

  CHECK(!lh_dedup_check_and_mark(&dedup, 0x0303, 99 - 40, tick()), "40 behind should be refused");
  CHECK(dedup.stat_too_old == 1, "expected 1 too_old, counted %lu",
        (unsigned long)dedup.stat_too_old);
  CHECK(dedup.stat_dupes_dropped == 0, "a late frame is not a duplicate");

  /* Exactly on the boundary: age 31 is inside the window, age 32 is not. */
  lh_dedup_t edge;
  lh_dedup_init(&edge);
  lh_dedup_check_and_mark(&edge, 0x0304, 100, tick());
  CHECK(lh_dedup_check_and_mark(&edge, 0x0304, 100 - 31, tick()), "age 31 is inside the window");
  CHECK(!lh_dedup_check_and_mark(&edge, 0x0304, 100 - 32, tick()), "age 32 is outside it");
}

/** Out-of-order arrival: each frame new once, and refused on replay. */
static void test_out_of_order(void) {
  lh_dedup_t dedup;
  lh_dedup_init(&dedup);

  const uint8_t arrival[5] = {5, 3, 7, 4, 6};
  for (int i = 0; i < 5; i++) {
    CHECK(lh_dedup_check_and_mark(&dedup, 0x0404, arrival[i], tick()),
          "shuffled seq %u should be new", (unsigned)arrival[i]);
  }

  const uint8_t replay[3] = {3, 4, 5};
  for (int i = 0; i < 3; i++) {
    CHECK(!lh_dedup_check_and_mark(&dedup, 0x0404, replay[i], tick()),
          "replayed seq %u should be a duplicate", (unsigned)replay[i]);
  }

  /* And the gaps that were never filled are still open. */
  CHECK(lh_dedup_check_and_mark(&dedup, 0x0404, 2, tick()), "seq 2 never arrived; it is new");
}

/**
 * Peers are independent, and the ninth evicts the least recently heard.
 *
 * The second half of this test is the part that matters: after eviction, the
 * evicted peer's history is gone, so its next frame is unconditionally new even
 * if it is a duplicate. That is a real hole in the guarantee, and the counter
 * is what makes it visible rather than mysterious.
 */
static void test_peer_table(void) {
  lh_dedup_t dedup;
  lh_dedup_init(&dedup);

  for (uint16_t peer = 0; peer < LH_DEDUP_PEERS; peer++) {
    CHECK(lh_dedup_check_and_mark(&dedup, (uint16_t)(0x1000 + peer), 7, tick()),
          "peer %u seq 7 should be new", (unsigned)peer);
  }
  /* Same sequence number from eight different senders — no cross-talk. */
  CHECK(dedup.stat_dupes_dropped == 0, "peers must not share a window");
  CHECK(dedup.peer_count == LH_DEDUP_PEERS, "expected a full table, have %u",
        (unsigned)dedup.peer_count);

  /* Touch every peer except the first, so it is the least recently heard. */
  for (uint16_t peer = 1; peer < LH_DEDUP_PEERS; peer++) {
    lh_dedup_check_and_mark(&dedup, (uint16_t)(0x1000 + peer), 8, tick());
  }

  CHECK(lh_dedup_check_and_mark(&dedup, 0x2000, 1, tick()), "a ninth sender should be accepted");
  CHECK(dedup.stat_peer_evicted == 1, "expected 1 eviction, counted %lu",
        (unsigned long)dedup.stat_peer_evicted);
  CHECK(lh_dedup_find_peer(&dedup, 0x1000) == NULL, "the idle peer should have been evicted");
  CHECK(lh_dedup_find_peer(&dedup, 0x1001) != NULL, "an active peer must not be evicted");
  CHECK(dedup.peer_count == LH_DEDUP_PEERS, "the table should stay full, have %u",
        (unsigned)dedup.peer_count);

  /* The evicted peer's replay is now indistinguishable from a new frame. */
  CHECK(lh_dedup_check_and_mark(&dedup, 0x1000, 7, tick()),
        "after eviction, history is gone — this is the documented hole");
}

/* ------------------------------------------------------------------------- */
/* Property test                                                             */
/* ------------------------------------------------------------------------- */

#define PROPERTY_EVENTS 100000
#define PROPERTY_PEERS 4
#define RESEND_DEPTH 24

typedef struct {
  uint32_t double_processed; /* THE invariant: must be 0                   */
  uint32_t accepted;
  uint32_t refused;
  uint32_t originals_lost; /* first sighting refused — costs a frame       */
} property_result_t;

/**
 * A stream with retransmissions and reordering, checked against frame identity.
 *
 * Each peer numbers its frames from 0 upward; the sequence number on the wire
 * is the low byte of that id, so ids wrap past 255 exactly as the protocol
 * does. Roughly a third of events are a replay of one of the last 24 ids,
 * which is what an ARQ under loss actually produces — duplicates clustered
 * close behind the newest frame, not scattered uniformly.
 *
 * `seen` records which ids the window let through. Accepting one twice is the
 * failure this whole component exists to prevent.
 */
static property_result_t test_property(void) {
  static uint8_t seen[PROPERTY_PEERS][PROPERTY_EVENTS];
  lh_dedup_t dedup;
  property_result_t out;
  uint32_t next_id[PROPERTY_PEERS];

  memset(seen, 0, sizeof seen);
  memset(&out, 0, sizeof out);
  memset(next_id, 0, sizeof next_id);
  lh_dedup_init(&dedup);

  for (int event = 0; event < PROPERTY_EVENTS; event++) {
    const uint32_t draw = next_random();
    const int peer = (int)(draw % PROPERTY_PEERS);

    uint32_t id;
    if ((draw >> 8) % 3 == 0 && next_id[peer] > RESEND_DEPTH) {
      /* A retransmission of something recent. */
      id = next_id[peer] - 1 - ((draw >> 16) % RESEND_DEPTH);
    } else {
      id = next_id[peer]++;
    }

    const bool accepted =
        lh_dedup_check_and_mark(&dedup, (uint16_t)(0x0500 + peer), (uint8_t)(id & 0xFFu), tick());

    if (accepted) {
      out.accepted++;
      if (seen[peer][id] != 0) out.double_processed++;
      seen[peer][id] = 1;
    } else {
      out.refused++;
      if (seen[peer][id] == 0) out.originals_lost++;
    }
  }

  CHECK(out.double_processed == 0, "INVARIANT BROKEN: %lu frames processed twice",
        (unsigned long)out.double_processed);
  /* Every refusal must be a frame we had already accepted. A refusal of a frame
   * never seen would mean the window is dropping originals, which is a
   * correctness failure of a different kind — the link losing data silently. */
  CHECK(out.originals_lost == 0, "%lu first-sightings were refused",
        (unsigned long)out.originals_lost);
  return out;
}

/* ------------------------------------------------------------------------- */
/* Benchmarks                                                                */
/* ------------------------------------------------------------------------- */

#define ROUNDS 60
#define BATCH 20000

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

/** In-order traffic from a single peer: the path almost every frame takes. */
static bench_result_t bench_typical(void) {
  double samples[ROUNDS];
  lh_dedup_t dedup;
  lh_dedup_init(&dedup);
  uint8_t seq = 0;

  for (int i = 0; i < BATCH; i++) g_sink += lh_dedup_check_and_mark(&dedup, 0x0601, seq++, i);

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) g_sink += lh_dedup_check_and_mark(&dedup, 0x0601, seq++, i);
    samples[round] = (now_seconds() - started) * 1e6 / BATCH;
  }
  return summarise(samples, ROUNDS);
}

/**
 * The worst case the roadmap names: a full peer table, and the sender we want
 * sitting at the end of it.
 *
 * The lookup is linear over eight entries, so this is where the cost lives.
 * Measuring the last slot rather than an average makes the number an upper
 * bound instead of a description of one particular traffic mix.
 */
static bench_result_t bench_worst(void) {
  double samples[ROUNDS];
  lh_dedup_t dedup;
  lh_dedup_init(&dedup);

  for (uint16_t peer = 0; peer < LH_DEDUP_PEERS; peer++) {
    lh_dedup_check_and_mark(&dedup, (uint16_t)(0x0700 + peer), 0, peer);
  }
  const uint16_t last = (uint16_t)(0x0700 + LH_DEDUP_PEERS - 1);

  /* A duplicate, so the state does not drift over a hundred million calls and
   * the measured path is the full one: lookup, delta, mask, counter. */
  for (int i = 0; i < BATCH; i++) g_sink += lh_dedup_check_and_mark(&dedup, last, 0, i);

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) g_sink += lh_dedup_check_and_mark(&dedup, last, 0, i);
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
  printf("LH_ENV dedup.selftest.build=sanitized\n");
#else
  printf("LH_ENV dedup.selftest.build=plain\n");
#endif

  test_sequence_wrap();
  test_window_rejects_replays();
  test_older_than_window();
  test_out_of_order();
  test_peer_table();

  const property_result_t property = test_property();

  printf("LH_METRIC test.dedup.checks value=%d unit=count\n", g_checks);
  printf("LH_METRIC test.dedup.failures value=%d unit=count budget=0\n", g_failures);
  printf("LH_METRIC test.dedup.property_events value=%d unit=count budget=%d\n", PROPERTY_EVENTS,
         PROPERTY_EVENTS);
  printf("LH_METRIC test.dedup.double_processed value=%lu unit=count budget=0\n",
         (unsigned long)property.double_processed);
  printf("LH_METRIC test.dedup.originals_lost value=%lu unit=count budget=0\n",
         (unsigned long)property.originals_lost);
  printf("LH_METRIC test.dedup.property_dupes_refused value=%lu unit=count\n",
         (unsigned long)property.refused);

  printf("LH_METRIC mem.dedup.struct value=%u unit=B budget=256\n", (unsigned)sizeof(lh_dedup_t));
  printf("LH_METRIC mem.dedup.per_peer value=%u unit=B\n", (unsigned)sizeof(lh_dedup_peer_t));

  const bench_result_t typical = bench_typical();
  const bench_result_t worst = bench_worst();

  metric("bench.dedup.check.native.p50", typical.p50_us, "us");
  metric("bench.dedup.check.native.p99", typical.p99_us, "us");
  metric("bench.dedup.check.worst.native.p50", worst.p50_us, "us");
  metric("bench.dedup.check.worst.native.p99", worst.p99_us, "us");

  /* The roadmap's 5 us budget is an on-target figure and there is no board on
   * this bench. Emitting SKIPPED keeps the budget attached to the measurement
   * it was written for; a host CPU clears it by three orders of magnitude, so
   * gating the native number would be a tick that can never fail. */
  printf("LH_METRIC bench.dedup.check.esp32 value=SKIPPED unit=us budget=5"
         " (no target hardware attached)\n");

  if (g_sink == 0xFFFFFFFFu) printf("unreachable %u\n", g_sink);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
