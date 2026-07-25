import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath, pathToFileURL } from 'node:url';
import fc from 'fast-check';
import { crc16, CRC16_CHECK, CRC16_INIT, crc16Reference } from '../src/crc16.js';
import { LORA_MTU } from '../src/frame.js';

function repoRoot(): string {
  let dir = dirname(fileURLToPath(import.meta.url));
  while (!existsSync(join(dir, 'pnpm-workspace.yaml'))) {
    const parent = dirname(dir);
    if (parent === dir) throw new Error('repo root not found');
    dir = parent;
  }
  return dir;
}

const ROOT = repoRoot();

interface VectorFile {
  algorithm: Record<string, string | boolean>;
  vectors: { len: number; data_hex: string; crc: string }[];
}

const vectorFile = JSON.parse(
  readFileSync(join(ROOT, 'packages/protocol/test/fixtures/crc16-vectors.json'), 'utf8'),
) as VectorFile;

const unhex = (s: string): Uint8Array => new Uint8Array(Buffer.from(s, 'hex'));

// ---------------------------------------------------------------------------
// Algorithm identity
// ---------------------------------------------------------------------------

test('CRC("123456789") is the CCITT-FALSE check value', () => {
  // 0x29B1 is the catalogue check value for poly 0x1021 / init 0xFFFF. The
  // roadmap quotes 0xE5CC, which is CRC-16/AUG-CCITT (init 0x1D0F) — a
  // different algorithm. The parameter set, the algorithm name and every
  // committed golden frame all agree on init 0xFFFF, so the constant is what
  // gives. See the T0.5 commit message.
  assert.equal(crc16(new TextEncoder().encode('123456789')), 0x29b1);
  assert.equal(CRC16_CHECK, 0x29b1);
});

test('an empty buffer yields the init value', () => {
  assert.equal(crc16(new Uint8Array()), CRC16_INIT);
});

test('the vector file declares the parameters we actually implement', () => {
  assert.deepEqual(vectorFile.algorithm, {
    name: 'CRC-16/CCITT-FALSE',
    poly: '0x1021',
    init: '0xFFFF',
    reflect_in: false,
    reflect_out: false,
    xor_out: '0x0000',
    check: '0x29B1',
  });
});

// ---------------------------------------------------------------------------
// Nibble table vs. the polynomial definition
// ---------------------------------------------------------------------------

test('the nibble table agrees with the bit-by-bit reference on 1000 random buffers', () => {
  // Guards the optimisation, not the algorithm: if someone mistypes an entry in
  // the 16-word table, this is what catches it.
  fc.assert(
    fc.property(fc.uint8Array({ maxLength: LORA_MTU }), (data) => {
      assert.equal(crc16(data), crc16Reference(data));
    }),
    { numRuns: 1000 },
  );
});

// ---------------------------------------------------------------------------
// Shared vectors
// ---------------------------------------------------------------------------

test(`all ${vectorFile.vectors.length} shared vectors match`, () => {
  let checked = 0;
  for (const [index, vector] of vectorFile.vectors.entries()) {
    const data = unhex(vector.data_hex);
    assert.equal(data.length, vector.len, `vector ${index}: declared length disagrees with data`);
    assert.equal(
      `0x${crc16(data).toString(16).toUpperCase().padStart(4, '0')}`,
      vector.crc,
      `vector ${index} (len ${vector.len})`,
    );
    checked++;
  }
  assert.equal(checked, 500, 'expected the full 500-vector contract');
});

test('the vector set covers the edges and the full MTU range', () => {
  const lengths = vectorFile.vectors.map((v) => v.len);
  for (const edge of [0, 1, 2, LORA_MTU]) {
    assert.ok(lengths.includes(edge), `no vector of length ${edge}`);
  }
  assert.ok(Math.max(...lengths) === LORA_MTU);
});

// ---------------------------------------------------------------------------
// Error detection
// ---------------------------------------------------------------------------

test('every single-bit flip in a 230 B buffer changes the CRC', () => {
  const data = new Uint8Array(LORA_MTU);
  for (let i = 0; i < data.length; i++) data[i] = (i * 37 + 11) & 0xff;
  const baseline = crc16(data);

  let detected = 0;
  let total = 0;
  for (let byte = 0; byte < data.length; byte++) {
    for (let bit = 0; bit < 8; bit++) {
      const corrupted = Uint8Array.from(data);
      corrupted[byte] = corrupted[byte]! ^ (1 << bit);
      total++;
      if (crc16(corrupted) !== baseline) detected++;
    }
  }

  assert.equal(total, LORA_MTU * 8);
  assert.equal(detected, total, `${total - detected} single-bit flips went undetected`);
});

test('appending or truncating a buffer changes the CRC', () => {
  const data = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
  const base = crc16(data);
  assert.notEqual(crc16(data.subarray(0, 7)), base);
  assert.notEqual(crc16(new Uint8Array([...data, 0])), base);
});

test('a trailing zero byte is not transparent', () => {
  // A CRC with a zero init would treat leading zeros as invisible; init 0xFFFF
  // is chosen precisely so length changes are detectable.
  assert.notEqual(crc16(new Uint8Array([0])), crc16(new Uint8Array([0, 0])));
});

// ---------------------------------------------------------------------------
// Cross-language: the same vectors through the real C implementation.
// ---------------------------------------------------------------------------

/**
 * Host toolchain, resolved by tools/host-cc.mjs.
 *
 * Imported at run time rather than reimplemented here. This test used to carry
 * its own three-line PATH search, which meant it skipped on any machine where
 * the compiler was not on PATH — MSYS2 on Windows, for instance, keeps its
 * `bin` off it deliberately. Three copies of "where is gcc" in one repo is the
 * same drift hazard as three copies of a wire format.
 */
interface HostToolchain {
  id: string;
  kind: string;
  version: string;
  env: NodeJS.ProcessEnv;
  compile(options: {
    sources: string[];
    includeDirs?: string[];
    output: string;
    optimize?: string;
  }): void;
}

const hostCc = (await import(
  pathToFileURL(join(ROOT, 'tools/host-cc.mjs')).href
)) as {
  findHostToolchain(): HostToolchain | null;
  exeName(stem: string): string;
};

const toolchain = hostCc.findHostToolchain();

test(
  'C and TypeScript agree on all 500 vectors',
  {
    // Skipped loudly rather than silently passing: without a host compiler the
    // C implementation is genuinely unverified, and a green tick here would be
    // a lie about the strongest guarantee in this task.
    skip: toolchain === null ? 'no host C compiler (cross compilers cannot execute here)' : false,
  },
  () => {
    const workDir = mkdtempSync(join(tmpdir(), 'lh-crc-'));
    try {
      const binary = join(workDir, hostCc.exeName('crc16_cli'));
      toolchain!.compile({
        sources: [
          join(ROOT, 'firmware/common/test/crc16_cli.c'),
          join(ROOT, 'firmware/common/src/crc16.c'),
        ],
        includeDirs: [join(ROOT, 'firmware/common/include')],
        output: binary,
        optimize: 'O2',
      });

      const input = vectorFile.vectors.map((v) => v.data_hex).join('\n') + '\n';
      // The toolchain's env: a MinGW binary needs its DLLs on PATH to start.
      const output = execFileSync(binary, { input, encoding: 'utf8', env: toolchain!.env });
      const fromC = output.trim().split(/\r?\n/);

      assert.equal(fromC.length, vectorFile.vectors.length, 'C produced the wrong number of results');
      for (const [index, vector] of vectorFile.vectors.entries()) {
        const expected = vector.crc.replace('0x', '');
        assert.equal(fromC[index], expected, `vector ${index} (len ${vector.len}) disagrees`);
      }
    } finally {
      rmSync(workDir, { recursive: true, force: true });
    }
  },
);
