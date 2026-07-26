import assert from 'node:assert/strict';
import { test } from 'node:test';
import { deliveryPct, runChaos } from '../src/reliability/chaos.js';
import { LINK_PROFILES, type LinkProfileName } from '../src/transport/sim-link.js';

/**
 * The reliability suite as a unit test (T2.6).
 *
 * Two thousand frames per profile rather than the hundred thousand the
 * benchmark run uses. The invariant does not care about the count — a single
 * double-processed frame is a failure at any scale — and a suite that takes a
 * minute is a suite somebody eventually moves to "nightly", where it stops
 * catching anything before merge.
 *
 * The full 100k × 5 run lives in bench/chaos.bench.ts and reports the delivery
 * rates that go in the commit message.
 */

const PROFILES = Object.keys(LINK_PROFILES) as LinkProfileName[];

for (const profile of PROFILES) {
  test(`chaos ${profile}: no frame is ever processed twice`, () => {
    const result = runChaos({ profile, frames: 2000, seed: 0x8f2a });

    // THE invariant. Not "few", not "within tolerance" — none.
    assert.equal(
      result.doubleProcessed,
      0,
      `${result.doubleProcessed} frames processed twice on ${profile} (seed 0x8f2a)`,
    );
  });
}

test('a clean link delivers everything and never retries', () => {
  const result = runChaos({ profile: 'clean', frames: 2000, seed: 0x1234 });

  assert.equal(deliveryPct(result), 100);
  assert.equal(result.retries, 0, 'a clean link that retries means a timing bug, not a lossy link');
  assert.equal(result.giveUps, 0);
  assert.equal(result.crcRejects, 0);
});

test('loss is recovered by retransmission, not tolerated', () => {
  const result = runChaos({ profile: 'loss30', frames: 2000, seed: 0x8f2a });

  assert.ok(result.retries > 0, 'a 30% loss link with no retransmissions is a broken harness');
  assert.ok(
    deliveryPct(result) > 97,
    `delivery was ${deliveryPct(result).toFixed(3)}% — the ARQ is not recovering`,
  );
});

test('corruption is caught by the CRC, never processed', () => {
  const result = runChaos({ profile: 'corrupt', frames: 2000, seed: 0x51a3 });

  assert.ok(result.crcRejects > 0, 'nothing was corrupted — check the profile');
  assert.equal(result.doubleProcessed, 0);
  // Every frame that survives the CRC is genuine, so the retransmissions the
  // rejects cause still deliver everything.
  assert.ok(deliveryPct(result) > 99, `delivery was ${deliveryPct(result).toFixed(3)}%`);
});

test('reordering is absorbed without duplicating work', () => {
  const result = runChaos({ profile: 'reorder', frames: 2000, seed: 0x2b7e });

  assert.equal(result.doubleProcessed, 0);
  assert.equal(deliveryPct(result), 100);
});

test('duplicate arrivals are dropped by the window, not by luck', () => {
  // Every frame delivered twice: what an ARQ produces when ACKs are slow rather
  // than lost. Without the duplicate window this test would report 2000
  // double-processed frames, which is exactly the failure it is here to catch.
  const result = runChaos({
    profile: 'clean',
    frames: 500,
    seed: 0x6c8e,
    overrides: { duplicatePct: 100 },
  });

  assert.equal(result.doubleProcessed, 0);
  assert.equal(result.delivered, 500);
  assert.ok(result.dupesDropped >= 500, `only ${result.dupesDropped} duplicates were refused`);
});

test('the same seed produces the same result, every time', () => {
  // R2.6: a chaos failure has to be reproducible from its seed alone, or it
  // becomes the test people re-run until it passes.
  const first = runChaos({ profile: 'loss30', frames: 1000, seed: 0x8f2a });
  const second = runChaos({ profile: 'loss30', frames: 1000, seed: 0x8f2a });

  assert.equal(first.delivered, second.delivered);
  assert.equal(first.retries, second.retries);
  assert.equal(first.crcRejects, second.crcRejects);
  assert.equal(first.giveUps, second.giveUps);
});
