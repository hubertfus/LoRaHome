/**
 * Generates the shared CRC16 vector file consumed by both language test suites.
 *
 * `pnpm --filter @lorahome/protocol gen:vectors`
 *
 * The vectors are deterministic: a small xorshift PRNG with a fixed seed, not
 * Math.random. A vector file that changes on every run cannot be committed, and
 * a cross-language contract that both sides regenerate independently proves
 * nothing. Both sides must read the *same bytes*.
 */
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { crc16, CRC16_CHECK, crc16Reference } from '../crc16.js';
import { LORA_MTU } from '../frame.js';

/** xorshift32 — tiny, deterministic, and identical across platforms. */
function makeRng(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state ^= state << 13;
    state >>>= 0;
    state ^= state >>> 17;
    state ^= state << 5;
    state >>>= 0;
    return state;
  };
}

const VECTOR_COUNT = 500;
const SEED = 0x10ca1f05;

export interface CrcVectorFile {
  description: string;
  algorithm: {
    name: string;
    poly: string;
    init: string;
    reflect_in: boolean;
    reflect_out: boolean;
    xor_out: string;
    check: string;
  };
  seed: number;
  vectors: { len: number; data_hex: string; crc: string }[];
}

export function buildVectorFile(): CrcVectorFile {
  const rng = makeRng(SEED);
  const vectors: CrcVectorFile['vectors'] = [];

  // Deliberate edge cases first, then pseudorandom ones across the length range.
  const fixedLengths = [0, 1, 2, 3, 8, 9, 10, 15, 16, 17, 220, LORA_MTU];

  for (let i = 0; i < VECTOR_COUNT; i++) {
    const len = i < fixedLengths.length ? fixedLengths[i]! : rng() % (LORA_MTU + 1);
    const data = new Uint8Array(len);
    for (let b = 0; b < len; b++) data[b] = rng() & 0xff;

    const value = crc16(data);
    // Cross-check the table implementation against the bit-by-bit definition as
    // the file is built, so a bad table can never be baked into the contract.
    const reference = crc16Reference(data);
    if (value !== reference) {
      throw new Error(
        `table/reference mismatch at vector ${i} (len ${len}): ` +
          `0x${value.toString(16)} vs 0x${reference.toString(16)}`,
      );
    }

    vectors.push({
      len,
      data_hex: Buffer.from(data).toString('hex').toUpperCase(),
      crc: `0x${value.toString(16).toUpperCase().padStart(4, '0')}`,
    });
  }

  return {
    description:
      'Shared CRC-16/CCITT-FALSE vectors. Generated from packages/protocol by ' +
      '`pnpm gen:vectors` with a fixed seed. Both the TS and C test suites read ' +
      'this exact file — do not regenerate to make a failing test pass.',
    algorithm: {
      name: 'CRC-16/CCITT-FALSE',
      poly: '0x1021',
      init: '0xFFFF',
      reflect_in: false,
      reflect_out: false,
      xor_out: '0x0000',
      check: `0x${CRC16_CHECK.toString(16).toUpperCase()}`,
    },
    seed: SEED,
    vectors,
  };
}

function repoRoot(): string {
  let dir = dirname(fileURLToPath(import.meta.url));
  for (;;) {
    const parent = dirname(dir);
    if (dir.endsWith('packages')) return parent;
    if (parent === dir) throw new Error('repo root not found');
    dir = parent;
  }
}

const OUTPUT = join(
  repoRoot(),
  'packages',
  'protocol',
  'test',
  'fixtures',
  'crc16-vectors.json',
);

const invokedPath = process.argv[1];
if (invokedPath !== undefined && invokedPath.endsWith('gen-vectors.js')) {
  const file = buildVectorFile();
  mkdirSync(dirname(OUTPUT), { recursive: true });
  writeFileSync(OUTPUT, JSON.stringify(file, null, 2) + '\n', 'utf8');
  console.log(`wrote ${file.vectors.length} vectors to test/fixtures/crc16-vectors.json`);
}
