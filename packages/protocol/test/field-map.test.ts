import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { RULE_FIELD_MAP, RuleOp, ruleFromWireMap, ruleToWireMap, type Rule } from '../src/field-map.js';

function packageRoot(): string {
  let dir = dirname(fileURLToPath(import.meta.url));
  while (!existsSync(join(dir, 'package.json'))) {
    const parent = dirname(dir);
    if (parent === dir) throw new Error('package root not found');
    dir = parent;
  }
  return dir;
}

const RULE: Rule = {
  src_sensor_id: 10,
  op: RuleOp.GT,
  threshold: 25.5,
  hysteresis: 0.5,
  debounce_ms: 5000,
  action_id: 3,
  action_param: 1,
};

test('the integer keys are frozen — they are a wire contract', () => {
  // These numbers go on air and are burned into deployed nodes. Renumbering a
  // field silently reinterprets every field after it, so the exact mapping is
  // asserted here rather than merely round-tripped: a round-trip test passes
  // happily even if every key changes at once.
  assert.deepEqual(RULE_FIELD_MAP, {
    src_sensor_id: 1,
    op: 2,
    threshold: 3,
    hysteresis: 4,
    debounce_ms: 5,
    action_id: 6,
    action_param: 7,
  });
});

test('the operator enum values are frozen too', () => {
  assert.deepEqual(
    { GT: RuleOp.GT, LT: RuleOp.LT, GTE: RuleOp.GTE, LTE: RuleOp.LTE, EQ: RuleOp.EQ, NEQ: RuleOp.NEQ },
    { GT: 0, LT: 1, GTE: 2, LTE: 3, EQ: 4, NEQ: 5 },
  );
});

test('ruleToWireMap emits integer keys only', () => {
  const wire = ruleToWireMap(RULE);
  for (const key of wire.keys()) {
    assert.equal(typeof key, 'number', 'a string key would blow the CBOR size budget');
    assert.ok(Number.isInteger(key));
  }
  assert.deepEqual([...wire.keys()].sort((a, b) => a - b), [1, 2, 3, 4, 5, 6, 7]);
});

test('ruleToWireMap places each value under its declared key', () => {
  const wire = ruleToWireMap(RULE);
  assert.equal(wire.get(RULE_FIELD_MAP.src_sensor_id), 10);
  assert.equal(wire.get(RULE_FIELD_MAP.threshold), 25.5);
  assert.equal(wire.get(RULE_FIELD_MAP.debounce_ms), 5000);
});

test('ruleToWireMap -> ruleFromWireMap round-trips', () => {
  assert.deepEqual(ruleFromWireMap(ruleToWireMap(RULE)), RULE);
});

test('unknown keys are ignored rather than throwing', () => {
  // Forward compatibility: an older node must survive a config carrying fields
  // it has never heard of, not refuse the whole rule.
  const wire = ruleToWireMap(RULE);
  wire.set(99, 12345);
  assert.deepEqual(ruleFromWireMap(wire), RULE);
});

test('matches the committed rule.json golden fixture', () => {
  const fixture = JSON.parse(
    readFileSync(join(packageRoot(), 'test', 'fixtures', 'rule.json'), 'utf8'),
  ) as { wire_map: Record<string, number> };

  const wire = ruleToWireMap(RULE);
  const asObject = Object.fromEntries([...wire].map(([key, value]) => [String(key), value]));
  assert.deepEqual(asObject, fixture.wire_map);
});
