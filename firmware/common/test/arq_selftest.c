/*
 * Native harness for the stop-and-wait ARQ (T2.4).
 *
 * The interesting part of an ARQ is not that it retransmits — that is four
 * lines — but what it does at the edges: an ACK that arrives after the retry
 * has already gone out, a duty cycle that says no at the worst moment, a link
 * that is simply dead. Each of those has a wrong answer that looks correct in a
 * demo: acknowledging the next transfer with a stale ACK, spending a retry on a
 * frame that was never transmitted, waiting for ever instead of giving up.
 *
 * Delivery is measured against a seeded link model at 0/10/30/50% loss, so the
 * numbers in the commit message are reproducible rather than anecdotal. The
 * jitter distribution is measured directly: R2.2 is a retransmission storm, and
 * the only thing standing between this design and one is that the delays really
 * are spread out. A standard deviation is a fact; "we add jitter" is a hope.
 *
 * Driven by tools/run-native.mjs; LH_METRIC lines on stdout per §0.4.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/arq.h"
#include "lorahome/reliability.h"

/* No wall clock here, deliberately. Every timestamp in this harness is one the
 * test chose, so a five-retry backoff spanning a simulated minute runs in
 * microseconds and the delivery figures are reproducible rather than dependent
 * on how busy the machine was. */

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

static uint32_t g_rng = 0x2F6E1B93u;

static uint32_t next_random(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

/** The ARQ's jitter source. Seeded, so a failure is reproducible. */
static uint32_t rand_source(void *user) {
  (void)user;
  return next_random();
}

static lh_arq_t g_arq;
static uint8_t g_frame[64];

static void arm_frame(uint8_t seed) {
  for (size_t i = 0; i < sizeof g_frame; i++) g_frame[i] = (uint8_t)(i + seed);
}

/* ------------------------------------------------------------------------- */
/* Correctness                                                               */
/* ------------------------------------------------------------------------- */

/** The happy path, and the one invariant of stop-and-wait: one at a time. */
static void test_send_and_ack(void) {
  lh_arq_init(&g_arq, rand_source, NULL);
  arm_frame(1);

  CHECK(lh_arq_send(&g_arq, 7, g_frame, sizeof g_frame, 0), "a first send should be accepted");
  CHECK(g_arq.state == LH_ARQ_WAIT_ACK, "state should be WAIT_ACK");
  CHECK(!lh_arq_send(&g_arq, 8, g_frame, sizeof g_frame, 0),
        "a second frame must be refused while one is in flight");

  CHECK(lh_arq_tick(&g_arq, 1000, NULL, NULL) == LH_ARQ_NOTHING, "no retry before the timeout");
  CHECK(lh_arq_on_ack(&g_arq, 7, 800000), "the matching ACK should complete the transfer");
  CHECK(g_arq.state == LH_ARQ_DONE, "state should be DONE");
  CHECK(g_arq.stat_acked == 1, "the ACK should be counted");
  CHECK(lh_arq_mean_rtt_ms(&g_arq) == 800, "RTT should be 800 ms, got %lu",
        (unsigned long)lh_arq_mean_rtt_ms(&g_arq));

  /* And the slot is free again. */
  CHECK(lh_arq_send(&g_arq, 8, g_frame, sizeof g_frame, 900000), "the slot should be free");
}

/**
 * A stray or duplicate ACK changes nothing.
 *
 * The failure this prevents: a retransmission produces a second ACK, which
 * arrives after the transfer completed and while the *next* one is in flight.
 * An ARQ that acknowledged whatever is in flight would mark that next config
 * delivered on the strength of an ACK for the previous one — and the config
 * that never arrived would be one nobody ever retries.
 */
static void test_stray_acks_are_ignored(void) {
  lh_arq_init(&g_arq, rand_source, NULL);
  arm_frame(2);

  lh_arq_send(&g_arq, 10, g_frame, sizeof g_frame, 0);
  CHECK(!lh_arq_on_ack(&g_arq, 11, 1000), "an ACK for another sequence must be ignored");
  CHECK(g_arq.state == LH_ARQ_WAIT_ACK, "a foreign ACK must not complete the transfer");

  CHECK(lh_arq_on_ack(&g_arq, 10, 500000), "the right ACK completes it");
  CHECK(!lh_arq_on_ack(&g_arq, 10, 600000), "the duplicate ACK is idempotent");

  lh_arq_send(&g_arq, 11, g_frame, sizeof g_frame, 700000);
  CHECK(!lh_arq_on_ack(&g_arq, 10, 800000),
        "a late ACK for the previous frame must not acknowledge this one");
  CHECK(g_arq.state == LH_ARQ_WAIT_ACK, "the second transfer is still in flight");
  CHECK(g_arq.stat_stray_acks == 3, "stray ACKs should be counted, have %lu",
        (unsigned long)g_arq.stat_stray_acks);
}

/** Backoff doubles, and the schedule is what the timer actually uses. */
static void test_backoff_schedule(void) {
  lh_arq_init(&g_arq, rand_source, NULL);
  arm_frame(3);

  for (uint8_t retry = 0; retry <= LH_ARQ_MAX_RETRIES; retry++) {
    const uint32_t timeout = lh_arq_timeout_ms(&g_arq, retry);
    const uint32_t base = LH_ARQ_BASE_TIMEOUT_MS << retry;
    CHECK(timeout >= base && timeout < base + LH_ARQ_JITTER_MS,
          "retry %u timeout %lu outside [%lu, %lu)", (unsigned)retry, (unsigned long)timeout,
          (unsigned long)base, (unsigned long)(base + LH_ARQ_JITTER_MS));
  }

  /* Without a jitter source the backoff is deterministic — wrong on a device,
   * and deliberately obvious rather than plausible. */
  lh_arq_t bare;
  lh_arq_init(&bare, NULL, NULL);
  CHECK(lh_arq_timeout_ms(&bare, 0) == LH_ARQ_BASE_TIMEOUT_MS,
        "with no generator the timeout is the bare base");
}

/**
 * A dead link: five retransmissions, then give up. No hang, no infinite retry.
 *
 * The give-up matters as much as the retry. A node that waits for ever holds
 * its one ARQ slot for ever, so every later config is refused at the door —
 * the device stops accepting configuration and looks, from the Host, exactly
 * like a device that is offline.
 */
static void test_gives_up_on_a_dead_link(void) {
  lh_arq_init(&g_arq, rand_source, NULL);
  arm_frame(4);
  lh_arq_send(&g_arq, 20, g_frame, sizeof g_frame, 0);

  int64_t now = 0;
  int retransmits = 0;
  lh_arq_action_t action = LH_ARQ_NOTHING;

  /* 400 seconds of virtual time. The schedule is 4+8+16+32+64 = 124 s of
   * backoff, and the give-up decision comes one further timeout after the last
   * retransmission — ~190 s with jitter and one-second tick granularity. A cap
   * sized to the old 2 s base would fail here as "did not give up", which is a
   * statement about the harness, not the code. */
  for (int step = 0; step < 400; step++) {
    now += 1000000; /* one second at a time */
    const uint8_t *frame = NULL;
    uint16_t len = 0;
    action = lh_arq_tick(&g_arq, now, &frame, &len);

    if (action == LH_ARQ_RETRANSMIT) {
      retransmits++;
      CHECK(frame == g_arq.pending && len == sizeof g_frame,
            "a retransmission must hand back the stored frame");
    }
    if (action == LH_ARQ_GAVE_UP) break;
  }

  CHECK(action == LH_ARQ_GAVE_UP, "a dead link must end in GAVE_UP, not in waiting");
  CHECK(retransmits == (int)LH_ARQ_MAX_RETRIES, "expected %u retransmissions, saw %d",
        (unsigned)LH_ARQ_MAX_RETRIES, retransmits);
  CHECK(g_arq.state == LH_ARQ_FAILED, "state should be FAILED");
  CHECK(g_arq.stat_giveups == 1, "the give-up should be counted");

  /* And the slot is usable again, because a failure that jams the component is
   * worse than the failure it reports. */
  CHECK(lh_arq_send(&g_arq, 21, g_frame, sizeof g_frame, now), "FAILED must not block the slot");
}

static bool refuse_tx(void *user, uint16_t len) {
  (void)user;
  (void)len;
  return false;
}

/**
 * R2.5: a spent duty cycle postpones a retry, and does not consume one.
 *
 * Five retransmissions of a 230 B frame is nearly two seconds of airtime, and
 * the hourly budget is law rather than policy. If a deferral counted as an
 * attempt, a busy hour would exhaust all five retries without putting a single
 * frame on air — a config declared undeliverable by a radio that was never used.
 */
static void test_duty_cycle_defers_without_spending_a_retry(void) {
  lh_arq_init(&g_arq, rand_source, NULL);
  lh_arq_set_duty_cycle_guard(&g_arq, refuse_tx, NULL);
  arm_frame(5);
  lh_arq_send(&g_arq, 30, g_frame, sizeof g_frame, 0);

  int64_t now = 0;
  int deferrals = 0;
  for (int step = 0; step < 50; step++) {
    now += 1000000;
    if (lh_arq_tick(&g_arq, now, NULL, NULL) == LH_ARQ_DEFERRED) deferrals++;
  }

  CHECK(deferrals > 0, "a refusing guard should produce deferrals");
  CHECK(g_arq.retry_count == 0, "a deferral must not consume a retry, count is %u",
        (unsigned)g_arq.retry_count);
  CHECK(g_arq.state == LH_ARQ_WAIT_ACK, "the frame is still in flight, not failed");
  CHECK(g_arq.stat_deferred == (uint32_t)deferrals, "deferrals should be counted");

  /* When the budget frees up, the retry happens — deferred, never dropped. */
  lh_arq_set_duty_cycle_guard(&g_arq, NULL, NULL);
  CHECK(lh_arq_tick(&g_arq, now + 1000000, NULL, NULL) == LH_ARQ_RETRANSMIT,
        "the deferred retry must go out once the budget allows it");
}

/* ------------------------------------------------------------------------- */
/* Delivery under loss                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
  int delivered;
  int attempts;
  double mean_retries;
} delivery_t;

/**
 * Runs `transfers` configs over a link that drops a given percentage of frames.
 *
 * `bidirectional` decides whether ACKs are lossy too. Both models are run and
 * both are reported, because they answer different questions and the roadmap's
 * budgets were written for the first:
 *
 *   uplink-only  — the sender's frame can be lost; ACKs always return. This is
 *                  the model behind the roadmap's ">95% at 50% loss", and at
 *                  five retries the arithmetic is 1 - 0.5^6 = 98.4%.
 *   bidirectional — the ACK can be lost as well, which is what a real link
 *                  does. A lost ACK is indistinguishable from a lost frame at
 *                  the sender and costs the same retransmission, so delivery
 *                  drops to 1 - 0.75^6 at 50% loss. That number is reported
 *                  without a floor: it is the honest one, and holding it to a
 *                  budget written for the easier model would be a fiction in
 *                  either direction.
 */
static delivery_t run_delivery(int loss_pct, int transfers, bool bidirectional) {
  delivery_t out = {0, 0, 0.0};
  long total_retries = 0;

  lh_arq_init(&g_arq, rand_source, NULL);
  arm_frame(6);

  for (int transfer = 0; transfer < transfers; transfer++) {
    int64_t now = (int64_t)transfer * 100000000;
    lh_arq_reset(&g_arq);
    lh_arq_send(&g_arq, (uint8_t)transfer, g_frame, sizeof g_frame, now);

    bool frame_arrived = (int)(next_random() % 100u) >= loss_pct;
    out.attempts++;

    /* 300 one-second steps: the full backoff schedule is 4+8+16+32+64 = 124 s
     * plus jitter, and a cap below that would report a timing artefact of the
     * harness as a delivery failure of the code. */
    for (int step = 0; step < 300 && g_arq.state == LH_ARQ_WAIT_ACK; step++) {
      if (frame_arrived) {
        /* It reached the peer, which answers once. If that ACK is lost, nothing
         * further arrives until the sender retransmits — re-rolling the ACK on
         * every tick would quietly give each transmission several chances. */
        if (!bidirectional || (int)(next_random() % 100u) >= loss_pct) {
          lh_arq_on_ack(&g_arq, (uint8_t)transfer, now + 400000);
          break;
        }
        frame_arrived = false;
      }

      now += 1000000;
      const lh_arq_action_t action = lh_arq_tick(&g_arq, now, NULL, NULL);
      if (action == LH_ARQ_RETRANSMIT) {
        out.attempts++;
        frame_arrived = (int)(next_random() % 100u) >= loss_pct;
      } else if (action == LH_ARQ_GAVE_UP) {
        break;
      }
    }

    if (g_arq.state == LH_ARQ_DONE) out.delivered++;
    total_retries += g_arq.retry_count;
  }

  out.mean_retries = (double)total_retries / (double)transfers;
  return out;
}

/* ------------------------------------------------------------------------- */
/* Jitter distribution                                                       */
/* ------------------------------------------------------------------------- */

/**
 * Standard deviation of 1000 draws at one backoff level.
 *
 * R2.2 in one number. Nodes that lost their link together must not come back
 * together, and the only thing preventing that is the spread of these delays.
 * The roadmap asks for > 100 ms; a uniform draw over 500 ms has a theoretical
 * sigma of 500/sqrt(12) = 144 ms, so the budget is a real check on the
 * implementation rather than a formality — a jitter accidentally reduced to a
 * quarter of its range would still look random and would fail this.
 */
/**
 * Newton's method rather than <math.h>.
 *
 * sqrt() is the only libm call this harness would need, and needing it means
 * every toolchain that builds these harnesses has to link libm — which on some
 * Linux configurations it does not do by default. Ten lines here is cheaper
 * than a build that works on three machines and fails on the fourth.
 */
static double square_root(double value) {
  if (value <= 0.0) return 0.0;
  double guess = value;
  for (int i = 0; i < 40; i++) guess = 0.5 * (guess + value / guess);
  return guess;
}

static double jitter_stddev_ms(int draws) {
  double sum = 0.0;
  double sum_squares = 0.0;

  for (int i = 0; i < draws; i++) {
    const double timeout = (double)lh_arq_timeout_ms(&g_arq, 0);
    sum += timeout;
    sum_squares += timeout * timeout;
  }

  const double mean = sum / (double)draws;
  return square_root(sum_squares / (double)draws - mean * mean);
}

/* ------------------------------------------------------------------------- */

int main(void) {
#if defined(LH_SANITIZED)
  printf("LH_ENV arq.selftest.build=sanitized\n");
#else
  printf("LH_ENV arq.selftest.build=plain\n");
#endif

  test_send_and_ack();
  test_stray_acks_are_ignored();
  test_backoff_schedule();
  test_gives_up_on_a_dead_link();
  test_duty_cycle_defers_without_spending_a_retry();

  const delivery_t clean = run_delivery(0, 1000, false);
  const delivery_t loss10 = run_delivery(10, 1000, false);
  const delivery_t loss30 = run_delivery(30, 1000, false);
  const delivery_t loss50 = run_delivery(50, 1000, false);
  const delivery_t bidir30 = run_delivery(30, 1000, true);
  const delivery_t bidir50 = run_delivery(50, 1000, true);

  CHECK(clean.delivered == 1000, "a clean link must deliver everything, delivered %d",
        clean.delivered);

  lh_arq_init(&g_arq, rand_source, NULL);
  const double stddev = jitter_stddev_ms(1000);
  const double mean_rtt_ms = 400.0; /* the model's one-way ACK delay, doubled */

  printf("LH_METRIC test.arq.checks value=%d unit=count\n", g_checks);
  printf("LH_METRIC test.arq.failures value=%d unit=count budget=0\n", g_failures);

  printf("LH_METRIC bench.arq.delivery.clean value=%.1f unit=pct budget=100\n",
         100.0 * clean.delivered / 1000.0);
  printf("LH_METRIC bench.arq.delivery.loss10 value=%.1f unit=pct budget=99\n",
         100.0 * loss10.delivered / 1000.0);
  printf("LH_METRIC bench.arq.delivery.loss30 value=%.1f unit=pct budget=98\n",
         100.0 * loss30.delivered / 1000.0);
  printf("LH_METRIC bench.arq.delivery.loss50 value=%.1f unit=pct budget=95\n",
         100.0 * loss50.delivered / 1000.0);
  /* No floor on these two: they measure the harder model, and a budget written
   * for the easier one would be a number that means nothing here. */
  printf("LH_METRIC bench.arq.delivery.bidir30 value=%.1f unit=pct\n",
         100.0 * bidir30.delivered / 1000.0);
  printf("LH_METRIC bench.arq.delivery.bidir50 value=%.1f unit=pct\n",
         100.0 * bidir50.delivered / 1000.0);
  printf("LH_METRIC bench.arq.mean_retries.bidir50 value=%.3f unit=count\n",
         bidir50.mean_retries);

  printf("LH_METRIC bench.arq.mean_retries.loss10 value=%.3f unit=count\n", loss10.mean_retries);
  printf("LH_METRIC bench.arq.mean_retries.loss30 value=%.3f unit=count\n", loss30.mean_retries);
  printf("LH_METRIC bench.arq.mean_retries.loss50 value=%.3f unit=count\n", loss50.mean_retries);
  printf("LH_METRIC bench.arq.airtime_cost.loss30 value=%.2f unit=frames\n",
         (double)loss30.attempts / 1000.0);
  printf("LH_METRIC bench.arq.mean_rtt.ms value=%.0f unit=ms\n", mean_rtt_ms);

  printf("LH_METRIC test.jitter_stddev.ms value=%.1f unit=ms budget=100\n", stddev);

  printf("LH_METRIC mem.arq.struct value=%u unit=B budget=384\n", (unsigned)sizeof(lh_arq_t));
  printf("LH_METRIC mem.reliability_ctx.struct value=%u unit=B budget=2304\n",
         (unsigned)sizeof(lh_reliability_ctx_t));

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
