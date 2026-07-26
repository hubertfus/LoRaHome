import assert from 'node:assert/strict';
import { test } from 'node:test';
import { LINK_PROFILES, SimLink } from '../src/transport/sim-link.js';

/**
 * Tests for the link simulator (T2.5).
 *
 * The simulator is test infrastructure, which is exactly why it needs tests of
 * its own: every reliability result in Etap 2 is measured through it, and a
 * simulator that silently dropped nothing would turn the whole chaos suite into
 * a very slow way of proving that a working link works.
 *
 * So each impairment is checked to actually happen, at roughly the rate asked
 * for, and — the property the suite depends on — the same seed is checked to
 * produce the same trace, event for event.
 */

const frameOf = (length: number, seed = 0): Uint8Array =>
  Uint8Array.from({ length }, (_, i) => (i + seed) & 0xff);

/** Sends `count` frames one way and drains the link. */
function run(link: SimLink, count: number, frameLength = 32): Buffer[] {
  const received: Buffer[] = [];
  link.b.onFrame((frame) => received.push(frame));
  for (let i = 0; i < count; i++) {
    link.a.send(frameOf(frameLength, i));
    link.advance(500);
  }
  link.drain();
  return received;
}

test('a clean link delivers everything, in order, unchanged', () => {
  const link = new SimLink({ seed: 0x1234, ...LINK_PROFILES.clean });
  const received = run(link, 200);

  assert.equal(received.length, 200);
  for (const [index, frame] of received.entries()) {
    assert.deepEqual(Array.from(frame), Array.from(frameOf(32, index)), `frame ${index}`);
  }
  assert.equal(link.stats.dropped, 0);
  assert.equal(link.stats.inFlight, 0);
});

test('a frame is delivered after the link latency, not before', () => {
  // Default latency is a 230 B frame at SF9. A test that got its frames back
  // instantly would make every timeout in the ARQ look generous.
  const link = new SimLink({ seed: 0x1234 });
  const received: Buffer[] = [];
  link.b.onFrame((frame) => received.push(frame));

  link.a.send(frameOf(16));
  link.advance(389);
  assert.equal(received.length, 0, 'nothing arrives before the airtime has elapsed');
  link.advance(1);
  assert.equal(received.length, 1);
});

test('loss happens at about the rate requested', () => {
  const link = new SimLink({ seed: 0x8f2a, ...LINK_PROFILES.loss30 });
  const received = run(link, 2000, 16);

  // 30% of 2000 with a fixed seed: the interval is generous because this is a
  // check that the knob is connected, not a test of the generator's uniformity.
  assert.ok(received.length > 1300 && received.length < 1500, `delivered ${received.length}`);
  assert.equal(link.stats.dropped + link.stats.delivered, link.stats.sent);
});

test('corruption changes bytes without changing lengths', () => {
  const link = new SimLink({ seed: 0x51a3, ...LINK_PROFILES.corrupt });
  const received = run(link, 500, 64);

  assert.equal(received.length, 500, 'corruption is not loss — the frame still arrives');
  const damaged = received.filter(
    (frame, index) => !frame.equals(Buffer.from(frameOf(64, index))),
  );
  assert.ok(damaged.length > 0, 'nothing was corrupted');
  assert.equal(link.stats.corrupted, damaged.length);
  for (const frame of received) assert.equal(frame.length, 64);
});

test('reordering delivers frames out of the order they were sent', () => {
  const link = new SimLink({ seed: 0x2b7e, ...LINK_PROFILES.reorder });
  const received: number[] = [];
  link.b.onFrame((frame) => received.push(frame[0]!));

  // Sent close together so a held-back frame really does land behind its
  // successor; spacing them by more than the delay would hide the effect.
  for (let i = 0; i < 200; i++) {
    link.a.send(Uint8Array.from([i & 0xff, 0, 0, 0]));
    link.advance(20);
  }
  link.drain();

  assert.equal(received.length, 200, 'reordering is not loss');
  const outOfOrder = received.some((value, index) => index > 0 && value < received[index - 1]!);
  assert.ok(outOfOrder, 'nothing arrived out of order');
  assert.ok(link.stats.reordered > 0);
});

test('duplicates arrive twice, byte for byte', () => {
  const link = new SimLink({ seed: 0x6c8e, duplicatePct: 100, latencyMs: 10 });
  const received: Buffer[] = [];
  link.b.onFrame((frame) => received.push(frame));

  link.a.send(frameOf(8));
  link.drain();

  assert.equal(received.length, 2);
  assert.deepEqual(Array.from(received[0]!), Array.from(received[1]!));
});

test('the same seed produces the same trace, 100 runs out of 100', () => {
  // R2.6, and the reason this file exists. A failure found in CI has to be
  // reproducible on a laptop from the seed alone; comparing counters would not
  // be enough, because two runs can drop the same number of frames and disagree
  // about which ones.
  const traceFor = (seed: number): readonly string[] => {
    const link = new SimLink({
      seed,
      lossPct: 20,
      corruptPct: 10,
      reorderPct: 15,
      duplicatePct: 5,
      jitterMs: 50,
    });
    link.startTrace();
    run(link, 300, 24);
    return link.trace;
  };

  const reference = traceFor(0x8f2a).join('\n');
  let identical = 0;
  for (let run_ = 0; run_ < 100; run_++) {
    if (traceFor(0x8f2a).join('\n') === reference) identical++;
  }

  assert.equal(identical, 100);
  assert.ok(reference.length > 0, 'the trace recorded nothing');

  // And a different seed must actually take a different path, or "deterministic"
  // would be indistinguishable from "does nothing".
  assert.notEqual(traceFor(0x1d87).join('\n'), reference);
});

test('seed 0 is refused rather than silently repaired', () => {
  // xorshift seeded with zero stays at zero for ever: every draw identical,
  // every probability collapsing to one branch. A test that asked for seed 0
  // and got seed 1 is a test whose seed is a lie.
  assert.throws(() => new SimLink({ seed: 0 }), RangeError);
});

test('both directions are independent', () => {
  const link = new SimLink({ seed: 0x1234, latencyMs: 100 });
  const atA: Buffer[] = [];
  const atB: Buffer[] = [];
  link.a.onFrame((frame) => atA.push(frame));
  link.b.onFrame((frame) => atB.push(frame));

  link.a.send(Uint8Array.from([1]));
  link.b.send(Uint8Array.from([2]));
  link.drain();

  assert.equal(atB.length, 1);
  assert.equal(atB[0]![0], 1, 'what A sent must arrive at B');
  assert.equal(atA.length, 1);
  assert.equal(atA[0]![0], 2, 'and the reverse');
});
