import assert from 'node:assert/strict';
import { test } from 'node:test';
import { Rule, RuleOp } from '@lorahome/protocol';
import { simulateRule } from '../src/simulator/simulate.js';

const RULE: Rule = {
  src_sensor_id: 10,
  op: RuleOp.GT,
  threshold: 25,
  hysteresis: 2,
  debounce_ms: 1000,
  action_id: 3,
  action_param: 1,
};

test('simulateRule replays a recorded time series and reports the same fire point the field would see', () => {
  const fired = simulateRule(RULE, [
    { atMs: 0, value: 20 },
    { atMs: 500, value: 30 },
    { atMs: 1000, value: 31 },
    { atMs: 1500, value: 29 }, // debounce satisfied at 500+1000=1500
    { atMs: 2000, value: 28 },
  ]);

  assert.equal(fired.length, 1);
  assert.equal(fired[0].atMs, 1500);
});

test('simulateRule reports nothing when the condition never holds long enough', () => {
  const fired = simulateRule(RULE, [
    { atMs: 0, value: 30 },
    { atMs: 400, value: 20 }, // drops before debounce elapses
    { atMs: 800, value: 30 },
  ]);
  assert.deepEqual(fired, []);
});
