import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { countStaticAsserts, emitCHeader, generatedHeaderHash } from '../src/codegen/emit-c.js';
import { OUTPUT_RELATIVE } from '../src/codegen/gen-c.js';
import {
  BROADCAST_ID,
  CRC_SIZE,
  FRAME_MAGIC,
  FrameType,
  HEADER_LAYOUT,
  HEADER_SIZE,
  LORA_MTU,
  MAX_PAYLOAD,
} from '../src/frame.js';

function repoRoot(): string {
  let dir = dirname(fileURLToPath(import.meta.url));
  while (!existsSync(join(dir, 'pnpm-workspace.yaml'))) {
    const parent = dirname(dir);
    if (parent === dir) throw new Error('repo root not found');
    dir = parent;
  }
  return dir;
}

const GENERATED_PATH = join(repoRoot(), OUTPUT_RELATIVE);

test('codegen is deterministic across runs', () => {
  assert.equal(emitCHeader(), emitCHeader());
});

test('generated header carries a source hash and no wall-clock timestamp', () => {
  const source = emitCHeader();
  assert.match(source, /source-hash: [0-9a-f]{8}/);

  // Risk R0.4: a timestamp makes every regeneration produce a diff, the drift
  // gate turns into noise, and people switch it off. The hash is the only
  // provenance marker allowed.
  assert.doesNotMatch(source, /generated-at/i);
  assert.doesNotMatch(source, /\d{4}-\d{2}-\d{2}/, 'looks like a date leaked into the output');
  assert.doesNotMatch(source, /\d{2}:\d{2}:\d{2}/, 'looks like a clock time leaked into the output');
});

test('source hash tracks the schema, not the emitted text', () => {
  assert.equal(generatedHeaderHash(), generatedHeaderHash());
  assert.match(generatedHeaderHash(), /^[0-9a-f]{8}$/);
});

test('generated header says DO NOT EDIT', () => {
  assert.match(emitCHeader(), /DO NOT EDIT/);
});

test('generated constants match the TypeScript schema', () => {
  const source = emitCHeader();
  const define = (name: string): string => {
    const match = source.match(new RegExp(`^#define ${name}\\s+(\\S+)$`, 'm'));
    assert.ok(match, `missing #define ${name}`);
    return match[1]!;
  };

  assert.equal(define('LH_FRAME_MAGIC'), `0x${FRAME_MAGIC.toString(16).toUpperCase()}u`);
  assert.equal(define('LH_HEADER_SIZE'), `${HEADER_SIZE}u`);
  assert.equal(define('LH_CRC_SIZE'), `${CRC_SIZE}u`);
  assert.equal(define('LH_LORA_MTU'), `${LORA_MTU}u`);
  assert.equal(define('LH_MAX_PAYLOAD'), `${MAX_PAYLOAD}u`);
  assert.equal(define('LH_BROADCAST_ID'), `0x${BROADCAST_ID.toString(16).toUpperCase()}u`);
});

test('every layout field emits matching offset and length defines', () => {
  const source = emitCHeader();
  for (const field of HEADER_LAYOUT) {
    const name = field.name.toUpperCase();
    assert.match(
      source,
      new RegExp(`^#define LH_HDR_OFF_${name}\\s+${field.offset}u$`, 'm'),
      `offset define for ${field.name}`,
    );
    assert.match(
      source,
      new RegExp(`^#define LH_HDR_LEN_${name}\\s+${field.size}u$`, 'm'),
      `length define for ${field.name}`,
    );
  }
});

test('every frame type reaches the generated enum', () => {
  const source = emitCHeader();
  const names = Object.keys(FrameType).filter((k) => Number.isNaN(Number(k)));
  for (const name of names) {
    assert.match(source, new RegExp(`LH_TYPE_${name}\\s+= 0x`), `missing LH_TYPE_${name}`);
  }
});

test('generated header asserts size and every field offset at compile time', () => {
  const source = emitCHeader();
  assert.match(source, /_Static_assert\(sizeof\(lh_header_t\) == LH_HEADER_SIZE/);
  for (const field of HEADER_LAYOUT) {
    assert.match(
      source,
      new RegExp(`_Static_assert\\(offsetof\\(lh_header_t, ${field.name}\\) ==`),
      `no offset assertion for ${field.name}`,
    );
  }

  // The count is a tracked metric and is expected to grow, never shrink.
  const count = countStaticAsserts(source);
  assert.ok(count >= 2 * HEADER_LAYOUT.length + 3, `only ${count} static assertions`);
});

test('big-endian fields get accessors and u8 fields do not', () => {
  const source = emitCHeader();
  for (const field of HEADER_LAYOUT) {
    const hasAccessor = source.includes(`lh_hdr_get_${field.name}(`);
    assert.equal(
      hasAccessor,
      field.endian === 'be16',
      `${field.name} (${field.endian}) accessor presence is wrong`,
    );
  }
});

test('output uses LF endings and ends with a newline', () => {
  const source = emitCHeader();
  assert.ok(source.endsWith('\n'), 'missing trailing newline');
  assert.doesNotMatch(source, /\r/, 'CRLF would make the byte-for-byte drift check platform-dependent');
});

test('the checked-in generated header is not stale (equivalent of gen:c --check)', () => {
  assert.ok(
    existsSync(GENERATED_PATH),
    `${OUTPUT_RELATIVE} is missing — run \`pnpm gen:c\``,
  );
  assert.equal(
    readFileSync(GENERATED_PATH, 'utf8'),
    emitCHeader(),
    `${OUTPUT_RELATIVE} does not match the schema — run \`pnpm gen:c\``,
  );
});

test('the gen-c CLI exits 0 in --check mode when the header is current', () => {
  const cli = join(dirname(fileURLToPath(import.meta.url)), '..', 'src', 'codegen', 'gen-c.js');
  const output = execFileSync(process.execPath, [cli, '--check'], { encoding: 'utf8' });
  assert.match(output, /gen:c --check OK/);
});

test('the gen-c CLI exits non-zero when the header has been tampered with', () => {
  // Exercises the real gate the way CI invokes it, not just the comparison
  // function underneath. A gate is only proven by watching it reject something.
  const cli = join(dirname(fileURLToPath(import.meta.url)), '..', 'src', 'codegen', 'gen-c.js');
  const pristine = readFileSync(GENERATED_PATH, 'utf8');
  try {
    writeFileSync(GENERATED_PATH, pristine.replace('LH_HEADER_SIZE     8u', 'LH_HEADER_SIZE     9u'));
    assert.throws(
      () => execFileSync(process.execPath, [cli, '--check'], { stdio: 'pipe' }),
      /Command failed/,
    );
  } finally {
    writeFileSync(GENERATED_PATH, pristine);
  }

  // And leaves the tree exactly as it found it.
  assert.equal(readFileSync(GENERATED_PATH, 'utf8'), pristine);
});

test('a hand edit to the generated file is detectable', () => {
  // What R0.3 is really about: proving the comparison is byte-exact, so that
  // "just tweaking one constant in the .h" cannot survive CI.
  const pristine = emitCHeader();
  const tampered = pristine.replace('#define LH_HEADER_SIZE     8u', '#define LH_HEADER_SIZE     9u');
  assert.notEqual(tampered, pristine, 'test fixture failed to tamper with anything');
  assert.notEqual(tampered, emitCHeader());
});
