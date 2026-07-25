import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { test } from 'node:test';

import {
  SLIP_END,
  SLIP_ESC,
  SLIP_ESC_END,
  SLIP_ESC_ESC,
  SlipDecoder,
  SlipState,
  slipEncode,
  slipEncodedMax,
} from '../src/transport/slip.js';
import { SerialTransport, type ByteStream } from '../src/transport/serial.js';

/**
 * A byte stream that records what was written and lets a test push bytes back.
 *
 * The whole reason SerialTransport takes a stream instead of a port path: with
 * this, the framing and the accounting are ordinary unit tests rather than
 * something only checkable with a board plugged in.
 */
class FakePort extends EventEmitter implements ByteStream {
  readonly written: Buffer[] = [];
  acceptWrites = true;

  write(data: Uint8Array): boolean {
    this.written.push(Buffer.from(data));
    return this.acceptWrites;
  }

  /** Delivers bytes as the OS would: in arbitrary chunks. */
  deliver(bytes: Uint8Array, chunkSize = bytes.length): void {
    for (let i = 0; i < bytes.length; i += chunkSize) {
      this.emit('data', Buffer.from(bytes.subarray(i, i + chunkSize)));
    }
  }
}

// ---------------------------------------------------------------------------
// Codec — the same contract as firmware/common/src/slip.c
// ---------------------------------------------------------------------------

test('an empty payload encodes to the two delimiters and decodes to nothing', () => {
  const encoded = slipEncode(new Uint8Array());
  assert.deepEqual([...encoded], [SLIP_END, SLIP_END]);

  const decoder = new SlipDecoder();
  // END END is a delimiter pair, not a zero-length message.
  assert.equal(decoder.push(encoded).length, 0);
  assert.equal(decoder.stats.framesOk, 0);
});

test('the two reserved bytes are escaped and nothing else is', () => {
  const encoded = slipEncode(new Uint8Array([SLIP_END, SLIP_ESC, 0x42]));
  assert.deepEqual(
    [...encoded],
    [SLIP_END, SLIP_ESC, SLIP_ESC_END, SLIP_ESC, SLIP_ESC_ESC, 0x42, SLIP_END],
  );
});

test('round-trips 1000 random payloads of every length up to the MTU', () => {
  const decoder = new SlipDecoder();
  let checked = 0;

  for (let i = 0; i < 1000; i++) {
    const length = Math.floor(Math.random() * 231);
    const payload = new Uint8Array(length);
    for (let j = 0; j < length; j++) payload[j] = Math.floor(Math.random() * 256);

    const frames = decoder.push(slipEncode(payload));
    if (length === 0) {
      assert.equal(frames.length, 0);
      continue;
    }
    assert.equal(frames.length, 1);
    assert.deepEqual([...frames[0]!], [...payload]);
    checked++;
  }

  assert.ok(checked > 900, 'expected nearly every payload to be non-empty');
});

test('worst case is exactly 2n+2 and survives the round trip', () => {
  // Risk R1.1: the payload that doubles on the wire.
  const payload = new Uint8Array(230);
  for (let i = 0; i < payload.length; i++) payload[i] = i % 2 === 0 ? SLIP_END : SLIP_ESC;

  const encoded = slipEncode(payload);
  assert.equal(encoded.length, slipEncodedMax(payload.length));

  const frames = new SlipDecoder().push(encoded);
  assert.equal(frames.length, 1);
  assert.deepEqual([...frames[0]!], [...payload]);
});

test('frames straddling chunk boundaries decode identically', () => {
  // A serial read hands over whatever was in the OS buffer, so this is the
  // normal case rather than an edge case.
  const payload = Uint8Array.from({ length: 100 }, (_, i) => (i * 7) % 256);
  const encoded = slipEncode(payload);

  for (const chunkSize of [1, 2, 3, 7, 64]) {
    const decoder = new SlipDecoder();
    const frames: Buffer[] = [];
    for (let i = 0; i < encoded.length; i += chunkSize) {
      frames.push(...decoder.push(encoded.subarray(i, i + chunkSize)));
    }
    assert.equal(frames.length, 1, `chunk size ${chunkSize}`);
    assert.deepEqual([...frames[0]!], [...payload], `chunk size ${chunkSize}`);
  }
});

test('an illegal escape pair drops one frame and no more', () => {
  const decoder = new SlipDecoder();

  // ESC followed by something that is neither ESC_END nor ESC_ESC.
  const corrupt = Uint8Array.from([SLIP_END, 0x11, SLIP_ESC, 0x42, 0x33, SLIP_END]);
  assert.equal(decoder.push(corrupt).length, 0);
  assert.equal(decoder.stats.badEscape, 1);
  assert.equal(decoder.stats.dropped, 1);
  assert.equal(decoder.stats.framesOk, 0);

  // The property that matters: the next frame arrives whole.
  const good = Uint8Array.from([0xde, 0xad, 0xbe, 0xef]);
  const frames = decoder.push(slipEncode(good));
  assert.equal(frames.length, 1);
  assert.deepEqual([...frames[0]!], [...good]);
});

test('a truncated escape does not swallow the following frame', () => {
  const decoder = new SlipDecoder();
  decoder.push(Uint8Array.from([SLIP_END, 0x01, SLIP_ESC, SLIP_END]));
  assert.equal(decoder.stats.badEscape, 1);

  const good = Uint8Array.from([0x55, 0xaa]);
  const frames = decoder.push(slipEncode(good));
  assert.equal(frames.length, 1, 'the END that exposed the error was consumed as a delimiter');
  assert.deepEqual([...frames[0]!], [...good]);
});

test('an oversized frame is counted and dropped, and the decoder recovers', () => {
  const decoder = new SlipDecoder(32);
  const tooBig = new Uint8Array(100);

  assert.equal(decoder.push(slipEncode(tooBig)).length, 0);
  assert.equal(decoder.stats.overflow, 1);
  assert.equal(decoder.stats.dropped, 1);

  const good = Uint8Array.from([1, 2, 3]);
  const frames = decoder.push(slipEncode(good));
  assert.equal(frames.length, 1);
  assert.deepEqual([...frames[0]!], [...good]);
});

test('leading noise is discarded and the decoder syncs on the next delimiter', () => {
  const decoder = new SlipDecoder();
  const payload = Uint8Array.from([0xaa, 0xbb]);

  // Garbage, then a real frame. The garbage is data-indistinguishable, so it
  // forms its own bogus frame — which is precisely why CRC sits above SLIP.
  const noise = Uint8Array.from([0x01, 0x02, 0x03]);
  const frames = decoder.push(Uint8Array.from([...noise, ...slipEncode(payload)]));

  const last = frames.at(-1);
  assert.ok(last !== undefined);
  assert.deepEqual([...last], [...payload]);
  assert.equal(decoder.currentState, SlipState.Idle);
});

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

test('send frames the payload and write delivers it', () => {
  const port = new FakePort();
  const transport = new SerialTransport(port);

  const frame = Uint8Array.from([0x4b, 0x20, 0x00, 0x01]);
  assert.equal(transport.send(frame), true);

  assert.equal(port.written.length, 1);
  assert.deepEqual([...port.written[0]!], [...slipEncode(frame)]);
  assert.equal(transport.stats.framesSent, 1);
});

test('incoming bytes are surfaced as whole frames', () => {
  const port = new FakePort();
  const transport = new SerialTransport(port);

  const received: Buffer[] = [];
  transport.onFrame((frame) => received.push(frame));

  const a = Uint8Array.from([1, 2, 3]);
  const b = Uint8Array.from([SLIP_END, SLIP_ESC]);
  port.deliver(Uint8Array.from([...slipEncode(a), ...slipEncode(b)]), 3);

  assert.equal(received.length, 2);
  assert.deepEqual([...received[0]!], [...a]);
  assert.deepEqual([...received[1]!], [...b]);
  assert.equal(transport.stats.framesOk, 2);
});

test('backpressure is counted but not treated as a lost frame', () => {
  const port = new FakePort();
  port.acceptWrites = false;
  const transport = new SerialTransport(port);

  // `write` returning false means "queued, stop pushing", not "dropped".
  // Retrying here would duplicate the frame on the wire.
  assert.equal(transport.send(Uint8Array.from([1, 2, 3])), true);
  assert.equal(transport.stats.writeFailures, 1);
  assert.equal(transport.stats.framesSent, 1);
  assert.equal(port.written.length, 1);
});

test('a closed port refuses further sends', () => {
  const port = new FakePort();
  const transport = new SerialTransport(port);
  port.emit('close');
  assert.equal(transport.send(Uint8Array.from([1])), false);
});

test('stream errors reach the caller instead of being swallowed', () => {
  const port = new FakePort();
  const transport = new SerialTransport(port);

  const seen: Error[] = [];
  transport.onError((error) => seen.push(error));
  port.emit('error', new Error('cable pulled'));

  assert.equal(seen.length, 1);
  assert.equal(seen[0]?.message, 'cable pulled');
});

// ---------------------------------------------------------------------------
// Risk R1.7: the Host is the component that runs for weeks.
// ---------------------------------------------------------------------------

test(
  '100k frames through the transport do not grow the heap',
  { skip: typeof globalThis.gc !== 'function' ? 'run with --expose-gc' : false },
  () => {
    const port = new FakePort();
    const transport = new SerialTransport(port);
    let delivered = 0;
    transport.onFrame(() => delivered++);

    const payload = Uint8Array.from({ length: 200 }, (_, i) => i % 256);
    const encoded = slipEncode(payload);

    // Warm up first: measuring from a cold start would charge JIT tiering and
    // lazily-grown internal buffers to the frame loop.
    for (let i = 0; i < 5_000; i++) {
      port.written.length = 0;
      transport.send(payload);
      port.deliver(encoded);
    }

    globalThis.gc!();
    const before = process.memoryUsage().heapUsed;

    for (let i = 0; i < 100_000; i++) {
      port.written.length = 0;
      transport.send(payload);
      port.deliver(encoded);
    }

    globalThis.gc!();
    const growth = (process.memoryUsage().heapUsed - before) / (1024 * 1024);

    assert.equal(delivered, 105_000);
    assert.ok(growth < 1, `heap grew ${growth.toFixed(2)} MB over 100k frames (budget: 1 MB)`);
  },
);
