import assert from 'node:assert/strict';
import { test } from 'node:test';
import { RuleOp } from '@lorahome/protocol';
import { GlobalRuleEngine } from '../src/rule-engine/index.js';

function makeEngine() {
  const engine = new GlobalRuleEngine();
  engine.setRules([
    {
      src_device_id: 1,
      src_sensor_id: 10,
      op: RuleOp.GT,
      threshold: 25,
      hysteresis: 2,
      debounce_ms: 1000,
      action_id: 3,
      action_param: 1,
    },
  ]);
  return engine;
}

test('does not fire before debounce elapses', () => {
  const engine = makeEngine();
  assert.deepEqual(engine.ingest(1, 10, 30, 0), []);
  assert.deepEqual(engine.ingest(1, 10, 30, 500), []);
});

test('fires once debounce holds continuously above threshold', () => {
  const engine = makeEngine();
  engine.ingest(1, 10, 30, 0);
  const fired = engine.ingest(1, 10, 30, 1000);
  assert.equal(fired.length, 1);
  assert.equal(fired[0].value, 30);
});

test('does not re-fire until value drops past the hysteresis band and re-crosses', () => {
  const engine = makeEngine();
  engine.ingest(1, 10, 30, 0);
  assert.equal(engine.ingest(1, 10, 30, 1000).length, 1);
  // still above threshold, already fired: no repeat
  assert.equal(engine.ingest(1, 10, 30, 2000).length, 0);
  // drop only to 24 (inside hysteresis band, threshold=25,hysteresis=2 -> needs <23 to reset)
  assert.equal(engine.ingest(1, 10, 24, 3000).length, 0);
  // drop below 23 to reset
  engine.ingest(1, 10, 20, 4000);
  // cross back above threshold and hold for debounce
  engine.ingest(1, 10, 30, 5000);
  assert.equal(engine.ingest(1, 10, 30, 6000).length, 1);
});

test('resets the debounce timer if the condition drops before it elapses', () => {
  const engine = makeEngine();
  engine.ingest(1, 10, 30, 0); // condition becomes true at t=0
  engine.ingest(1, 10, 20, 400); // condition drops before the 1000ms debounce elapses -> resets
  engine.ingest(1, 10, 30, 500); // condition becomes true again, timer restarts at t=500
  assert.equal(engine.ingest(1, 10, 30, 1400).length, 0); // only 900ms held since the restart
  assert.equal(engine.ingest(1, 10, 30, 1500).length, 1); // 1000ms held since t=500
});
