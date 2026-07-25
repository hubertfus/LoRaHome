/**
 * Cross-language verification of the SLIP codec (T1.5).
 *
 * There are two implementations of this wire format — firmware/common/src/slip.c
 * and packages/host/src/transport/slip.ts — which is exactly the arrangement
 * Etap 0 exists to prevent. It is unavoidable here: one end runs on an ESP32
 * and the other on Node, and there is no shared artefact to generate both from
 * the way the frame header is generated. So the two are held together by
 * evidence instead of by discipline.
 *
 * A thousand random buffers go both ways:
 *
 *   C encode  -> TypeScript decode -> must equal the original
 *   TS encode -> C decode          -> must equal the original
 *
 * Both directions matter and neither implies the other. A pair of codecs that
 * agree on a shared mistake would pass a round trip through *one* language
 * every time; only crossing catches that.
 *
 * The corpus is deliberately biased toward 0xC0 and 0xDB. Uniformly random
 * bytes hit an escape roughly once every 128, so the escape path — which is
 * where the two implementations could most plausibly differ — would barely be
 * exercised by chance.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { exeName, findHostToolchain, spawnFailureDetail } from './host-cc.mjs';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));
const COMMON = join(REPO_ROOT, 'firmware', 'common');
const HOST_SLIP = join(REPO_ROOT, 'packages', 'host', 'dist', 'src', 'transport', 'slip.js');

const VECTOR_COUNT = 1000;
const MTU = 230;

if (!existsSync(HOST_SLIP)) {
  console.log('SKIPPED  packages/host is not built — run `pnpm -r build` first');
  console.log('LH_METRIC test.cross_lang_slip value=SKIPPED unit=count');
  process.exit(0);
}

const toolchain = findHostToolchain();
if (toolchain === null) {
  console.log('SKIPPED  no host C toolchain — the C codec was not compared');
  console.log('LH_METRIC test.cross_lang_slip value=SKIPPED unit=count');
  process.exit(0);
}

const { slipEncode, SlipDecoder } = await import(pathToFileURL(HOST_SLIP).href);

console.log(`LH_ENV toolchain.slip_check=${toolchain.version}`);

/**
 * Deterministic corpus.
 *
 * Same seed every run, so a failure is reproducible and the vector set does not
 * quietly change between commits.
 */
let rng = 0x2B7E1516;
function nextRandom() {
  rng ^= rng << 13;
  rng ^= rng >>> 17;
  rng ^= rng << 5;
  return rng >>> 0;
}

function buildCorpus() {
  const vectors = [];

  // The cases worth naming, rather than hoping the random draw includes them.
  vectors.push(new Uint8Array(0));
  vectors.push(Uint8Array.from([0xc0]));
  vectors.push(Uint8Array.from([0xdb]));
  vectors.push(Uint8Array.from([0xdb, 0xdc]));  // looks like an escape, is data
  vectors.push(Uint8Array.from([0xdb, 0xdd]));
  vectors.push(Uint8Array.from(new Array(MTU).fill(0xc0)));
  vectors.push(Uint8Array.from(new Array(MTU).fill(0xdb)));
  vectors.push(Uint8Array.from({ length: MTU }, (_, i) => (i % 2 ? 0xdb : 0xc0)));

  while (vectors.length < VECTOR_COUNT) {
    const length = nextRandom() % (MTU + 1);
    const bytes = new Uint8Array(length);
    for (let i = 0; i < length; i++) {
      const draw = nextRandom();
      // One byte in four is a reserved value, so escapes are common rather than
      // incidental.
      bytes[i] = draw % 4 === 0 ? (draw % 8 === 0 ? 0xc0 : 0xdb) : (draw >>> 8) & 0xff;
    }
    vectors.push(bytes);
  }

  return vectors;
}

const toHex = (bytes) => Buffer.from(bytes).toString('hex').toUpperCase();
const fromHex = (hex) => new Uint8Array(Buffer.from(hex, 'hex'));

const corpus = buildCorpus();
const workDir = mkdtempSync(join(tmpdir(), 'lh-slipx-'));

let cToTs = 0;
let tsToC = 0;
const failures = [];

try {
  const binary = join(workDir, exeName('slip_cli'));
  try {
    toolchain.compile({
      sources: [join(COMMON, 'test', 'slip_cli.c'), join(COMMON, 'src', 'slip.c')],
      includeDirs: [join(COMMON, 'include')],
      output: binary,
      optimize: 'O2',
    });
  } catch (error) {
    console.error(`FAIL     could not build the SLIP CLI\n${spawnFailureDetail(error)}`);
    process.exit(1);
  }

  const runCli = (mode, lines) =>
    execFileSync(binary, [mode], {
      input: lines.join('\n') + '\n',
      encoding: 'utf8',
      env: toolchain.env,
      maxBuffer: 32 * 1024 * 1024,
    })
      .trimEnd()
      .split(/\r?\n/);

  // --- C encode -> TypeScript decode ---------------------------------------

  const cEncoded = runCli('encode', corpus.map(toHex));
  if (cEncoded.length !== corpus.length) {
    console.error(`FAIL     C encode produced ${cEncoded.length} lines for ${corpus.length} inputs`);
    process.exit(1);
  }

  for (const [index, original] of corpus.entries()) {
    const frames = new SlipDecoder(MTU + 32).push(fromHex(cEncoded[index]));

    if (original.length === 0) {
      // An empty payload is END END on the wire, which is a delimiter pair and
      // decodes to nothing. Both implementations must agree on that.
      if (frames.length === 0) cToTs++;
      else failures.push(`vector ${index}: TS produced a frame from an empty payload`);
      continue;
    }

    if (frames.length === 1 && Buffer.from(original).equals(frames[0])) cToTs++;
    else failures.push(`vector ${index} (len ${original.length}): C encode -> TS decode mismatch`);
  }

  // --- TypeScript encode -> C decode ---------------------------------------

  const tsEncoded = corpus.map((v) => toHex(slipEncode(v)));
  const cDecoded = runCli('decode', tsEncoded);
  if (cDecoded.length !== corpus.length) {
    console.error(`FAIL     C decode produced ${cDecoded.length} lines for ${corpus.length} inputs`);
    process.exit(1);
  }

  for (const [index, original] of corpus.entries()) {
    if (toHex(original) === cDecoded[index]) tsToC++;
    else failures.push(`vector ${index} (len ${original.length}): TS encode -> C decode mismatch`);
  }
} finally {
  rmSync(workDir, { recursive: true, force: true });
}

for (const failure of failures.slice(0, 10)) console.error(`MISMATCH ${failure}`);

console.log(`LH_METRIC test.cross_lang_slip.c_to_ts value=${cToTs} unit=count budget=${corpus.length}`);
console.log(`LH_METRIC test.cross_lang_slip.ts_to_c value=${tsToC} unit=count budget=${corpus.length}`);
console.log(`LH_METRIC test.cross_lang_slip value=${Math.min(cToTs, tsToC)} unit=count budget=${corpus.length}`);
console.log(`LH_METRIC test.cross_lang_slip.mismatches value=${failures.length} unit=count budget=0`);

if (failures.length > 0) {
  console.error(`\n${failures.length} SLIP vectors disagree between C and TypeScript.`);
  process.exit(1);
}
console.log(`OK       slip cross-language — ${corpus.length} vectors, both directions`);
