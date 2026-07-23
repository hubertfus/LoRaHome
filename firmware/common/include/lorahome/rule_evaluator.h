#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Rule record and hysteresis/debounce state machine. Mirrors
 * packages/protocol/src/rule-evaluator.ts exactly (same semantics, same
 * field layout as ARCHITECTURE.md §6) — the local Node evaluator and the
 * Host's global rule engine must never disagree about when a rule fires.
 *
 * No dynamic allocation: rule_t and rule_state_t are meant to live in
 * statically-sized arrays (see CONTRIBUTING.md §1.1, MAX_RULES = 16).
 */

typedef enum {
  LORAHOME_OP_GT = 0,
  LORAHOME_OP_LT = 1,
  LORAHOME_OP_GTE = 2,
  LORAHOME_OP_LTE = 3,
  LORAHOME_OP_EQ = 4,
  LORAHOME_OP_NEQ = 5,
} lorahome_rule_op_t;

typedef struct {
  uint16_t src_sensor_id;
  uint8_t op; /* lorahome_rule_op_t */
  float threshold;
  float hysteresis;
  uint32_t debounce_ms;
  uint16_t action_id;
  int32_t action_param;
} lorahome_rule_t;

typedef struct {
  bool armed;                  /* condition not yet fired since the last reset */
  bool has_condition_since;    /* whether condition_since_ms is meaningful */
  uint32_t condition_since_ms; /* when the condition started holding continuously */
  bool fired;                  /* action already fired for the current arm cycle */
} lorahome_rule_state_t;

void lorahome_rule_state_init(lorahome_rule_state_t* state);

/**
 * Feeds one new reading through a single rule's state machine. Returns true
 * exactly once per arm cycle, the moment the condition has held continuously
 * for rule->debounce_ms. Mutates *state in place.
 */
bool lorahome_rule_step(lorahome_rule_state_t* state, const lorahome_rule_t* rule, float value, uint32_t at_ms);

#ifdef __cplusplus
}
#endif
