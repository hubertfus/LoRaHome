import assert from 'node:assert/strict';
import { test } from 'node:test';
import fc from 'fast-check';
import { DedupWindow, DEDUP_PEERS, DEDUP_WINDOW } from '../src/dedup.js';

/**
 * Tests for the Host's dedup window (T2.1).
 *
 * The C twin is tested at volume in firmware/common/test/dedup_selftest.c and
 * the two are compared event-for-event by tools/check-dedup-cross.mjs. What is
 * tested here is what only this side can get wrong — JavaScript's 32-bit
 * bitwise operators are signed, so a bitmap with bit 31 set is a negative
 * number, and a missing `>>> 0` turns the window's oldest entry into a
 * silently wrong answer.
 */

const clock = (): (() => number) => {
  let us = 0;
  return () => (us += 1000);
};

test('a first frame from an unknown sender is new', () => {
  const window = new DedupWindow();
  const now = clock();
  assert.equal(window.checkAndMark(0x0101, 0, now()), true);
  assert.equal(window.stats.accepted, 1);
  assert.equal(window.peerCount, 1);
});

test('an immediate replay is refused and counted as a duplicate', () => {
  const window = new DedupWindow();
  const now = clock();
  window.checkAndMark(0x0101, 42, now());
  assert.equal(window.checkAndMark(0x0101, 42, now()), false);
  assert.equal(window.stats.dupesDropped, 1);
});

test('sequence numbers wrap forwards, not 255 frames backwards', () => {
  // R2.1. Written as an explicit case rather than left to the property test,
  // because the property generator would hit it eventually and this is the one
  // failure that must be legible in the test output when it happens.
  const window = new DedupWindow();
  const now = clock();
  for (const seq of [0xfd, 0xfe, 0xff, 0x00, 0x01]) {
    assert.equal(window.checkAndMark(0x0202, seq, now()), true, `seq 0x${seq.toString(16)}`);
  }
  assert.equal(window.checkAndMark(0x0202, 0xff, now()), false, 'still inside the window');
  assert.equal(window.stats.tooOld, 0);
});

test('out-of-order frames inside the window are each new exactly once', () => {
  const window = new DedupWindow();
  const now = clock();
  for (const seq of [5, 3, 7, 4, 6]) {
    assert.equal(window.checkAndMark(0x0303, seq, now()), true, `seq ${seq}`);
  }
  for (const seq of [3, 4, 5, 6, 7]) {
    assert.equal(window.checkAndMark(0x0303, seq, now()), false, `replayed seq ${seq}`);
  }
  // A gap that never arrived is still open.
  assert.equal(window.checkAndMark(0x0303, 2, now()), true);
});

test('the window edge is exactly 32 sequence numbers wide', () => {
  const window = new DedupWindow();
  const now = clock();
  window.checkAndMark(0x0404, 100, now());

  // Age 31 is the oldest remembered frame — bit 31, the sign bit, and the one
  // an unsigned-shift bug would get wrong.
  assert.equal(window.checkAndMark(0x0404, 100 - (DEDUP_WINDOW - 1), now()), true);
  assert.equal(window.checkAndMark(0x0404, 100 - (DEDUP_WINDOW - 1), now()), false);
  assert.equal(window.stats.dupesDropped, 1);

  assert.equal(window.checkAndMark(0x0404, 100 - DEDUP_WINDOW, now()), false);
  assert.equal(window.stats.tooOld, 1);
});

test('a step of a full window forgets the old bitmap without shifting past it', () => {
  const window = new DedupWindow();
  const now = clock();
  window.checkAndMark(0x0505, 0, now());
  window.checkAndMark(0x0505, 200, now()); // a jump far beyond the window
  const peer = window.findPeer(0x0505);
  assert.ok(peer);
  assert.equal(peer.bitmap, 1, 'only the newest frame should be remembered');
  assert.equal(window.checkAndMark(0x0505, 200, now()), false);
});

test('senders do not share a window', () => {
  const window = new DedupWindow();
  const now = clock();
  for (let peer = 0; peer < DEDUP_PEERS; peer++) {
    assert.equal(window.checkAndMark(0x0600 + peer, 5, now()), true);
  }
  assert.equal(window.stats.dupesDropped, 0);
});

test('the ninth sender evicts the least recently heard, and says so', () => {
  const window = new DedupWindow();
  const now = clock();
  for (let peer = 0; peer < DEDUP_PEERS; peer++) window.checkAndMark(0x0700 + peer, 1, now());
  for (let peer = 1; peer < DEDUP_PEERS; peer++) window.checkAndMark(0x0700 + peer, 2, now());

  assert.equal(window.checkAndMark(0x9999, 1, now()), true);
  assert.equal(window.stats.peerEvicted, 1);
  assert.equal(window.peerCount, DEDUP_PEERS);
  assert.equal(window.findPeer(0x0700), undefined, 'the idle peer was evicted');
  assert.ok(window.findPeer(0x0701), 'an active peer was not');
});

/**
 * The invariant, stated as a property: no frame is ever processed twice.
 *
 * Frames carry an identity the window cannot see — a per-sender counter, of
 * which the sequence number is the low byte — so "the same frame" is a fact
 * about the stream rather than about the window's opinion of it. Retransmissions
 * are drawn from the recent past, which is what an ARQ under loss produces;
 * scattering them uniformly would mostly generate frames outside the window and
 * test the cheap rejection path instead of the interesting one.
 */
test('property: no frame is accepted twice, and no first sighting is refused', () => {
  fc.assert(
    fc.property(
      fc.integer({ min: 1, max: 0xffffffff }),
      fc.array(fc.integer({ min: 0, max: 99 }), { minLength: 200, maxLength: 2000 }),
      (seed, draws) => {
        const window = new DedupWindow();
        const now = clock();
        const seen = new Map<string, boolean>();
        const nextId = new Map<number, number>();
        let doubleProcessed = 0;
        let originalsRefused = 0;
        let rng = seed;

        const nextRandom = (): number => {
          rng ^= rng << 13;
          rng ^= rng >>> 17;
          rng ^= rng << 5;
          return rng >>> 0;
        };

        for (const draw of draws) {
          const src = 0x0800 + (draw % 3);
          const current = nextId.get(src) ?? 0;

          let id: number;
          if (draw % 3 === 0 && current > 20) {
            id = current - 1 - (nextRandom() % 20); // a retransmission
          } else {
            id = current;
            nextId.set(src, current + 1);
          }

          const key = `${src}:${id}`;
          if (window.checkAndMark(src, id & 0xff, now())) {
            if (seen.get(key) === true) doubleProcessed++;
            seen.set(key, true);
          } else if (seen.get(key) !== true) {
            originalsRefused++;
          }
        }

        return doubleProcessed === 0 && originalsRefused === 0;
      },
    ),
    { numRuns: 200 },
  );
});
