/*
 * Native correctness, flood and benchmark harness for the UART ring (T1.2).
 *
 * The ring's hard parts are arithmetic, not electrical: free-running uint16_t
 * indices that must survive their own wraparound, occupancy that must stay
 * correct across it, and an overrun path that must lose exactly the bytes it
 * says it lost. All of that is testable without a UART, and testing it here
 * means the on-hardware flood test is checking the wiring rather than the
 * algebra.
 *
 * What this cannot test is the ISR/task race the fences exist for. A host
 * harness has neither an interrupt nor a second core, so the ordering claim
 * rests on the fences themselves plus review; the 10 MB flood over a real UART
 * (deferred, no board attached) is what would exercise it for real.
 *
 * Driven by tools/run-native.mjs. Emits LH_METRIC lines; non-zero exit on any
 * failed assertion.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/ring.h"

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

static uint32_t g_rng = 0x6C1A93E7u;

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

static lh_ring_t g_ring;

/* ------------------------------------------------------------------------- */

static void test_empty_and_full_are_distinguishable(void) {
  lh_ring_init(&g_ring);
  CHECK(lh_ring_count(&g_ring) == 0, "a fresh ring should be empty");
  CHECK(lh_ring_free_space(&g_ring) == LH_UART_RING_SIZE, "a fresh ring should be all free");

  uint8_t sink;
  CHECK(!lh_ring_pop(&g_ring, &sink), "popping an empty ring must fail");

  /* Fill it to the last byte. With wrapped indices this is the state that is
   * indistinguishable from empty; with free-running ones it is not. */
  for (uint16_t i = 0; i < LH_UART_RING_SIZE; i++) {
    if (!lh_ring_push(&g_ring, (uint8_t)i)) {
      CHECK(0, "push failed at %u with space still free", (unsigned)i);
      return;
    }
  }
  CHECK(lh_ring_count(&g_ring) == LH_UART_RING_SIZE, "a full ring should report full occupancy");
  CHECK(lh_ring_free_space(&g_ring) == 0, "a full ring should report no free space");
  CHECK(g_ring.stat_hwm == LH_UART_RING_SIZE, "hwm should have reached capacity");

  /* Every byte of capacity is usable — no slot is sacrificed to disambiguate. */
  for (uint16_t i = 0; i < LH_UART_RING_SIZE; i++) {
    uint8_t byte = 0;
    if (!lh_ring_pop(&g_ring, &byte) || byte != (uint8_t)i) {
      CHECK(0, "readback mismatch at %u", (unsigned)i);
      return;
    }
  }
  CHECK(lh_ring_count(&g_ring) == 0, "ring should be empty again");
}

static void test_overrun_drops_newest_and_counts(void) {
  lh_ring_init(&g_ring);
  for (uint16_t i = 0; i < LH_UART_RING_SIZE; i++) lh_ring_push(&g_ring, (uint8_t)(i & 0xFF));

  CHECK(!lh_ring_push(&g_ring, 0xEE), "pushing into a full ring must fail");
  CHECK(g_ring.stat_overrun == 1, "stat_overrun should be 1, is %lu",
        (unsigned long)g_ring.stat_overrun);

  /* Drop-newest: the byte already at the head of the queue is untouched. */
  uint8_t first = 0;
  lh_ring_pop(&g_ring, &first);
  CHECK(first == 0, "overrun corrupted data already in the ring");

  /* Bulk overrun accounting is per byte, not per call. */
  lh_ring_init(&g_ring);
  static uint8_t big[LH_UART_RING_SIZE + 100];
  memset(big, 0x5A, sizeof big);
  const uint16_t accepted = lh_ring_push_bytes(&g_ring, big, (uint16_t)sizeof big);
  CHECK(accepted == LH_UART_RING_SIZE, "should accept exactly capacity, accepted %u",
        (unsigned)accepted);
  CHECK(g_ring.stat_overrun == 100, "should count 100 dropped bytes, counted %lu",
        (unsigned long)g_ring.stat_overrun);
}

/**
 * The index wraparound.
 *
 * head and tail are uint16_t, so they roll over at 65536 bytes. If the ring
 * size did not divide that range exactly, the masked index would jump at the
 * rollover and silently corrupt one buffer's worth of data — at 921600 baud,
 * about every 0.7 s, looking for all the world like a bad cable. slip.c-style
 * static assertions catch the misconfiguration; this catches the arithmetic.
 */
static long test_wraparound(long bytes) {
  lh_ring_init(&g_ring);
  uint32_t write_seq = 0;
  uint32_t read_seq = 0;
  long verified = 0;

  while (verified < bytes) {
    /* Uneven burst sizes so the wrap lands mid-burst rather than tidily on a
     * boundary, which is the case that would otherwise never be exercised. */
    const uint16_t burst = (uint16_t)(1u + (next_random() % 300u));
    for (uint16_t i = 0; i < burst; i++) {
      if (!lh_ring_push(&g_ring, (uint8_t)(write_seq & 0xFF))) break;
      write_seq++;
    }

    const uint16_t drain = (uint16_t)(1u + (next_random() % 300u));
    for (uint16_t i = 0; i < drain; i++) {
      uint8_t byte = 0;
      if (!lh_ring_pop(&g_ring, &byte)) break;
      if (byte != (uint8_t)(read_seq & 0xFF)) {
        CHECK(0, "wraparound corruption at byte %lu (expected %02X, got %02X)",
              (unsigned long)read_seq, (unsigned)(read_seq & 0xFF), (unsigned)byte);
        return verified;
      }
      read_seq++;
      verified++;
    }
  }

  /* Bursts are random, so the ring does fill and does drop bytes here. That is
   * not a defect to assert away — a producer outrunning its consumer is the
   * case the overrun path exists for, and it gets exercised for free. What must
   * hold is that a dropped byte costs exactly itself: the reader's sequence
   * stays aligned with the writer's, which is what the check above verifies on
   * every single byte. */
  CHECK(verified >= bytes, "wraparound test verified %ld of %ld bytes", verified, bytes);
  return verified;
}

/**
 * The roadmap's 10 MB flood, in process.
 *
 * The real version pushes 10 MB over a UART from the host and checks for
 * corrupted bytes and overruns. Without a board that is not runnable, but the
 * half that is pure logic is: 10 MB through the ring with the consumer running
 * at a varying rate, every byte verified against a known sequence.
 *
 * Named `.native` so nobody mistakes it for the on-hardware result, which is
 * still owed and reported SKIPPED below.
 */
static long test_flood(long total_bytes) {
  lh_ring_init(&g_ring);

  static uint8_t chunk[512];
  static uint8_t drained[512];
  uint32_t write_seq = 0;
  uint32_t read_seq = 0;
  long errors = 0;

  while ((long)write_seq < total_bytes) {
    const uint16_t burst = (uint16_t)(1u + (next_random() % sizeof chunk));
    for (uint16_t i = 0; i < burst; i++) chunk[i] = (uint8_t)((write_seq + i) * 31u + 7u);

    const uint16_t accepted = lh_ring_push_bytes(&g_ring, chunk, burst);
    write_seq += accepted;

    /* Consumer runs a little faster than the producer on average, which is the
     * regime the real link has to be in; occasionally it stalls entirely, which
     * is the NVS-write case that makes stat_hwm interesting. */
    const uint16_t want = (next_random() % 8u) == 0u ? 0u : (uint16_t)(1u + next_random() % 600u);
    const uint16_t got = lh_ring_pop_bytes(&g_ring, drained, want > sizeof drained
                                                                 ? (uint16_t)sizeof drained
                                                                 : want);
    for (uint16_t i = 0; i < got; i++) {
      if (drained[i] != (uint8_t)((read_seq + i) * 31u + 7u)) errors++;
    }
    read_seq += got;
  }

  /* Drain the tail. */
  for (;;) {
    const uint16_t got = lh_ring_pop_bytes(&g_ring, drained, (uint16_t)sizeof drained);
    if (got == 0) break;
    for (uint16_t i = 0; i < got; i++) {
      if (drained[i] != (uint8_t)((read_seq + i) * 31u + 7u)) errors++;
    }
    read_seq += got;
  }

  CHECK(errors == 0, "flood test saw %ld corrupted bytes", errors);
  CHECK((long)read_seq == (long)write_seq, "flood test lost bytes: wrote %lu, read %lu",
        (unsigned long)write_seq, (unsigned long)read_seq);
  return errors;
}

/* ------------------------------------------------------------------------- */

#define ROUNDS 40
#define BATCH 4096

static volatile uint32_t g_sink = 0;

static int compare_double(const void *a, const void *b) {
  const double lhs = *(const double *)a;
  const double rhs = *(const double *)b;
  return (lhs > rhs) - (lhs < rhs);
}

/** ns per byte for a push/pop pair, p50 across rounds. */
static double bench_byte_at_a_time(void) {
  double samples[ROUNDS];
  uint8_t byte = 0;

  lh_ring_init(&g_ring);
  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      lh_ring_push(&g_ring, (uint8_t)i);
      lh_ring_pop(&g_ring, &byte);
    }
    samples[round] = (now_seconds() - started) * 1e9 / BATCH;
    g_sink += byte;
  }
  qsort(samples, ROUNDS, sizeof(double), compare_double);
  return samples[ROUNDS / 2];
}

/** MB/s for the bulk path, which is what an ISR draining a FIFO actually uses. */
static double bench_bulk(void) {
  static uint8_t src[512];
  static uint8_t dst[512];
  double samples[ROUNDS];

  lh_ring_init(&g_ring);
  memset(src, 0xA5, sizeof src);

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      lh_ring_push_bytes(&g_ring, src, (uint16_t)sizeof src);
      g_sink += lh_ring_pop_bytes(&g_ring, dst, (uint16_t)sizeof dst);
    }
    const double elapsed = now_seconds() - started;
    samples[round] = ((double)BATCH * (double)sizeof src) / elapsed / 1e6;
  }
  qsort(samples, ROUNDS, sizeof(double), compare_double);
  return samples[ROUNDS / 2];
}

/* ------------------------------------------------------------------------- */

int main(void) {
#if defined(LH_SANITIZED)
  printf("LH_ENV ring.selftest.build=sanitized\n");
#else
  printf("LH_ENV ring.selftest.build=plain\n");
#endif

  test_empty_and_full_are_distinguishable();
  test_overrun_drops_newest_and_counts();

  /* Four times the uint16_t range, so the rollover is crossed repeatedly rather
   * than once by luck. */
  const long wrap_bytes = 4L * 65536L;
  const long wrapped = test_wraparound(wrap_bytes);

  const long flood_bytes = 10L * 1024L * 1024L;
  const long flood_errors = test_flood(flood_bytes);
  const uint16_t flood_hwm = g_ring.stat_hwm;
  const uint32_t flood_overruns = g_ring.stat_overrun;

  printf("LH_METRIC test.ring.checks value=%d unit=count\n", g_checks);
  printf("LH_METRIC test.ring.failures value=%d unit=count budget=0\n", g_failures);
  /* Budgets on these are floors, not ceilings — doing fewer verifications is
   * the regression. The gate knows they are higher-is-better. */
  printf("LH_METRIC test.ring.wraparound_bytes value=%ld unit=count budget=%ld\n", wrapped,
         wrap_bytes);
  printf("LH_METRIC test.ring.flood_10MB.native.errors value=%ld unit=count budget=0\n",
         flood_errors);
  printf("LH_METRIC test.ring.flood_10MB.native.overruns value=%lu unit=count"
         " (deliberate: the consumer stalls, so the overrun path is exercised)\n",
         (unsigned long)flood_overruns);
  printf("LH_METRIC mem.ring.struct value=%u unit=B budget=%u\n", (unsigned)sizeof(lh_ring_t),
         (unsigned)(LH_UART_RING_SIZE + 16u));
  printf("LH_METRIC mem.ring.hwm_pct.native value=%.1f unit=pct\n",
         100.0 * (double)flood_hwm / (double)LH_UART_RING_SIZE);

  printf("LH_METRIC bench.ring.push_pop.native value=%.2f unit=ns\n", bench_byte_at_a_time());
  /* An in-cache upper bound, not a link figure: 512 B stays resident in L1, so
   * this says "the ring is nowhere near being the bottleneck" and nothing more.
   * The number that matters for the link is bench.uart.tput.sustained, and that
   * one needs a board. */
  printf("LH_METRIC bench.ring.tput.bulk_l1.native value=%.1f unit=MB/s\n", bench_bulk());

  /*
   * The on-hardware half of T1.2. These are the numbers the roadmap budgets,
   * and none of them can be produced without a board: sustained UART
   * throughput, the ring's high-water mark under a real 921600-baud load, the
   * task's stack high-water mark, and the proof that the driver path allocates
   * nothing. Emitted as SKIPPED so the budget stays attached to a measurement
   * that has not happened, rather than being quietly satisfied by a host number
   * that means something else.
   */
  printf("LH_METRIC bench.uart.tput.sustained value=SKIPPED unit=kB/s budget=85"
         " (no target hardware attached)\n");
  printf("LH_METRIC mem.ring.hwm_pct value=SKIPPED unit=pct budget=50"
         " (needs a real 921600 baud load)\n");
  printf("LH_METRIC stack.hwm.uart_task value=SKIPPED unit=B budget=2048"
         " (no target hardware attached)\n");
  printf("LH_METRIC test.flood_10MB.errors value=SKIPPED unit=count budget=0"
         " (needs a host-to-bridge UART)\n");

  if (g_sink == 0xFFFFFFFFu) printf("unreachable %u\n", g_sink);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
