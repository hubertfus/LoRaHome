import assert from 'node:assert/strict';
import { test } from 'node:test';
import { decode } from 'cbor-x';
import { Rule, RuleOp } from '@lorahome/protocol';
import { compileRulesToCbor } from '../src/compiler/index.js';

test('compileRulesToCbor round-trips through CBOR with integer keys', () => {
  const rules: Rule[] = [
    {
      src_sensor_id: 10,
      op: RuleOp.GT,
      threshold: 25.5,
      hysteresis: 0.5,
      debounce_ms: 5000,
      action_id: 3,
      action_param: 1,
    },
  ];

  const wire = compileRulesToCbor(rules);
  const decoded = decode(wire) as Map<number, number>[];

  assert.equal(decoded.length, 1);
  const keys = [...decoded[0].keys()].sort((a, b) => a - b);
  assert.deepEqual(keys, [1, 2, 3, 4, 5, 6, 7]);
  assert.equal(decoded[0].get(1), 10); // src_sensor_id
  assert.equal(decoded[0].get(3), 25.5); // threshold

  // Every key must be a number — a string key here would mean the payload
  // silently blew the CBOR-integer-key contract (ARCHITECTURE.md §5).
  for (const key of decoded[0].keys()) {
    assert.equal(typeof key, 'number');
  }
});
