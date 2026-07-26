import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  ARQ_BASE_TIMEOUT_MS,
  ARQ_JITTER_MS,
  ARQ_MAX_RETRIES,
  Arq,
  ArqAction,
  ArqState,
} from '../src/arq.js';

/**
 * Tests for the Host's ARQ (T2.4).
 *
 * tools/check-arq-cross.mjs replays a 1100-step script through this and the C
 * twin and requires identical decisions, so the state machine itself is covered
 * there. What is pinned here is the jitter distribution — R2.2 is prevented by
 * a spread, not by an intention — and the handling of stale ACKs, which is the
 * one behaviour whose failure produces a *silent* missing config rather than a
 * visible error.
 */

const frame = Uint8Array.from({ length: 64 }, (_, i) => i);

/** Deterministic jitter source; on a real device this would be esp_random(). */
function seededRandom(seed = 0x2f6e1b93): () => number {
  let state = seed;
  return () => {
    state ^= state << 13;
    state ^= state >>> 17;
    state ^= state << 5;
    return state >>> 0;
  };
}

test('send, tick, ack — and only one frame in flight at a time', () => {
  const arq = new Arq({ random: seededRandom() });

  assert.equal(arq.send(7, frame, 0), true);
  assert.equal(arq.state, ArqState.WAIT_ACK);
  assert.equal(arq.send(8, frame, 0), false, 'stop-and-wait means exactly one');

  assert.equal(arq.tick(1000).action, ArqAction.NOTHING);
  assert.equal(arq.onAck(7, 800_000), true);
  assert.equal(arq.state, ArqState.DONE);
  assert.equal(arq.meanRttMs(), 800);
  assert.equal(arq.send(8, frame, 900_000), true, 'the slot is free once acknowledged');
});

test('a stale ACK never acknowledges the frame currently in flight', () => {
  // The failure this prevents is silent: a retransmission produces a second
  // ACK, it arrives while the *next* config is in flight, and an ARQ that
  // accepted it would mark that config delivered. Nobody retries it, and
  // nobody ever finds out.
  const arq = new Arq({ random: seededRandom() });

  arq.send(10, frame, 0);
  assert.equal(arq.onAck(11, 1000), false, 'an ACK for another sequence');
  assert.equal(arq.onAck(10, 500_000), true);
  assert.equal(arq.onAck(10, 600_000), false, 'the duplicate is idempotent');

  arq.send(11, frame, 700_000);
  assert.equal(arq.onAck(10, 800_000), false, 'the straggler must not land on the new frame');
  assert.equal(arq.state, ArqState.WAIT_ACK);
  assert.equal(arq.stats.strayAcks, 3);
});

test('a dead link gives up after five retransmissions and frees the slot', () => {
  const arq = new Arq({ random: seededRandom() });
  arq.send(20, frame, 0);

  let now = 0;
  let retransmits = 0;
  let action = ArqAction.NOTHING;

  for (let step = 0; step < 200; step++) {
    now += 1_000_000;
    action = arq.tick(now).action;
    if (action === ArqAction.RETRANSMIT) retransmits++;
    if (action === ArqAction.GAVE_UP) break;
  }

  assert.equal(action, ArqAction.GAVE_UP);
  assert.equal(retransmits, ARQ_MAX_RETRIES);
  assert.equal(arq.state, ArqState.FAILED);
  // A component that jams on failure is worse than the failure it reports.
  assert.equal(arq.send(21, frame, now), true);
});

test('a duty-cycle refusal postpones a retry without spending one', () => {
  // R2.5. Five retransmissions of a full frame is nearly two seconds of
  // airtime against an hourly budget that is law. If a deferral counted as an
  // attempt, a busy hour would exhaust all five without a frame going on air.
  let allowed = false;
  const arq = new Arq({ random: seededRandom(), allowTx: () => allowed });
  arq.send(30, frame, 0);

  let now = 0;
  let deferrals = 0;
  for (let step = 0; step < 20; step++) {
    now += 1_000_000;
    if (arq.tick(now).action === ArqAction.DEFERRED) deferrals++;
  }

  assert.ok(deferrals > 0);
  assert.equal(arq.retryCount, 0, 'a deferral is not an attempt');
  assert.equal(arq.state, ArqState.WAIT_ACK, 'deferred, never dropped');

  allowed = true;
  assert.equal(arq.tick(now + 1_000_000).action, ArqAction.RETRANSMIT);
});

test('backoff doubles and every timeout carries jitter', () => {
  const arq = new Arq({ random: seededRandom() });

  for (let retry = 0; retry <= ARQ_MAX_RETRIES; retry++) {
    const base = ARQ_BASE_TIMEOUT_MS * 2 ** retry;
    const timeout = arq.timeoutMs(retry);
    assert.ok(timeout >= base && timeout < base + ARQ_JITTER_MS, `retry ${retry}: ${timeout} ms`);
  }
});

test('the jitter spread is wide enough to break up a retransmission storm', () => {
  // The number that stands between this design and R2.2. A uniform draw over
  // 500 ms has a theoretical sigma of 500/sqrt(12) = 144 ms; the 100 ms floor
  // is low enough not to be flaky and high enough that a jitter accidentally
  // narrowed to a quarter of its range would fail here rather than in the field
  // after the next power cut.
  const arq = new Arq({ random: seededRandom(0x51a3d7c9) });
  const draws = Array.from({ length: 1000 }, () => arq.timeoutMs(0));

  const mean = draws.reduce((sum, value) => sum + value, 0) / draws.length;
  const variance =
    draws.reduce((sum, value) => sum + (value - mean) ** 2, 0) / draws.length;
  const stddev = Math.sqrt(variance);

  assert.ok(stddev > 100, `jitter stddev is ${stddev.toFixed(1)} ms, expected > 100`);
  assert.ok(stddev < 200, `jitter stddev is ${stddev.toFixed(1)} ms, expected < 200`);
});
