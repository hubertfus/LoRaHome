import { strict as assert } from 'node:assert';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';

import { CborError, CborReader, CborWriter } from '../src/cbor.js';
import { MAX_PAYLOAD } from '../src/frame.js';
import {
  CAP_FLAG_CONFIGURED,
  CAP_MAX,
  CAP_WIRE_BUDGET,
  CAP_WIRE_WORST_CASE,
  BusType,
  capabilityReportsEqual,
  decodeCapabilityReport,
  encodeCapabilityReport,
  type CapabilityReport,
} from '../src/capability.js';

/**
 * Fixtures are read from source, not from dist: they are shared data that the
 * C-side checks read from the same path, and tsc has no reason to copy them.
 * Walking up to package.json keeps this working from test/ or dist/test/.
 */
const FIXTURES = (() => {
  let dir = dirname(fileURLToPath(import.meta.url));
  while (!existsSync(join(dir, 'package.json'))) {
    const parent = dirname(dir);
    if (parent === dir) throw new Error('could not locate package root');
    dir = parent;
  }
  return join(dir, 'test', 'fixtures');
})();

const hex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex').toUpperCase();
const unhex = (text: string) => Uint8Array.from(Buffer.from(text, 'hex'));

/* -------------------------------------------------------------------------- */
/* CBOR primitives                                                            */
/* -------------------------------------------------------------------------- */

/**
 * Canonical widths, at every seam.
 *
 * These are the values where the argument encoding changes size. Both languages
 * must pick the same width for the same number or the cross-language byte
 * comparison — the strongest check in the suite — is comparing two encoders
 * that merely happen to agree on the corpus somebody chose.
 */
test('cbor encodes integers in the shortest form', () => {
  const cases: [number, string][] = [
    [0, '00'],
    [1, '01'],
    [23, '17'],
    [24, '1818'],
    [255, '18FF'],
    [256, '190100'],
    [65535, '19FFFF'],
    [65536, '1A00010000'],
    [4294967295, '1AFFFFFFFF'],
  ];

  for (const [value, expected] of cases) {
    assert.equal(hex(new CborWriter().uint(value).finish()), expected, `uint ${value}`);
  }
});

test('cbor encodes negative integers as -1 - n', () => {
  assert.equal(hex(new CborWriter().int(-1).finish()), '20');
  assert.equal(hex(new CborWriter().int(-24).finish()), '37');
  assert.equal(hex(new CborWriter().int(-25).finish()), '3818');
  assert.equal(hex(new CborWriter().int(-256).finish()), '38FF');

  const reader = new CborReader(unhex('38FF'));
  assert.equal(reader.int(), -256);
});

test('cbor round-trips containers', () => {
  const writer = new CborWriter();
  writer.map(2).uint(1).uint(42).uint(2).array(3).uint(7).int(-7).bytes(Uint8Array.of(0xde, 0xad));

  const reader = new CborReader(writer.finish());
  assert.equal(reader.map(), 2);
  assert.equal(reader.uint(), 1);
  assert.equal(reader.uint(), 42);
  assert.equal(reader.uint(), 2);
  assert.equal(reader.array(), 3);
  assert.equal(reader.uint(), 7);
  assert.equal(reader.int(), -7);
  assert.deepEqual([...reader.bytes()], [0xde, 0xad]);
  assert.equal(reader.done, true);
});

/**
 * Everything outside the subset is refused, not guessed at.
 *
 * Indefinite lengths in particular: an indefinite item is a stream with no
 * stated end, and the C side reads into a fixed buffer. Accepting one would
 * mean trusting a sender to terminate it.
 */
test('cbor refuses what is not in the subset', () => {
  assert.throws(() => new CborReader(unhex('5F')).uint(), CborError); // indefinite bytes
  assert.throws(() => new CborReader(unhex('9F')).array(), CborError); // indefinite array
  assert.throws(() => new CborReader(unhex('FB')).int(), CborError); // double
  assert.throws(() => new CborReader(unhex('C0')).uint(), CborError); // tag
  assert.throws(() => new CborReader(unhex('1C')).uint(), CborError); // reserved info 28
});

test('cbor refuses a truncated argument', () => {
  assert.throws(() => new CborReader(unhex('19FF')).uint(), CborError);
  assert.throws(() => new CborReader(unhex('')).uint(), CborError);
});

/**
 * A declared length larger than the buffer is refused at the header.
 *
 * Every item costs at least one byte, so a count above the bytes remaining
 * cannot be honest. Without this a crafted frame drives a caller's loop far
 * past the data it was given.
 */
test('cbor refuses a container longer than its buffer', () => {
  assert.throws(() => new CborReader(unhex('9AFFFFFFFF')).array(), CborError);
  assert.throws(() => new CborReader(unhex('BAFFFFFFFF')).map(), CborError);
});

test('cbor skip steps over nested containers', () => {
  const writer = new CborWriter();
  writer.array(2);
  writer.map(1).uint(9).array(2).uint(1).uint(2);
  writer.uint(77);

  const reader = new CborReader(writer.finish());
  assert.equal(reader.array(), 2);
  reader.skip(); // the whole nested map
  assert.equal(reader.uint(), 77);
  assert.equal(reader.done, true);
});

test('cbor skip refuses nesting deeper than the cap', () => {
  const writer = new CborWriter();
  for (let i = 0; i < 12; i++) writer.array(1);
  writer.uint(0);

  assert.throws(() => new CborReader(writer.finish()).skip(), CborError);
});

/* -------------------------------------------------------------------------- */
/* Capability reports                                                         */
/* -------------------------------------------------------------------------- */

const realistic: CapabilityReport = {
  fwVersion: 0x00040000,
  freeHeapKb: 192,
  components: [
    {
      driverTypeId: 16,
      busAddr: 0x76,
      busType: BusType.I2C,
      channelCount: 4,
      flags: CAP_FLAG_CONFIGURED,
    },
    { driverTypeId: 17, busAddr: 4, busType: BusType.GPIO, channelCount: 2, flags: 0 },
  ],
};

test('capability report round-trips', () => {
  const decoded = decodeCapabilityReport(encodeCapabilityReport(realistic));
  assert.equal(capabilityReportsEqual(decoded, realistic), true);
});

test('an empty report round-trips', () => {
  const empty: CapabilityReport = { fwVersion: 1, freeHeapKb: 0, components: [] };
  assert.deepEqual(decodeCapabilityReport(encodeCapabilityReport(empty)), empty);
});

/**
 * The budget the roadmap gates the release on.
 *
 * A full report has to fit one frame. Fragmenting a discovery response would
 * make discovery depend on reassembly, which depends on an ARQ — and all of
 * that is what you would be debugging when a new node fails to appear.
 */
test('the largest report the protocol permits fits one frame', () => {
  // Every field at the widest value its type allows: 12 B per component (1
  // array header + 3 type id + 2 addr + 2 bus + 2 channels + 2 flags) and 13 B
  // of envelope, so 8 x 12 + 13 = 109. There is no report this codec can
  // produce that is larger.
  //
  // Worked out as 93 first, assuming bus type, channel count and flags would
  // stay in the inline form. They do in every report the system generates and
  // they do not in every report it can be sent — the decoder accepts a full
  // byte in each — and the bound a receive buffer needs is the reachable one.
  const worst: CapabilityReport = {
    fwVersion: 0xffffffff,
    freeHeapKb: 0xffff,
    components: Array.from({ length: CAP_MAX }, () => ({
      driverTypeId: 0xffff,
      busAddr: 0xff,
      busType: 0xff as BusType,
      channelCount: 0xff,
      flags: 0xff,
    })),
  };

  const encoded = encodeCapabilityReport(worst);
  assert.equal(encoded.length, CAP_WIRE_WORST_CASE, 'the arithmetic above should be exact');
  assert.equal(capabilityReportsEqual(decodeCapabilityReport(encoded), worst), true);

  // The requirement the budget exists to express: a discovery response must
  // never fragment. Fragmenting it would make discovery depend on reassembly,
  // which depends on an ARQ, and all of that is what you would be debugging
  // when a new node fails to appear.
  assert.ok(encoded.length <= MAX_PAYLOAD, `${encoded.length} B must fit one frame`);

  // And the roadmap's 100 B, against the widest report the system can actually
  // emit: bus types from a five-value enum, channel counts in single digits,
  // one flag bit defined. This is the number that constrains adding drivers.
  const realisticFull: CapabilityReport = {
    fwVersion: 0xffffffff,
    freeHeapKb: 0xffff,
    components: Array.from({ length: CAP_MAX }, () => ({
      driverTypeId: 0xffff,
      busAddr: 0x77,
      busType: BusType.ADC,
      channelCount: 8,
      flags: CAP_FLAG_CONFIGURED,
    })),
  };
  const realistic = encodeCapabilityReport(realisticFull);
  assert.ok(
    realistic.length <= CAP_WIRE_BUDGET,
    `a full realistic report is ${realistic.length} B, budget ${CAP_WIRE_BUDGET} B`,
  );
});

test('a report beyond the component limit is refused on both sides', () => {
  const tooMany: CapabilityReport = {
    fwVersion: 0,
    freeHeapKb: 0,
    components: Array.from({ length: CAP_MAX + 1 }, () => ({
      driverTypeId: 1,
      busAddr: 1,
      busType: BusType.I2C,
      channelCount: 1,
      flags: 0,
    })),
  };
  assert.throws(() => encodeCapabilityReport(tooMany), RangeError);

  // And a hand-built message claiming nine components is refused on decode,
  // rather than silently truncated to eight — a host acting on the first eight
  // of nine would show a device with a part missing and no sign of it.
  const writer = new CborWriter();
  writer.map(1).uint(3).array(9);
  for (let i = 0; i < 9; i++) writer.array(5).uint(1).uint(1).uint(0).uint(1).uint(0);
  assert.throws(() => decodeCapabilityReport(writer.finish()), RangeError);
});

/**
 * Forward compatibility, in both places a newer node can add something.
 *
 * Without this, every protocol addition is a flag day: a host meeting one
 * unknown key would refuse the whole report and the node would simply not
 * appear, which looks like a radio fault.
 */
test('unknown keys and extra component fields are skipped', () => {
  const writer = new CborWriter();
  writer.map(4);
  writer.uint(1).uint(0x00050000);
  writer.uint(2).uint(150);
  writer.uint(99).array(2).uint(1).uint(2); // a key from a newer protocol
  writer.uint(3).array(1);
  writer.array(7).uint(16).uint(0x76).uint(0).uint(4).uint(1).uint(123).uint(456);

  const decoded = decodeCapabilityReport(writer.finish());
  assert.equal(decoded.fwVersion, 0x00050000);
  assert.equal(decoded.freeHeapKb, 150);
  assert.equal(decoded.components.length, 1);
  assert.equal(decoded.components[0]!.driverTypeId, 16);
  assert.equal(decoded.components[0]!.channelCount, 4);
});

test('a component with too few fields is refused', () => {
  const writer = new CborWriter();
  writer.map(1).uint(3).array(1);
  writer.array(3).uint(16).uint(0x76).uint(0);
  assert.throws(() => decodeCapabilityReport(writer.finish()), RangeError);
});

/**
 * The golden vector. This file is sacred, in the roadmap's words.
 *
 * A round-trip test proves the two ends of one implementation agree with each
 * other; it says nothing about whether the bytes on the wire are the bytes the
 * protocol specifies. Only a hand-checked hex dump does that, and only if it is
 * never regenerated from the code it is meant to be checking.
 */
test('the golden vector matches byte for byte', () => {
  const golden = JSON.parse(readFileSync(join(FIXTURES, 'capability.json'), 'utf8')) as {
    cases: { name: string; report: CapabilityReport; hex: string }[];
  };

  for (const testCase of golden.cases) {
    assert.equal(
      hex(encodeCapabilityReport(testCase.report)),
      testCase.hex,
      `golden vector "${testCase.name}"`,
    );
    assert.equal(
      capabilityReportsEqual(decodeCapabilityReport(unhex(testCase.hex)), testCase.report),
      true,
      `golden vector "${testCase.name}" decodes back`,
    );
  }
});
