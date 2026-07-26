/*
 * Native harness for the frame build/parse path with CRC (T2.2).
 *
 * The CRC itself was verified in T0.5 — the polynomial, the check value, 500
 * cross-language vectors. None of that says anything about whether it is
 * *applied* correctly: over the right span, at the right offset, in the right
 * byte order, on both the sending and the receiving side. A CRC computed over
 * header+payload on one end and over payload alone on the other agrees with
 * itself perfectly and rejects every frame, and both implementations pass their
 * own unit tests while doing it.
 *
 * So what is measured here is detection: ten thousand frames, one flipped bit
 * each, and the answer must be ten thousand rejections. Then two- and three-bit
 * flips, where 100% is not available — CRC-16 has 65536 residues and a fraction
 * of multi-bit patterns are invisible to it by construction. Recording the real
 * rate rather than asserting a round number is the difference between knowing
 * the code's error detection and hoping for it.
 *
 * Driven by tools/run-native.mjs; LH_METRIC lines on stdout per §0.4.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/protocol.h"

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

static uint32_t g_rng = 0x3A5F1C08u;

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

#define FRAME_CAP LORAHOME_MAX_FRAME_SIZE

static uint16_t build_reference_frame(uint8_t *out, uint16_t payload_len, uint8_t seq) {
  static uint8_t payload[LORAHOME_MAX_PAYLOAD];
  const lorahome_header_t header = {LORAHOME_FRAME_TELEMETRY, 0x0102, 0x0304, seq,
                                    LORAHOME_FLAG_NONE};

  for (uint16_t i = 0; i < payload_len; i++) payload[i] = (uint8_t)next_random();

  const int len = lh_frame_build(&header, payload, payload_len, out, FRAME_CAP);
  return len < 0 ? 0 : (uint16_t)len;
}

/* ------------------------------------------------------------------------- */
/* Correctness                                                               */
/* ------------------------------------------------------------------------- */

/** Round trip, and the zero-copy promise: the payload is a view, not a copy. */
static void test_round_trip_is_zero_copy(void) {
  uint8_t frame[FRAME_CAP];
  const uint16_t len = build_reference_frame(frame, 64, 11);
  lh_frame_view_t view;

  CHECK(lh_frame_parse(frame, len, &view) == LH_OK, "a well-formed frame must parse");
  CHECK(view.payload == frame + LORAHOME_HEADER_SIZE,
        "the payload must point into the caller's buffer, not at a copy");
  CHECK(view.payload_len == 64, "payload length is %u, expected 64", (unsigned)view.payload_len);
  CHECK(view.hdr.src_id == 0x0102 && view.hdr.dst_id == 0x0304, "header fields did not survive");
  CHECK(view.hdr.seq == 11, "seq did not survive");
  CHECK(view.crc_rx == view.crc_calc, "CRCs must agree on a good frame");
}

/**
 * Every rejection reason, and the order they are decided in.
 *
 * The order is the point. A two-byte fragment fails the CRC check too, and a
 * frame of somebody else's protocol fails everything — reporting the first true
 * cause rather than the first check that happens to fire is what makes the
 * Bridge's counters a diagnosis instead of a tally.
 */
static void test_error_taxonomy(void) {
  uint8_t frame[FRAME_CAP];
  const uint16_t len = build_reference_frame(frame, 32, 1);
  lh_frame_view_t view;

  CHECK(lh_frame_parse(frame, 9, &view) == LH_ERR_TOO_SHORT, "9 bytes is too short");
  CHECK(lh_frame_parse(frame, LORAHOME_MAX_FRAME_SIZE + 1, &view) == LH_ERR_TOO_LONG,
        "one byte past the MTU is too long");

  uint8_t alien[FRAME_CAP];
  memcpy(alien, frame, len);
  alien[0] = 0x7E;
  CHECK(lh_frame_parse(alien, len, &view) == LH_ERR_BAD_MAGIC,
        "another protocol's traffic is a magic error, not a CRC error");

  uint8_t corrupt[FRAME_CAP];
  memcpy(corrupt, frame, len);
  corrupt[12] ^= 0x01;
  CHECK(lh_frame_parse(corrupt, len, &view) == LH_ERR_BAD_CRC, "a flipped bit is a CRC error");
  CHECK(view.crc_rx != view.crc_calc, "both CRCs must be reported on a mismatch");
  CHECK(view.crc_rx != 0 && view.crc_calc != 0, "a CRC failure must still say what it saw");

  /* An unknown type is reported, but the view is complete: the caller decides
   * whether to route it, and cannot decide anything about a frame it was not
   * given. */
  uint8_t future[FRAME_CAP];
  const lorahome_header_t future_header = {0x7F, 0x0505, 0x0606, 3, LORAHOME_FLAG_NONE};
  const int future_len = lh_frame_build(&future_header, NULL, 0, future, sizeof future);
  CHECK(future_len > 0, "could not build an unknown-type frame");
  CHECK(lh_frame_parse(future, (uint16_t)future_len, &view) == LH_ERR_BAD_TYPE,
        "an unknown type should be reported");
  CHECK(view.hdr.type == 0x7F && view.payload != NULL,
        "the view must be complete even for an unknown type");
}

/** The build side refuses what it cannot represent, and writes nothing. */
static void test_build_limits(void) {
  static uint8_t payload[LORAHOME_MAX_PAYLOAD + 1];
  uint8_t out[FRAME_CAP + 1];
  const lorahome_header_t header = {LORAHOME_FRAME_TELEMETRY, 1, 2, 3, 0};

  memset(out, 0xEE, sizeof out);
  CHECK(lh_frame_build(&header, payload, LORAHOME_MAX_PAYLOAD + 1, out, sizeof out) ==
            LH_ERR_TOO_LONG,
        "a payload past MAX_PAYLOAD must be refused");
  CHECK(out[0] == 0xEE, "a refused build must not touch the buffer");

  CHECK(lh_frame_build(&header, payload, 32, out, 41) == LH_ERR_TOO_SHORT,
        "a buffer one byte short must be refused");
  CHECK(out[0] == 0xEE, "a refused build must not touch the buffer");

  CHECK(lh_frame_build(&header, payload, 32, out, 42) == 42, "42 bytes is exactly enough");

  /* A full-MTU frame is exactly the MTU on the wire — the arithmetic the whole
   * payload budget rests on. */
  CHECK(lh_frame_build(&header, payload, LORAHOME_MAX_PAYLOAD, out, sizeof out) ==
            LORAHOME_MAX_FRAME_SIZE,
        "a full payload should produce exactly a %d byte frame", LORAHOME_MAX_FRAME_SIZE);
}

/* ------------------------------------------------------------------------- */
/* Bit-flip detection                                                        */
/* ------------------------------------------------------------------------- */

typedef struct {
  long trials;
  long detected; /* rejected for any reason — the number that matters */
  long by_crc;
  long by_magic; /* the flip landed in byte 0 and never reached the CRC */
} detection_t;

/**
 * Flips `bits` distinct bits in a frame and asks whether the CRC notices.
 *
 * Positions are drawn across the whole frame including the header and the CRC
 * field itself, because in the field the corruption does not politely stay in
 * the payload — and a header bit flip that goes undetected is worse than a
 * payload one, since it redirects a frame rather than garbling it.
 *
 * Detection means "not accepted", not "failed the CRC check specifically".
 * Roughly 1.5% of single-bit flips land in the magic byte and are refused
 * before the CRC is ever computed; counting those as misses would understate
 * the protection by measuring which check fired rather than whether the damaged
 * frame got through. Both numbers are reported, because a shift in the split is
 * itself a signal that the check order changed.
 */
static detection_t test_bitflip(int bits, long trials) {
  uint8_t frame[FRAME_CAP];
  uint8_t damaged[FRAME_CAP];
  detection_t out = {0, 0, 0, 0};
  lh_frame_view_t view;

  for (long trial = 0; trial < trials; trial++) {
    const uint16_t payload_len = (uint16_t)(next_random() % (LORAHOME_MAX_PAYLOAD + 1));
    const uint16_t len = build_reference_frame(frame, payload_len, (uint8_t)trial);
    if (len == 0) {
      CHECK(0, "could not build a frame of payload %u", (unsigned)payload_len);
      continue;
    }

    memcpy(damaged, frame, len);
    int flipped = 0;
    while (flipped < bits) {
      const uint32_t bit_index = next_random() % ((uint32_t)len * 8u);
      const uint16_t byte = (uint16_t)(bit_index / 8u);
      const uint8_t mask = (uint8_t)(1u << (bit_index % 8u));
      /* Distinct bits: flipping the same bit twice would cancel out and count
       * as an undetected error that never happened. */
      if ((damaged[byte] ^ frame[byte]) & mask) continue;
      damaged[byte] ^= mask;
      flipped++;
    }

    out.trials++;
    const lh_err_t err = lh_frame_parse(damaged, len, &view);
    if (err != LH_OK) out.detected++;
    if (err == LH_ERR_BAD_CRC) out.by_crc++;
    if (err == LH_ERR_BAD_MAGIC) out.by_magic++;
  }

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

static bench_result_t bench_build(void) {
  static uint8_t payload[LORAHOME_MAX_PAYLOAD];
  static uint8_t out[FRAME_CAP];
  const lorahome_header_t header = {LORAHOME_FRAME_TELEMETRY, 0x0102, 0x0304, 0, 0};
  double samples[ROUNDS];

  for (int i = 0; i < BATCH; i++) {
    g_sink += (uint32_t)lh_frame_build(&header, payload, LORAHOME_MAX_PAYLOAD, out, FRAME_CAP);
  }

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      g_sink += (uint32_t)lh_frame_build(&header, payload, LORAHOME_MAX_PAYLOAD, out, FRAME_CAP);
    }
    samples[round] = (now_seconds() - started) * 1e6 / BATCH;
  }
  return summarise(samples, ROUNDS);
}

static bench_result_t bench_parse(void) {
  uint8_t frame[FRAME_CAP];
  lh_frame_view_t view;
  double samples[ROUNDS];

  const uint16_t len = build_reference_frame(frame, LORAHOME_MAX_PAYLOAD, 0);

  for (int i = 0; i < BATCH; i++) g_sink += (uint32_t)lh_frame_parse(frame, len, &view);

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) g_sink += (uint32_t)lh_frame_parse(frame, len, &view);
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
  printf("LH_ENV frame.selftest.build=sanitized\n");
#else
  printf("LH_ENV frame.selftest.build=plain\n");
#endif

  test_round_trip_is_zero_copy();
  test_error_taxonomy();
  test_build_limits();

  const detection_t flip1 = test_bitflip(1, 10000);
  const detection_t flip2 = test_bitflip(2, 10000);
  const detection_t flip3 = test_bitflip(3, 10000);

  /* A single flipped bit is always detected: CRC-16/CCITT's generator has more
   * than one term, so no single-bit error can be a multiple of it. That is a
   * property of the polynomial, which makes 10000/10000 a budget rather than an
   * observation — anything less means the CRC is not being applied over the
   * span it is supposed to cover. */
  CHECK(flip1.detected == flip1.trials, "single-bit detection is %ld/%ld, must be perfect",
        flip1.detected, flip1.trials);

  printf("LH_METRIC test.frame.checks value=%d unit=count\n", g_checks);
  printf("LH_METRIC test.frame.failures value=%d unit=count budget=0\n", g_failures);
  printf("LH_METRIC test.bitflip_1.detection value=%ld unit=count budget=%ld\n", flip1.detected,
         flip1.trials);
  printf("LH_METRIC test.bitflip_1.by_crc value=%ld unit=count\n", flip1.by_crc);
  printf("LH_METRIC test.bitflip_1.by_magic value=%ld unit=count\n", flip1.by_magic);
  printf("LH_METRIC test.bitflip_2.detection value=%ld unit=count\n", flip2.detected);
  printf("LH_METRIC test.bitflip_3.detection value=%ld unit=count\n", flip3.detected);
  metric("test.bitflip_2.detection_pct", 100.0 * (double)flip2.detected / (double)flip2.trials,
         "pct");
  metric("test.bitflip_3.detection_pct", 100.0 * (double)flip3.detected / (double)flip3.trials,
         "pct");

  const bench_result_t build = bench_build();
  const bench_result_t parse = bench_parse();

  metric("bench.frame.build_full.native.p50", build.p50_us, "us");
  metric("bench.frame.build_full.native.p99", build.p99_us, "us");
  metric("bench.frame.parse_full.native.p50", parse.p50_us, "us");
  metric("bench.frame.parse_full.native.p99", parse.p99_us, "us");

  /* The roadmap's 150 us budgets are on-target figures; a host CPU clears them
   * by two orders of magnitude, so gating the native numbers would be a tick
   * that can never fail. The budget stays attached to the measurement it was
   * written for. */
  printf("LH_METRIC bench.frame.build_full.esp32 value=SKIPPED unit=us budget=150"
         " (no target hardware attached)\n");
  printf("LH_METRIC bench.frame.parse_full.esp32 value=SKIPPED unit=us budget=150"
         " (no target hardware attached)\n");

  if (g_sink == 0xFFFFFFFFu) printf("unreachable %u\n", g_sink);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
