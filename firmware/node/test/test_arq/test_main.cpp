/*
 * On-target ARQ tests (T2.4).
 *
 * The host harness in firmware/common/test/arq_selftest.c measures delivery
 * across loss profiles and the jitter distribution. What is checked here is
 * what only the target can answer: the struct's size under this ABI's alignment
 * rules, and that the state machine behaves the same when `int64_t` arithmetic
 * runs on a 32-bit core.
 *
 * Run with `pio test -e esp32dev -f test_arq`.
 */
#include <unity.h>

#include "lorahome/arq.h"
#include "lorahome/reliability.h"

static lh_arq_t g_arq;
static uint8_t g_frame[64];
static uint32_t g_rand_state;

/** A scripted generator: predictable here, never on a real device. */
static uint32_t test_random(void* user) {
  (void)user;
  g_rand_state = g_rand_state * 1103515245u + 12345u;
  return g_rand_state;
}

void setUp(void) {
  g_rand_state = 1;
  for (size_t i = 0; i < sizeof(g_frame); i++) g_frame[i] = (uint8_t)i;
  lh_arq_init(&g_arq, test_random, nullptr);
}
void tearDown(void) {}

/** The budget that decides whether Etap 2 fits at all, on this ABI. */
void test_reliability_context_fits_budget(void) {
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(2304, (uint32_t)sizeof(lh_reliability_ctx_t));
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(384, (uint32_t)sizeof(lh_arq_t));
}

void test_send_then_ack(void) {
  TEST_ASSERT_TRUE(lh_arq_send(&g_arq, 7, g_frame, sizeof(g_frame), 0));
  TEST_ASSERT_EQUAL_INT(LH_ARQ_WAIT_ACK, g_arq.state);
  TEST_ASSERT_FALSE(lh_arq_send(&g_arq, 8, g_frame, sizeof(g_frame), 0));

  TEST_ASSERT_TRUE(lh_arq_on_ack(&g_arq, 7, 800000));
  TEST_ASSERT_EQUAL_INT(LH_ARQ_DONE, g_arq.state);
  TEST_ASSERT_EQUAL_UINT32(800, lh_arq_mean_rtt_ms(&g_arq));
}

/** A stale ACK must never acknowledge the frame that is in flight now. */
void test_stale_ack_is_ignored(void) {
  lh_arq_send(&g_arq, 10, g_frame, sizeof(g_frame), 0);
  lh_arq_on_ack(&g_arq, 10, 500000);
  lh_arq_send(&g_arq, 11, g_frame, sizeof(g_frame), 600000);

  TEST_ASSERT_FALSE(lh_arq_on_ack(&g_arq, 10, 700000));
  TEST_ASSERT_EQUAL_INT(LH_ARQ_WAIT_ACK, g_arq.state);
  TEST_ASSERT_TRUE(lh_arq_on_ack(&g_arq, 11, 800000));
}

/** Backoff doubles and every timeout carries jitter (R2.2). */
void test_backoff_is_bounded_and_jittered(void) {
  for (uint8_t retry = 0; retry <= LH_ARQ_MAX_RETRIES; retry++) {
    const uint32_t timeout = lh_arq_timeout_ms(&g_arq, retry);
    const uint32_t base = LH_ARQ_BASE_TIMEOUT_MS << retry;
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(base, timeout);
    TEST_ASSERT_LESS_THAN_UINT32(base + LH_ARQ_JITTER_MS, timeout);
  }
}

/** A dead link ends in a give-up, and the slot is usable afterwards. */
void test_gives_up_after_five_retries(void) {
  lh_arq_send(&g_arq, 20, g_frame, sizeof(g_frame), 0);

  int64_t now = 0;
  int retransmits = 0;
  lh_arq_action_t action = LH_ARQ_NOTHING;

  for (int step = 0; step < 200; step++) {
    now += 1000000;
    action = lh_arq_tick(&g_arq, now, nullptr, nullptr);
    if (action == LH_ARQ_RETRANSMIT) retransmits++;
    if (action == LH_ARQ_GAVE_UP) break;
  }

  TEST_ASSERT_EQUAL_INT(LH_ARQ_GAVE_UP, action);
  TEST_ASSERT_EQUAL_INT(LH_ARQ_MAX_RETRIES, retransmits);
  TEST_ASSERT_EQUAL_INT(LH_ARQ_FAILED, g_arq.state);
  TEST_ASSERT_TRUE(lh_arq_send(&g_arq, 21, g_frame, sizeof(g_frame), now));
}

static bool refuse_tx(void* user, uint16_t len) {
  (void)user;
  (void)len;
  return false;
}

/** R2.5: a spent duty cycle postpones a retry and does not spend one. */
void test_duty_cycle_defers_without_spending_a_retry(void) {
  lh_arq_set_duty_cycle_guard(&g_arq, refuse_tx, nullptr);
  lh_arq_send(&g_arq, 30, g_frame, sizeof(g_frame), 0);

  int64_t now = 0;
  for (int step = 0; step < 10; step++) {
    now += 1000000;
    lh_arq_tick(&g_arq, now, nullptr, nullptr);
  }

  TEST_ASSERT_EQUAL_UINT8(0, g_arq.retry_count);
  TEST_ASSERT_EQUAL_INT(LH_ARQ_WAIT_ACK, g_arq.state);
  TEST_ASSERT_GREATER_THAN_UINT32(0, g_arq.stat_deferred);

  lh_arq_set_duty_cycle_guard(&g_arq, nullptr, nullptr);
  TEST_ASSERT_EQUAL_INT(LH_ARQ_RETRANSMIT, lh_arq_tick(&g_arq, now + 1000000, nullptr, nullptr));
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_reliability_context_fits_budget);
  RUN_TEST(test_send_then_ack);
  RUN_TEST(test_stale_ack_is_ignored);
  RUN_TEST(test_backoff_is_bounded_and_jittered);
  RUN_TEST(test_gives_up_after_five_retries);
  RUN_TEST(test_duty_cycle_defers_without_spending_a_retry);
  return UNITY_END();
}
