import assert from 'node:assert/strict';
import { test } from 'node:test';
import { computeAirtimeMs, DutyCycleGuard } from '../src/duty-cycle-guard/index.js';

test('computeAirtimeMs increases with payload size and spreading factor', () => {
  const small = computeAirtimeMs({ bytes: 20, spreadingFactor: 7, bandwidthHz: 125000 });
  const large = computeAirtimeMs({ bytes: 200, spreadingFactor: 7, bandwidthHz: 125000 });
  const higherSf = computeAirtimeMs({ bytes: 20, spreadingFactor: 12, bandwidthHz: 125000 });

  assert.ok(large > small);
  assert.ok(higherSf > small);
});

test('DutyCycleGuard blocks transmissions once the 1% budget is used up', () => {
  const guard = new DutyCycleGuard(0.01, 1000); // 1% of a 1s window = 10ms budget
  assert.equal(guard.tryRecord(6, 0), true);
  assert.equal(guard.tryRecord(3, 100), true);
  // used = 9ms; one more of 5ms would push to 14ms > 10ms budget
  assert.equal(guard.tryRecord(5, 200), false);
  assert.equal(guard.usedMs(200), 9);
});

test('DutyCycleGuard frees budget as the window rolls forward', () => {
  const guard = new DutyCycleGuard(0.01, 1000);
  assert.equal(guard.tryRecord(8, 0), true);
  assert.equal(guard.tryRecord(5, 500), false); // still within the same window, would exceed
  assert.equal(guard.tryRecord(8, 1001), true); // first transmission has rolled out of the window
});
