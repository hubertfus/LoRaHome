#include <unity.h>
#include "lorahome/rule_evaluator.h"

// Same scenarios as packages/protocol/test/rule-evaluator.test.ts and
// packages/host/test/rule-engine.test.ts — the whole point is that all
// three implementations agree on when a rule fires.

static lorahome_rule_t make_rule() {
  lorahome_rule_t rule;
  rule.src_sensor_id = 10;
  rule.op = LORAHOME_OP_GT;
  rule.threshold = 25.0f;
  rule.hysteresis = 2.0f;
  rule.debounce_ms = 1000;
  rule.action_id = 3;
  rule.action_param = 1;
  return rule;
}

void setUp(void) {}
void tearDown(void) {}

void test_does_not_fire_before_debounce_elapses(void) {
  lorahome_rule_t rule = make_rule();
  lorahome_rule_state_t state;
  lorahome_rule_state_init(&state);

  TEST_ASSERT_FALSE(lorahome_rule_step(&state, &rule, 30.0f, 0));
  TEST_ASSERT_FALSE(lorahome_rule_step(&state, &rule, 30.0f, 500));
}

void test_fires_once_debounce_holds_continuously(void) {
  lorahome_rule_t rule = make_rule();
  lorahome_rule_state_t state;
  lorahome_rule_state_init(&state);

  TEST_ASSERT_FALSE(lorahome_rule_step(&state, &rule, 30.0f, 0));
  TEST_ASSERT_TRUE(lorahome_rule_step(&state, &rule, 30.0f, 1000));
}

void test_does_not_refire_until_hysteresis_band_cleared(void) {
  lorahome_rule_t rule = make_rule();
  lorahome_rule_state_t state;
  lorahome_rule_state_init(&state);

  lorahome_rule_step(&state, &rule, 30.0f, 0);
  TEST_ASSERT_TRUE(lorahome_rule_step(&state, &rule, 30.0f, 1000));
  TEST_ASSERT_FALSE(lorahome_rule_step(&state, &rule, 30.0f, 2000));
  // threshold=25, hysteresis=2 -> needs value < 23 to re-arm
  TEST_ASSERT_FALSE(lorahome_rule_step(&state, &rule, 24.0f, 3000));
  lorahome_rule_step(&state, &rule, 20.0f, 4000); // clears the band, re-arms
  lorahome_rule_step(&state, &rule, 30.0f, 5000); // condition true again, debounce restarts
  TEST_ASSERT_TRUE(lorahome_rule_step(&state, &rule, 30.0f, 6000));
}

void test_resets_debounce_timer_if_condition_drops(void) {
  lorahome_rule_t rule = make_rule();
  lorahome_rule_state_t state;
  lorahome_rule_state_init(&state);

  lorahome_rule_step(&state, &rule, 30.0f, 0);
  lorahome_rule_step(&state, &rule, 20.0f, 400); // condition drops, timer resets
  lorahome_rule_step(&state, &rule, 30.0f, 500); // condition true again, restarts at t=500
  TEST_ASSERT_FALSE(lorahome_rule_step(&state, &rule, 30.0f, 1400)); // only 900ms held
  TEST_ASSERT_TRUE(lorahome_rule_step(&state, &rule, 30.0f, 1500)); // 1000ms held since t=500
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_does_not_fire_before_debounce_elapses);
  RUN_TEST(test_fires_once_debounce_holds_continuously);
  RUN_TEST(test_does_not_refire_until_hysteresis_band_cleared);
  RUN_TEST(test_resets_debounce_timer_if_condition_drops);
  return UNITY_END();
}
