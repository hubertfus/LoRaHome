import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import fc from 'fast-check';
import { crc16 } from '../src/crc16.js';
import {
  BROADCAST_ID,
  CRC_SIZE,
  decodeFrame,
  decodeHeader,
  encodeFrame,
  encodeHeader,
  encodeHeaderInto,
  FRAME_MAGIC,
  FrameFlags,
  FrameType,
  HEADER_LAYOUT,
  HEADER_SIZE,
  LORA_MTU,
  MAX_PAYLOAD,
  MIN_FRAME_SIZE,
  type FrameHeader,
} from '../src/frame.js';

const hex = (bytes: Uint8Array): string => Buffer.from(bytes).toString('hex').toUpperCase();
const unhex = (s: string): Uint8Array => new Uint8Array(Buffer.from(s, 'hex'));

/**
 * Fixtures are read from source, not from dist: they are shared data that the
 * C-side tests read from the same path, and tsc has no reason to copy them.
 * Walking up to package.json keeps this working whether the test runs from
 * test/ or dist/test/, and regardless of the caller's cwd.
 */
const FIXTURES_DIR = (() => {
  let dir = dirname(fileURLToPath(import.meta.url));
  while (!existsSync(join(dir, 'package.json'))) {
    const parent = dirname(dir);
    if (parent === dir) throw new Error('could not locate package root from ' + import.meta.url);
    dir = parent;
  }
  return join(dir, 'test', 'fixtures');
})();

const loadVector = (name: string): GoldenVector =>
  JSON.parse(readFileSync(join(FIXTURES_DIR, `${name}.json`), 'utf8')) as GoldenVector;

// ---------------------------------------------------------------------------
// Layout invariants — these guard the source of truth itself.
// ---------------------------------------------------------------------------

test('HEADER_LAYOUT tiles the header exactly: no gaps, no overlaps', () => {
  const covered = new Array<string | undefined>(HEADER_SIZE).fill(undefined);

  for (const field of HEADER_LAYOUT) {
    for (let i = field.offset; i < field.offset + field.size; i++) {
      assert.ok(i < HEADER_SIZE, `field ${field.name} runs past HEADER_SIZE`);
      assert.equal(covered[i], undefined, `byte ${i} claimed by both ${covered[i]} and ${field.name}`);
      covered[i] = field.name;
    }
  }

  const gaps = covered.flatMap((owner, i) => (owner === undefined ? [i] : []));
  assert.deepEqual(gaps, [], 'every header byte must belong to exactly one field');
});

test('HEADER_LAYOUT field sizes match their declared endianness', () => {
  for (const field of HEADER_LAYOUT) {
    const expected = field.endian === 'u8' ? 1 : 2;
    assert.equal(field.size, expected, `field ${field.name}: size/endian disagree`);
  }
});

test('derived size constants stay consistent', () => {
  assert.equal(MAX_PAYLOAD, LORA_MTU - HEADER_SIZE - CRC_SIZE);
  assert.equal(MIN_FRAME_SIZE, HEADER_SIZE + CRC_SIZE);
  assert.equal(MAX_PAYLOAD, 220);
  assert.equal(MIN_FRAME_SIZE, 10);
});

// ---------------------------------------------------------------------------
// Golden vectors. These files are the byte-for-byte contract with the firmware.
// If a change here forces a fixture edit, the on-air format changed — which is
// a protocol decision, not a refactor.
// ---------------------------------------------------------------------------

interface GoldenVector {
  description: string;
  header: Record<string, string>;
  payload_hex: string;
  wire_hex: string;
}

const GOLDEN_FILES = ['beacon', 'telemetry', 'boundary'] as const;

for (const name of GOLDEN_FILES) {
  test(`golden vector ${name}.json encodes byte-for-byte`, () => {
    const vector = loadVector(name);

    const header: FrameHeader = {
      type: Number(vector.header['type']),
      srcId: Number(vector.header['srcId']),
      dstId: Number(vector.header['dstId']),
      seq: Number(vector.header['seq']),
      flags: Number(vector.header['flags']),
    };

    assert.equal(hex(encodeFrame(header, unhex(vector.payload_hex))), vector.wire_hex);
  });

  test(`golden vector ${name}.json decodes back to its header and payload`, () => {
    const vector = loadVector(name);
    const frame = decodeFrame(unhex(vector.wire_hex));
    assert.equal(frame.type, Number(vector.header['type']));
    assert.equal(frame.srcId, Number(vector.header['srcId']));
    assert.equal(frame.dstId, Number(vector.header['dstId']));
    assert.equal(frame.seq, Number(vector.header['seq']));
    assert.equal(frame.flags, Number(vector.header['flags']));
    assert.equal(hex(frame.payload), vector.payload_hex);
  });
}

test('multi-byte header fields are big-endian on the wire', () => {
  // 0x0102 must appear as 01 02, not 02 01. This is risk R0.2 in one assertion:
  // a little-endian slip here is invisible until two devices disagree on air.
  const wire = encodeHeader({
    type: FrameType.BEACON,
    srcId: 0x0102,
    dstId: 0x0304,
    seq: 0,
    flags: 0,
  });
  assert.equal(hex(wire), '4B01010203040000');
});

// ---------------------------------------------------------------------------
// Property-based round-trip.
// ---------------------------------------------------------------------------

const arbHeader = fc.record({
  type: fc.integer({ min: 0, max: 0xff }),
  srcId: fc.integer({ min: 0, max: 0xffff }),
  dstId: fc.integer({ min: 0, max: 0xffff }),
  seq: fc.integer({ min: 0, max: 0xff }),
  flags: fc.integer({ min: 0, max: 0xff }),
});

test('encodeHeader -> decodeHeader round-trips for arbitrary headers', () => {
  fc.assert(
    fc.property(arbHeader, (header) => {
      // Spread to a plain object: fc.record yields a null-prototype object and
      // deepEqual compares prototypes, which would fail on identical values.
      assert.deepEqual(decodeHeader(encodeHeader(header)), { ...header });
    }),
    { numRuns: 1000 },
  );
});

test('encodeFrame -> decodeFrame round-trips with arbitrary payloads', () => {
  fc.assert(
    fc.property(arbHeader, fc.uint8Array({ maxLength: MAX_PAYLOAD }), (header, payload) => {
      const frame = decodeFrame(encodeFrame(header, payload));
      assert.deepEqual(
        { type: frame.type, srcId: frame.srcId, dstId: frame.dstId, seq: frame.seq, flags: frame.flags },
        { ...header },
      );
      assert.deepEqual([...frame.payload], [...payload]);
    }),
    { numRuns: 1000 },
  );
});

test('every single-bit flip in a frame is rejected', () => {
  const wire = encodeFrame(
    { type: FrameType.TELEMETRY, srcId: 0x1234, dstId: BROADCAST_ID, seq: 9, flags: 0 },
    new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]),
  );

  let detected = 0;
  let total = 0;
  for (let byte = 0; byte < wire.length; byte++) {
    for (let bit = 0; bit < 8; bit++) {
      const corrupted = Uint8Array.from(wire);
      corrupted[byte] = corrupted[byte]! ^ (1 << bit);
      total++;
      try {
        decodeFrame(corrupted);
      } catch {
        detected++;
      }
    }
  }
  assert.equal(detected, total, `${total - detected}/${total} single-bit flips slipped through`);
});

// ---------------------------------------------------------------------------
// Rejection paths.
// ---------------------------------------------------------------------------

test('decodeHeader rejects a bad magic byte', () => {
  const wire = encodeHeader({ type: FrameType.BEACON, srcId: 1, dstId: 2, seq: 3, flags: 0 });
  wire[0] = 0x4c;
  assert.throws(() => decodeHeader(wire), /bad magic/);
});

test('decodeHeader rejects buffers shorter than the header', () => {
  // decodeHeader is exported in its own right, so it carries its own guard —
  // decodeFrame's stricter MIN_FRAME_SIZE check never exercises this path.
  for (let len = 0; len < HEADER_SIZE; len++) {
    assert.throws(() => decodeHeader(new Uint8Array(len)), /too short/, `length ${len} was accepted`);
  }
});

test('decodeFrame rejects frames shorter than header + CRC', () => {
  for (let len = 0; len < MIN_FRAME_SIZE; len++) {
    assert.throws(() => decodeFrame(new Uint8Array(len)), /too short/, `length ${len} was accepted`);
  }
});

test('decodeFrame rejects frames longer than the LoRa MTU', () => {
  assert.throws(() => decodeFrame(new Uint8Array(LORA_MTU + 1)), /too long/);
});

test('decodeFrame rejects a corrupted CRC', () => {
  const wire = encodeFrame(
    { type: FrameType.BEACON, srcId: 1, dstId: BROADCAST_ID, seq: 0, flags: FrameFlags.NONE },
    new Uint8Array(),
  );
  const last = wire.length - 1;
  wire[last] = wire[last]! ^ 0xff;
  assert.throws(() => decodeFrame(wire), /CRC mismatch/);
});

test('encodeFrame refuses a payload over MAX_PAYLOAD', () => {
  const header: FrameHeader = { type: FrameType.TELEMETRY, srcId: 1, dstId: 2, seq: 0, flags: 0 };
  assert.doesNotThrow(() => encodeFrame(header, new Uint8Array(MAX_PAYLOAD)));
  assert.throws(() => encodeFrame(header, new Uint8Array(MAX_PAYLOAD + 1)), /exceeds MAX_PAYLOAD/);
});

test('encodeHeader range-checks fields instead of truncating them silently', () => {
  const base: FrameHeader = { type: FrameType.BEACON, srcId: 1, dstId: 2, seq: 3, flags: 0 };
  assert.throws(() => encodeHeader({ ...base, srcId: 0x10000 }), /src_id must be an integer/);
  assert.throws(() => encodeHeader({ ...base, seq: 256 }), /seq must be an integer/);
  assert.throws(() => encodeHeader({ ...base, type: -1 }), /type must be an integer/);
  assert.throws(() => encodeHeader({ ...base, flags: 1.5 }), /flags must be an integer/);
});

test('encodeHeaderInto writes the same bytes as encodeHeader, without allocating', () => {
  const header: FrameHeader = { type: FrameType.CMD, srcId: 0xbeef, dstId: 0x0001, seq: 5, flags: 2 };
  const buf = new Uint8Array(HEADER_SIZE);
  encodeHeaderInto(header, buf);
  assert.equal(hex(buf), hex(encodeHeader(header)));
});

test('encodeHeaderInto honours a non-zero offset and leaves neighbours intact', () => {
  const header: FrameHeader = { type: FrameType.BEACON, srcId: 0x0102, dstId: 0x0304, seq: 1, flags: 0 };
  const buf = new Uint8Array(HEADER_SIZE + 4).fill(0xaa);
  encodeHeaderInto(header, buf, 2);

  assert.equal(hex(buf.subarray(0, 2)), 'AAAA', 'wrote before the offset');
  assert.equal(hex(buf.subarray(2, 2 + HEADER_SIZE)), hex(encodeHeader(header)));
  assert.equal(hex(buf.subarray(2 + HEADER_SIZE)), 'AAAA', 'wrote past the header');
});

test('encodeHeaderInto refuses a buffer that cannot hold the header', () => {
  const header: FrameHeader = { type: FrameType.BEACON, srcId: 1, dstId: 2, seq: 0, flags: 0 };
  assert.throws(() => encodeHeaderInto(header, new Uint8Array(HEADER_SIZE - 1)), /need 8 bytes/);
  assert.throws(() => encodeHeaderInto(header, new Uint8Array(HEADER_SIZE), 1), /need 8 bytes/);
  assert.throws(() => encodeHeaderInto(header, new Uint8Array(HEADER_SIZE), -1), /need 8 bytes/);
});

test('a full-MTU frame is exactly LORA_MTU bytes on the wire', () => {
  const wire = encodeFrame(
    { type: FrameType.TELEMETRY, srcId: 1, dstId: 2, seq: 0, flags: 0 },
    new Uint8Array(MAX_PAYLOAD),
  );
  assert.equal(wire.length, LORA_MTU);
  assert.doesNotThrow(() => decodeFrame(wire));
});

// ---------------------------------------------------------------------------
// CRC anchor. The full cross-language vector suite lands in T0.5; this pins the
// algorithm identity so the golden frames above cannot drift silently.
// ---------------------------------------------------------------------------

test('crc16 matches the CRC-16/CCITT-FALSE check value', () => {
  // Catalogue check value for poly=0x1021, init=0xFFFF, no reflect, no xorout.
  // NB: 0xE5CC (quoted in the roadmap) is the CRC-16/AUG-CCITT value, which
  // uses init=0x1D0F — a different algorithm. See the T0.5 commit message.
  assert.equal(crc16(new TextEncoder().encode('123456789')), 0x29b1);
});

test('crc16 of an empty buffer is the init value', () => {
  assert.equal(crc16(new Uint8Array()), 0xffff);
});

test('FRAME_MAGIC is the first byte of every encoded frame', () => {
  const wire = encodeFrame(
    { type: FrameType.BEACON, srcId: 7, dstId: 8, seq: 1, flags: 0 },
    new Uint8Array(),
  );
  assert.equal(wire[0], FRAME_MAGIC);
});
