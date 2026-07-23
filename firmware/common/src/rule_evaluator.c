#include "lorahome/rule_evaluator.h"

static bool compare_op(lorahome_rule_op_t op, float value, float threshold) {
  switch (op) {
    case LORAHOME_OP_GT:
      return value > threshold;
    case LORAHOME_OP_LT:
      return value < threshold;
    case LORAHOME_OP_GTE:
      return value >= threshold;
    case LORAHOME_OP_LTE:
      return value <= threshold;
    case LORAHOME_OP_EQ:
      return value == threshold;
    case LORAHOME_OP_NEQ:
      return value != threshold;
    default:
      return false;
  }
}

/* True once the value has crossed back past the hysteresis band, allowing the rule to re-arm. */
static bool is_reset(lorahome_rule_op_t op, float value, float threshold, float hysteresis) {
  switch (op) {
    case LORAHOME_OP_GT:
    case LORAHOME_OP_GTE:
      return value < threshold - hysteresis;
    case LORAHOME_OP_LT:
    case LORAHOME_OP_LTE:
      return value > threshold + hysteresis;
    case LORAHOME_OP_EQ:
    case LORAHOME_OP_NEQ:
      return !compare_op(op, value, threshold);
    default:
      return true;
  }
}

void lorahome_rule_state_init(lorahome_rule_state_t* state) {
  state->armed = true;
  state->has_condition_since = false;
  state->condition_since_ms = 0;
  state->fired = false;
}

bool lorahome_rule_step(lorahome_rule_state_t* state, const lorahome_rule_t* rule, float value, uint32_t at_ms) {
  const lorahome_rule_op_t op = (lorahome_rule_op_t)rule->op;

  if (!state->armed) {
    if (is_reset(op, value, rule->threshold, rule->hysteresis)) {
      state->armed = true;
      state->fired = false;
      state->has_condition_since = false;
    }
    return false;
  }

  const bool holds = compare_op(op, value, rule->threshold);
  if (!holds) {
    state->has_condition_since = false;
    return false;
  }

  if (!state->has_condition_since) {
    state->condition_since_ms = at_ms;
    state->has_condition_since = true;
  }

  const uint32_t held_ms = at_ms - state->condition_since_ms;
  if (!state->fired && held_ms >= rule->debounce_ms) {
    state->fired = true;
    state->armed = false;
    return true;
  }
  return false;
}
