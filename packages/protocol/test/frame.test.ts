import assert from 'node:assert/strict';
import { test } from 'node:test';
import { decodeFrame, encodeFrame, FrameFlags, FrameType } from '../src/frame.js';

test('encodeFrame/decodeFrame round-trip', () => {
  const payload = new Uint8Array([0xde, 0xad, 0xbe, 0xef]);
  const wire = encodeFrame(
    { type: FrameType.TELEMETRY, srcId: 42, dstId: 1, seq: 7, flags: FrameFlags.ACK_REQ },
    payload,
  );

  const frame = decodeFrame(wire);
  assert.equal(frame.type, FrameType.TELEMETRY);
  assert.equal(frame.srcId, 42);
  assert.equal(frame.dstId, 1);
  assert.equal(frame.seq, 7);
  assert.equal(frame.flags, FrameFlags.ACK_REQ);
  assert.deepEqual([...frame.payload], [...payload]);
});

test('decodeFrame rejects a corrupted CRC', () => {
  const wire = encodeFrame(
    { type: FrameType.BEACON, srcId: 1, dstId: 0xffff, seq: 0, flags: FrameFlags.NONE },
    new Uint8Array(),
  );
  wire[wire.length - 1] ^= 0xff; // flip a CRC byte
  assert.throws(() => decodeFrame(wire), /CRC mismatch/);
});
