import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { test } from 'node:test';

import { BRIDGE_ID, FrameType, decodeFrame, encodeFrame } from '@lorahome/protocol';

import {
  BRIDGE_STAT_SIZE,
  decodeBridgeStat,
  encodeBridgeStat,
  type BridgeStat,
} from '../src/transport/bridge-stat.js';
import { BridgeHealthClient } from '../src/transport/bridge-health.js';
import { SerialTransport, type ByteStream } from '../src/transport/serial.js';
import { SlipDecoder, slipEncode } from '../src/transport/slip.js';

const READING: Omit<BridgeStat, 'version'> = {
  uptimeMs: 86_400_000,
  heapFreeInternal: 196_108,
  heapLargestBlock: 172_032,
  heapMinFreeEver: 190_512,
  serialFramesIn: 17_280,
  serialFramesOut: 17_277,
  radioFramesIn: 17_277,
  radioFramesOut: 17_280,
  rejectedCrc: 3,
  rejectedMagic: 0,
  rejectedLen: 0,
  rejectedDutyCycle: 12,
  radioTxErrors: 0,
  serialTxErrors: 0,
  slipFramesOk: 17_280,
  slipOverflow: 0,
  slipBadEscape: 1,
  slipDropped: 1,
  ringOverrun: 0,
  ringHwm: 634,
  ringCapacity: 2048,
};

/**
 * A port with a Bridge behind it: decodes what the Host sends and answers
 * BRIDGE_STAT_REQ the way firmware/common/src/bridge_core.c does.
 */
class FakeBridgePort extends EventEmitter implements ByteStream {
  private readonly decoder = new SlipDecoder(256);
  requestsSeen = 0;
  answer = true;
  corruptReply = false;

  constructor(private readonly reading = READING) {
    super();
  }

  write(data: Uint8Array): boolean {
    for (const frame of this.decoder.push(data)) {
      let decoded;
      try {
        decoded = decodeFrame(frame);
      } catch {
        continue;
      }
      if (decoded.type !== FrameType.BRIDGE_STAT_REQ || decoded.dstId !== BRIDGE_ID) continue;

      this.requestsSeen++;
      if (!this.answer) continue;

      const payload = this.corruptReply
        ? new Uint8Array(BRIDGE_STAT_SIZE - 1)
        : encodeBridgeStat(this.reading);

      const reply = encodeFrame(
        {
          type: FrameType.BRIDGE_STAT_RSP,
          srcId: BRIDGE_ID,
          dstId: decoded.srcId,
          // Echoed, exactly as the firmware does — it is how a Host tells
          // replies apart.
          seq: decoded.seq,
          flags: 0,
        },
        payload,
      );

      // Delivered asynchronously, like a real port would.
      setImmediate(() => this.emit('data', Buffer.from(slipEncode(reply))));
    }
    return true;
  }
}

test('the payload round-trips through the TypeScript codec', () => {
  const decoded = decodeBridgeStat(encodeBridgeStat(READING));
  assert.equal(decoded.version, 1);
  for (const [key, value] of Object.entries(READING)) {
    assert.equal(decoded[key as keyof BridgeStat], value, key);
  }
});

test('a payload of the wrong length is refused, not guessed at', () => {
  // Silently accepting a short payload would mean reporting a heap figure read
  // from the wrong offset — a plausible number that is simply false.
  assert.throws(() => decodeBridgeStat(new Uint8Array(BRIDGE_STAT_SIZE - 1)));
  assert.throws(() => decodeBridgeStat(new Uint8Array(BRIDGE_STAT_SIZE + 1)));
});

test('an unknown version is refused', () => {
  const payload = encodeBridgeStat(READING);
  payload[1] = 99;
  assert.throws(() => decodeBridgeStat(payload), /version/);
});

test('the client reads the bridge over the link', async () => {
  const port = new FakeBridgePort();
  const client = new BridgeHealthClient(new SerialTransport(port));

  const stat = await client.read();
  assert.ok(stat !== null);
  assert.equal(port.requestsSeen, 1);
  assert.equal(stat.heapFreeInternal, 196_108);
  assert.equal(stat.heapLargestBlock, 172_032);
  assert.equal(stat.ringHwm, 634);
});

test('the soak adapter surfaces the three numbers the soak needs', async () => {
  const port = new FakeBridgePort();
  const client = new BridgeHealthClient(new SerialTransport(port));

  const health = await client.readForSoak();
  assert.ok(health !== null);
  assert.equal(health.heapFreeBytes, 196_108);
  assert.equal(health.largestBlockBytes, 172_032);
  assert.equal(health.uptimeSeconds, 86_400);
});

test('a silent bridge times out to null rather than hanging', async () => {
  // A missed sample is normal over 17280 of them. A soak that throws on the
  // first one is a soak that never finishes.
  const port = new FakeBridgePort();
  port.answer = false;
  const client = new BridgeHealthClient(new SerialTransport(port), { timeoutMs: 40 });

  assert.equal(await client.read(), null);
  assert.equal(port.requestsSeen, 1);
});

test('a malformed reply is dropped rather than parsed into a wrong number', async () => {
  const port = new FakeBridgePort();
  port.corruptReply = true;
  const client = new BridgeHealthClient(new SerialTransport(port), { timeoutMs: 40 });

  // Better a gap in the series than a heap figure read from the wrong offset:
  // the first is visible, the second is a leak detector reporting fiction.
  assert.equal(await client.read(), null);
});

test('consecutive reads are matched by sequence number', async () => {
  const port = new FakeBridgePort();
  const client = new BridgeHealthClient(new SerialTransport(port));

  for (let i = 0; i < 5; i++) {
    const stat = await client.read();
    assert.ok(stat !== null, `read ${i}`);
  }
  assert.equal(port.requestsSeen, 5);
});
