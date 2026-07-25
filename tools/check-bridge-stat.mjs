/**
 * Cross-language verification of the Bridge's health payload.
 *
 * The Bridge encodes it in C and the Host decodes it in TypeScript, and the
 * numbers inside decide whether a release gets tagged. A silent disagreement
 * about field order would not produce an error — it would produce a
 * plausible-looking wrong heap figure, which is the worst possible failure mode
 * for a leak detector. So the same value grid goes through both.
 *
 * The grid is built to catch exactly that class of mistake: every field takes a
 * distinct value in at least one row, so two fields swapped in one language
 * cannot cancel out.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { exeName, findHostToolchain, spawnFailureDetail } from './host-cc.mjs';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));
const COMMON = join(REPO_ROOT, 'firmware', 'common');
const HOST_MODULE = join(
  REPO_ROOT, 'packages', 'host', 'dist', 'src', 'transport', 'bridge-stat.js',
);

/** Wire order. Must match lh_bridge_stat_t and bridge_stat_cli.c. */
const FIELDS = [
  'uptimeMs',
  'heapFreeInternal',
  'heapLargestBlock',
  'heapMinFreeEver',
  'serialFramesIn',
  'serialFramesOut',
  'radioFramesIn',
  'radioFramesOut',
  'rejectedCrc',
  'rejectedMagic',
  'rejectedLen',
  'rejectedDutyCycle',
  'radioTxErrors',
  'serialTxErrors',
  'slipFramesOk',
  'slipOverflow',
  'slipBadEscape',
  'slipDropped',
  'ringOverrun',
  'ringHwm',
  'ringCapacity',
];

/** The last two fields are uint16_t on the wire; everything before is uint32_t. */
const isU16 = (name) => name === 'ringHwm' || name === 'ringCapacity';
const cap = (name, value) => (isU16(name) ? value & 0xffff : value >>> 0);

if (!existsSync(HOST_MODULE)) {
  console.log('SKIPPED  packages/host is not built — run `pnpm -r build` first');
  console.log('LH_METRIC test.bridge_stat.cross_lang value=SKIPPED unit=count');
  process.exit(0);
}

const toolchain = findHostToolchain();
if (toolchain === null) {
  console.log('SKIPPED  no host C toolchain — the C encoder was not compared');
  console.log('LH_METRIC test.bridge_stat.cross_lang value=SKIPPED unit=count');
  process.exit(0);
}

const { encodeBridgeStat, decodeBridgeStat, BRIDGE_STAT_SIZE } = await import(
  pathToFileURL(HOST_MODULE).href
);

console.log(`LH_ENV toolchain.bridge_stat_check=${toolchain.version}`);

let rng = 0x1BADB002;
function nextRandom() {
  rng ^= rng << 13;
  rng ^= rng >>> 17;
  rng ^= rng << 5;
  return rng >>> 0;
}

function buildGrid() {
  const rows = [];

  // All zero, and all ones: the two rows where a field-order bug hides.
  rows.push(Object.fromEntries(FIELDS.map((f) => [f, 0])));
  rows.push(Object.fromEntries(FIELDS.map((f) => [f, cap(f, 0xffffffff)])));

  // One row per field with that field set and the rest zero. A swapped pair
  // shows up here as two mismatched rows, no matter what the values are.
  for (const [index, field] of FIELDS.entries()) {
    const row = Object.fromEntries(FIELDS.map((f) => [f, 0]));
    row[field] = cap(field, 0xA0000000 + index);
    rows.push(row);
  }

  // A realistic reading, so the common case is covered explicitly.
  rows.push({
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
  });

  while (rows.length < 200) {
    rows.push(Object.fromEntries(FIELDS.map((f) => [f, cap(f, nextRandom())])));
  }
  return rows;
}

const grid = buildGrid();
const workDir = mkdtempSync(join(tmpdir(), 'lh-bstat-'));
let agreed = 0;
const failures = [];

try {
  const binary = join(workDir, exeName('bridge_stat_cli'));
  try {
    toolchain.compile({
      sources: [
        join(COMMON, 'test', 'bridge_stat_cli.c'),
        join(COMMON, 'src', 'bridge_stat.c'),
      ],
      includeDirs: [join(COMMON, 'include')],
      output: binary,
      optimize: 'O2',
    });
  } catch (error) {
    console.error(`FAIL     could not build the bridge stat CLI\n${spawnFailureDetail(error)}`);
    process.exit(1);
  }

  const input = grid.map((row) => FIELDS.map((f) => row[f]).join(' ')).join('\n') + '\n';
  const fromC = execFileSync(binary, [], { input, encoding: 'utf8', env: toolchain.env })
    .trimEnd()
    .split(/\r?\n/);

  if (fromC.length !== grid.length) {
    console.error(`FAIL     C produced ${fromC.length} payloads for ${grid.length} rows`);
    process.exit(1);
  }

  for (const [index, row] of grid.entries()) {
    const fromTs = Buffer.from(encodeBridgeStat(row)).toString('hex').toUpperCase();
    if (fromTs !== fromC[index]) {
      failures.push(`row ${index}: C and TS encodings differ`);
      continue;
    }

    // And the decoder must recover the row it started from — an encoder pair
    // that agrees on the same mistake would otherwise pass.
    const decoded = decodeBridgeStat(new Uint8Array(Buffer.from(fromC[index], 'hex')));
    const wrong = FIELDS.filter((f) => decoded[f] !== row[f]);
    if (wrong.length > 0) {
      failures.push(`row ${index}: decode disagrees on ${wrong.join(', ')}`);
      continue;
    }
    agreed++;
  }
} finally {
  rmSync(workDir, { recursive: true, force: true });
}

for (const failure of failures.slice(0, 10)) console.error(`MISMATCH ${failure}`);

console.log(`LH_METRIC test.bridge_stat.cross_lang value=${agreed} unit=count budget=${grid.length}`);
console.log(`LH_METRIC test.bridge_stat.mismatches value=${failures.length} unit=count budget=0`);
console.log(`LH_METRIC size.bridge_stat.payload value=${BRIDGE_STAT_SIZE} unit=B budget=220`);

if (failures.length > 0) {
  console.error(`\n${failures.length} of ${grid.length} bridge stat payloads disagree.`);
  process.exit(1);
}
console.log(`OK       bridge stat — ${grid.length} rows agree between C and TypeScript`);
