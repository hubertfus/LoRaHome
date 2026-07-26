import assert from 'node:assert/strict';
import { test } from 'node:test';
import fc from 'fast-check';
import { crc16 } from '../src/crc16.js';
import {
  decodeFragHeader,
  encodeFragHeader,
  FRAG_CONFIG_MAX,
  FRAG_HDR_SIZE,
  FRAG_PAYLOAD_MAX,
  FRAG_TIMEOUT_MS,
  FragResult,
  Reassembler,
  fragmentCount,
  splitConfig,
} from '../src/fragment.js';

/**
 * Tests for the Host's fragmenter (T2.3).
 *
 * The C twin carries the volume work and tools/check-frag-cross.mjs crosses the
 * two on 200 configs in both directions. What is pinned here is the behaviour
 * the Host owns alone: refusing an oversized config before a single frame is
 * transmitted, and the header's byte order — the one field group in this
 * protocol that the roadmap sketched as a packed struct, which would have been
 * little-endian on both our platforms and wrong against everything else.
 */

const configOf = (length: number, seed = 1): Uint8Array =>
  Uint8Array.from({ length }, (_, i) => (i * 31 + seed) & 0xff);

test('the fragment header is big-endian, like every other multi-byte field', () => {
  const out = new Uint8Array(FRAG_HDR_SIZE);
  encodeFragHeader(
    { cfgId: 0x1234, fragIndex: 2, fragTotal: 5, totalLen: 0x0abc, crcTotal: 0xbeef },
    out,
  );
  assert.deepEqual(Array.from(out), [0x12, 0x34, 0x02, 0x05, 0x0a, 0xbc, 0xbe, 0xef]);
  assert.deepEqual(decodeFragHeader(out), {
    cfgId: 0x1234,
    fragIndex: 2,
    fragTotal: 5,
    totalLen: 0x0abc,
    crcTotal: 0xbeef,
  });
});

test('fragment counts at the boundaries', () => {
  assert.equal(fragmentCount(0), 1, 'an empty config is still one fragment');
  assert.equal(fragmentCount(1), 1);
  assert.equal(fragmentCount(FRAG_PAYLOAD_MAX), 1);
  assert.equal(fragmentCount(FRAG_PAYLOAD_MAX + 1), 2);
  assert.equal(fragmentCount(FRAG_CONFIG_MAX), 8);
  assert.equal(fragmentCount(FRAG_CONFIG_MAX + 1), 0, 'past the maximum there is no answer');
});

test('an oversized config is refused here, not discovered on a roof', () => {
  // The whole point of the check being on this side: the alternative is a
  // device one fragment into a transaction that can never complete.
  assert.throws(() => splitConfig(1, configOf(FRAG_CONFIG_MAX + 1)), RangeError);
  assert.equal(splitConfig(1, configOf(FRAG_CONFIG_MAX)).length, 8);
});

test('every fragment carries the CRC of the whole config, not of itself', () => {
  const config = configOf(600);
  const fragments = splitConfig(0x4242, config);
  assert.equal(fragments.length, 3);

  for (const [index, fragment] of fragments.entries()) {
    const hdr = decodeFragHeader(fragment);
    assert.equal(hdr.cfgId, 0x4242);
    assert.equal(hdr.fragIndex, index);
    assert.equal(hdr.fragTotal, 3);
    assert.equal(hdr.totalLen, 600);
    assert.equal(hdr.crcTotal, crc16(config), 'crc_total must cover the assembled config');
  }
});

test('a full-size config round-trips through eight fragments', () => {
  const config = configOf(FRAG_CONFIG_MAX, 7);
  const reassembler = new Reassembler();
  let result = FragResult.NEED_MORE;

  for (const fragment of splitConfig(0x1234, config)) {
    result = reassembler.feed(fragment, 1000);
  }

  assert.equal(result, FragResult.COMPLETE);
  assert.deepEqual(Array.from(reassembler.assembled()), Array.from(config));
  assert.equal(reassembler.stats.completed, 1);
});

test('a duplicate fragment changes nothing', () => {
  const config = configOf(600);
  const fragments = splitConfig(1, config);
  const reassembler = new Reassembler();

  reassembler.feed(fragments[0]!, 1000);
  assert.equal(reassembler.feed(fragments[0]!, 1100), FragResult.DUPLICATE);
  assert.equal(reassembler.stats.duplicates, 1);

  reassembler.feed(fragments[1]!, 1200);
  assert.equal(reassembler.feed(fragments[2]!, 1300), FragResult.COMPLETE);
  assert.deepEqual(Array.from(reassembler.assembled()), Array.from(config));
});

test('a fragment from another transaction never touches the live one', () => {
  // R2.3: every fragment here is well-formed and passed its frame CRC. What
  // makes the combination poison is that they describe different configs.
  const wanted = configOf(400, 1);
  const other = configOf(400, 200);
  const reassembler = new Reassembler();

  const wantedFragments = splitConfig(0x4444, wanted);
  reassembler.feed(wantedFragments[0]!, 1000);

  assert.equal(reassembler.feed(splitConfig(0x5555, other)[1]!, 1100), FragResult.ERR_FOREIGN);
  assert.equal(reassembler.stats.foreign, 1);

  assert.equal(reassembler.feed(wantedFragments[1]!, 1200), FragResult.COMPLETE);
  assert.deepEqual(Array.from(reassembler.assembled()), Array.from(wanted));
});

test('the whole-config CRC catches contents the headers cannot', () => {
  const config = configOf(400, 3);
  const fragments = splitConfig(0x7777, config);
  const forged = Uint8Array.from(fragments[1]!);
  forged[FRAG_HDR_SIZE] = forged[FRAG_HDR_SIZE]! ^ 0xff; // payload corrupted, header intact

  const reassembler = new Reassembler();
  reassembler.feed(fragments[0]!, 1000);
  assert.equal(reassembler.feed(forged, 1100), FragResult.ERR_CRC);
  assert.equal(reassembler.stats.crcFail, 1);
});

test('malformed headers store nothing', () => {
  const fragments = splitConfig(0x8888, configOf(400));
  const reassembler = new Reassembler();

  assert.equal(reassembler.feed(fragments[0]!.subarray(0, 4), 1000), FragResult.ERR_HEADER);

  const zeroTotal = Uint8Array.from(fragments[0]!);
  zeroTotal[3] = 0;
  assert.equal(reassembler.feed(zeroTotal, 1000), FragResult.ERR_HEADER);

  const badIndex = Uint8Array.from(fragments[0]!);
  badIndex[2] = 9;
  assert.equal(reassembler.feed(badIndex, 1000), FragResult.ERR_HEADER);

  const inconsistent = Uint8Array.from(fragments[0]!);
  inconsistent[3] = 5; // fragTotal that does not match totalLen
  assert.equal(reassembler.feed(inconsistent, 1000), FragResult.ERR_HEADER);

  // A slice shorter than its position implies would leave stale bytes inside a
  // config that then passes its CRC only by luck.
  assert.equal(
    reassembler.feed(fragments[0]!.subarray(0, fragments[0]!.length - 1), 1000),
    FragResult.ERR_HEADER,
  );
});

test('an incomplete transaction expires and frees the slot', () => {
  // R2.4: measured against a real clock, so one lost fragment costs 30 seconds
  // rather than blocking configuration until someone power-cycles the device.
  const start = 5_000_000;
  const fragments = splitConfig(0x9999, configOf(600));
  const reassembler = new Reassembler();

  reassembler.feed(fragments[0]!, start);
  assert.equal(reassembler.tick(start + FRAG_TIMEOUT_MS * 1000 - 1), false);
  assert.equal(reassembler.tick(start + FRAG_TIMEOUT_MS * 1000), true);
  assert.equal(reassembler.stats.timeouts, 1);

  let result = FragResult.NEED_MORE;
  for (const fragment of splitConfig(0xaaaa, configOf(300))) {
    result = reassembler.feed(fragment, start + 60_000_000);
  }
  assert.equal(result, FragResult.COMPLETE, 'the slot must be usable after a timeout');
});

test('property: any config up to the maximum survives any delivery order', () => {
  fc.assert(
    fc.property(
      fc.uint8Array({ minLength: 0, maxLength: FRAG_CONFIG_MAX }),
      fc.integer({ min: 0, max: 0xffff }),
      fc.integer({ min: 1, max: 0xffffffff }),
      (config, cfgId, seed) => {
        const fragments = splitConfig(cfgId, config);

        // Deterministic shuffle from the generated seed, with one duplicate
        // injected — an ARQ over a lossy link produces both.
        let rng = seed;
        const order = fragments.map((_, i) => i);
        for (let i = order.length; i > 1; i--) {
          rng ^= rng << 13;
          rng ^= rng >>> 17;
          rng ^= rng << 5;
          const j = (rng >>> 0) % i;
          [order[i - 1], order[j]] = [order[j]!, order[i - 1]!];
        }
        if (order.length > 1) order.splice(1, 0, order[0]!);

        const reassembler = new Reassembler();
        let result = FragResult.NEED_MORE;
        for (const position of order) result = reassembler.feed(fragments[position]!, 1000);

        if (result !== FragResult.COMPLETE) return false;
        const assembled = reassembler.assembled();
        if (assembled.length !== config.length) return false;
        return assembled.every((byte, i) => byte === config[i]);
      },
    ),
    { numRuns: 300 },
  );
});
