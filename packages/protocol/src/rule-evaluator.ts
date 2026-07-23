import { Rule, RuleOp } from './field-map.js';

/**
 * Pure hysteresis/debounce state machine shared by the host's cross-device
 * rule engine and the browser's dry-run simulator. The firmware's local
 * evaluator (C) mirrors this same semantics — see ARCHITECTURE.md §6 — but
 * is implemented separately since it must run with zero dynamic allocation.
 */

export function compareOp(op: RuleOp, value: number, threshold: number): boolean {
  switch (op) {
    case RuleOp.GT:
      return value > threshold;
    case RuleOp.LT:
      return value < threshold;
    case RuleOp.GTE:
      return value >= threshold;
    case RuleOp.LTE:
      return value <= threshold;
    case RuleOp.EQ:
      return value === threshold;
    case RuleOp.NEQ:
      return value !== threshold;
  }
}

/** True once the value has crossed back past the hysteresis band, allowing the rule to re-arm. */
export function isReset(op: RuleOp, value: number, threshold: number, hysteresis: number): boolean {
  switch (op) {
    case RuleOp.GT:
    case RuleOp.GTE:
      return value < threshold - hysteresis;
    case RuleOp.LT:
    case RuleOp.LTE:
      return value > threshold + hysteresis;
    case RuleOp.EQ:
    case RuleOp.NEQ:
      return !compareOp(op, value, threshold);
  }
}

export interface RuleEvaluatorState {
  armed: boolean; // true = condition not yet fired since the last reset
  conditionSinceMs: number | null; // when the condition started holding continuously, for debounce
  fired: boolean; // true once the action has fired for the current arm cycle
}

export function createRuleState(): RuleEvaluatorState {
  return { armed: true, conditionSinceMs: null, fired: false };
}

/**
 * Feeds one new reading through a single rule's state machine. Returns true
 * exactly once per arm cycle, the moment the condition has held continuously
 * for `debounce_ms`. Mutates `state` in place.
 */
export function stepRule(
  state: RuleEvaluatorState,
  rule: Pick<Rule, 'op' | 'threshold' | 'hysteresis' | 'debounce_ms'>,
  value: number,
  atMs: number,
): boolean {
  if (!state.armed) {
    if (isReset(rule.op, value, rule.threshold, rule.hysteresis)) {
      state.armed = true;
      state.fired = false;
      state.conditionSinceMs = null;
    }
    return false;
  }

  const holds = compareOp(rule.op, value, rule.threshold);
  if (!holds) {
    state.conditionSinceMs = null;
    return false;
  }

  if (state.conditionSinceMs === null) {
    state.conditionSinceMs = atMs;
  }

  const held = atMs - state.conditionSinceMs;
  if (!state.fired && held >= rule.debounce_ms) {
    state.fired = true;
    state.armed = false;
    return true;
  }
  return false;
}
