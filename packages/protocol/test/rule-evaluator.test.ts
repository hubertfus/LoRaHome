import assert from 'node:assert/strict';
import { test } from 'node:test';
import { RuleOp } from '../src/field-map.js';
import { createRuleState, stepRule } from '../src/rule-evaluator.js';

const RULE = { op: RuleOp.GT, threshold: 25, hysteresis: 2, debounce_ms: 1000 };

test('fires exactly once per arm cycle once debounce elapses', () => {
  const state = createRuleState();
  assert.equal(stepRule(state, RULE, 30, 0), false);
  assert.equal(stepRule(state, RULE, 30, 1000), true);
  assert.equal(stepRule(state, RULE, 30, 2000), false); // already fired, still armed-off
});

test('requires the value to clear the hysteresis band before re-arming', () => {
  const state = createRuleState();
  stepRule(state, RULE, 30, 0);
  stepRule(state, RULE, 30, 1000); // fires
  assert.equal(stepRule(state, RULE, 24, 1500), false); // inside band (threshold-hysteresis=23), no re-arm
  assert.equal(stepRule(state, RULE, 20, 2000), false); // clears band, re-arms, but condition (20>25) false
  assert.equal(stepRule(state, RULE, 30, 2500), false); // condition true again, debounce starts
  assert.equal(stepRule(state, RULE, 30, 3500), true); // held for 1000ms
});
