/**
 * Cross-language verification of the capability report (T3.5).
 *
 * The node encodes and the host decodes. That is the direction that matters
 * operationally, and it is exactly the direction neither side can test alone:
 * an encoder verified against its own decoder agrees with itself about every
 * mistake it makes consistently — a byte order, an argument width, a key that
 * both sides spell the same wrong way.
 *
 * So both directions are crossed on the same corpus, and the comparison is on
 * bytes rather than on values:
 *
 *   C encode  -> TS decode   (the real path: Node to Host)
 *   TS encode -> C decode    (the return path, and the check on the C decoder)
 *   C encode  == TS encode   (byte for byte — the strongest of the three)
 *
 * The third is only a meaningful check because both sides encode canonically:
 * shortest argument form for every integer. Two encoders that both produce
 * valid CBOR but pick different widths for the same number would pass the first
 * two checks for ever while disagreeing on the wire.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { exeName, findHostToolchain, spawnFailureDetail } from './host-cc.mjs';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));
const COMMON = join(REPO_ROOT, 'firmware', 'common');
const PROTOCOL_CAP = join(REPO_ROOT, 'packages', 'protocol', 'dist', 'src', 'capability.js');

const VECTOR_COUNT = 200;

if (!existsSync(PROTOCOL_CAP)) {
  console.log('SKIPPED  packages/protocol is not built — run `pnpm -r build` first');
  console.log('LH_METRIC test.cross_lang_cbor value=SKIPPED unit=count');
  process.exit(0);
}

const toolchain = findHostToolchain();
if (toolchain === null) {
  console.log('SKIPPED  no host C toolchain — the C capability codec was not compared');
  console.log('LH_METRIC test.cross_lang_cbor value=SKIPPED unit=count');
  process.exit(0);
}

const { encodeCapabilityReport, decodeCapabilityReport, capabilityReportsEqual, CAP_MAX } =
  await import(pathToFileURL(PROTOCOL_CAP).href);

console.log(`LH_ENV toolchain.capability_check=${toolchain.version}`);

let rng = 0x6b3f19c5;
function nextRandom() {
  rng ^= rng << 13;
  rng ^= rng >>> 17;
  rng ^= rng << 5;
  return rng >>> 0;
}

/**
 * Named boundaries first, then random reports.
 *
 * The boundaries are the ones where an encoding changes width: a type id below
 * 24 takes one byte and above it takes two, and the same threshold applies to
 * bus addresses, channel counts and the component count itself. A corpus of
 * purely random draws would hit those seams eventually; naming them means the
 * failure is reproducible rather than a run that happened to be unlucky.
 */
function buildCorpus() {
  const vectors = [];

  // Empty, and the two real drivers as they will actually be reported.
  vectors.push({ fwVersion: 0, freeHeapKb: 0, components: [] });
  vectors.push({
    fwVersion: 0x00040000,
    freeHeapKb: 192,
    components: [
      { driverTypeId: 16, busAddr: 0x76, busType: 0, channelCount: 4, flags: 1 },
      { driverTypeId: 17, busAddr: 4, busType: 1, channelCount: 2, flags: 0 },
    ],
  });

  // The widest report the system can actually emit: eight components, the
  // largest allocatable type id, bus types from the five-value enum, channel
  // counts in single digits, one flag bit. This is the one the 100 B budget is
  // about, and it is what `size.cap_report.cbor.8comp` measures.
  vectors.push({
    fwVersion: 0xffffffff,
    freeHeapKb: 0xffff,
    components: Array.from({ length: CAP_MAX }, () => ({
      driverTypeId: 0xffff,
      busAddr: 0x77,
      busType: 4,
      channelCount: 8,
      flags: 1,
    })),
  });

  // And the absolute maximum, with every field at the widest value its type
  // permits. Not reachable from the encoder, reachable from the wire — which
  // is the bound a receive buffer has to survive.
  vectors.push({
    fwVersion: 0xffffffff,
    freeHeapKb: 0xffff,
    components: Array.from({ length: CAP_MAX }, () => ({
      driverTypeId: 0xffff,
      busAddr: 0xff,
      busType: 0xff,
      channelCount: 0xff,
      flags: 0xff,
    })),
  });

  // Every CBOR argument-width seam, on each field in turn.
  for (const edge of [0, 1, 23, 24, 255, 256, 65535]) {
    vectors.push({
      fwVersion: edge,
      freeHeapKb: Math.min(edge, 0xffff),
      components: [
        {
          driverTypeId: Math.min(edge, 0xffff),
          busAddr: Math.min(edge, 0xff),
          busType: 0,
          channelCount: Math.min(edge, 0xff),
          flags: Math.min(edge, 0xff),
        },
      ],
    });
  }

  while (vectors.length < VECTOR_COUNT) {
    const count = nextRandom() % (CAP_MAX + 1);
    vectors.push({
      fwVersion: nextRandom(),
      freeHeapKb: nextRandom() & 0xffff,
      components: Array.from({ length: count }, () => ({
        driverTypeId: nextRandom() & 0xffff,
        busAddr: nextRandom() & 0xff,
        busType: nextRandom() % 5,
        channelCount: nextRandom() & 0xff,
        flags: nextRandom() & 0x01,
      })),
    });
  }

  return vectors;
}

const toHex = (bytes) => Buffer.from(bytes).toString('hex').toUpperCase();

/** The CLI's text form for one report. */
function toCliLine(report) {
  const components = report.components
    .map((c) => `${c.driverTypeId}:${c.busAddr}:${c.busType}:${c.channelCount}:${c.flags}`)
    .join(',');
  return `${report.fwVersion.toString(16)} ${report.freeHeapKb.toString(16)} ${components}`;
}

/** Parses the CLI's text form back into a report. */
function fromCliLine(line) {
  if (line === 'ERR') return null;
  const [fw, heap, components = ''] = line.split(' ');
  return {
    fwVersion: parseInt(fw, 16),
    freeHeapKb: parseInt(heap, 16),
    components: components
      .split(',')
      .filter((part) => part.length > 0)
      .map((part) => {
        const [driverTypeId, busAddr, busType, channelCount, flags] = part.split(':').map(Number);
        return { driverTypeId, busAddr, busType, channelCount, flags };
      }),
  };
}

const corpus = buildCorpus();
const workDir = mkdtempSync(join(tmpdir(), 'lh-capx-'));

let cToTs = 0;
let tsToC = 0;
let identical = 0;
let largest = 0;
let largestFull = 0;
const failures = [];

try {
  const binary = join(workDir, exeName('capability_cli'));
  try {
    toolchain.compile({
      sources: [
        join(COMMON, 'test', 'capability_cli.c'),
        join(COMMON, 'src', 'capability.c'),
        join(COMMON, 'src', 'cbor.c'),
      ],
      includeDirs: [join(COMMON, 'include')],
      output: binary,
      optimize: 'O2',
    });
  } catch (error) {
    console.error(`FAIL     could not build the capability CLI\n${spawnFailureDetail(error)}`);
    process.exit(1);
  }

  const runCli = (mode, lines) =>
    execFileSync(binary, [mode], {
      input: lines.join('\n') + '\n',
      encoding: 'utf8',
      env: toolchain.env,
      maxBuffer: 64 * 1024 * 1024,
    })
      .trimEnd()
      .split(/\r?\n/);

  // --- C encode -> TypeScript decode, and byte-for-byte against TS encode ---

  const cEncoded = runCli('encode', corpus.map(toCliLine));

  for (const [index, report] of corpus.entries()) {
    const hex = cEncoded[index];
    const tsHex = toHex(encodeCapabilityReport(report));

    if (hex === tsHex) identical++;
    else failures.push(`vector ${index}: C encoded ${hex}, TypeScript encoded ${tsHex}`);

    const bytes = hex.length / 2;
    if (bytes > largest) largest = bytes;
    if (report.components.length === CAP_MAX && bytes > largestFull) largestFull = bytes;

    try {
      const decoded = decodeCapabilityReport(Uint8Array.from(Buffer.from(hex, 'hex')));
      if (capabilityReportsEqual(decoded, report)) cToTs++;
      else failures.push(`vector ${index}: C encode -> TS decode gave a different report`);
    } catch (error) {
      failures.push(`vector ${index}: C encode -> TS decode threw ${error.message}`);
    }
  }

  // --- TypeScript encode -> C decode ---------------------------------------

  const cDecoded = runCli(
    'decode',
    corpus.map((report) => toHex(encodeCapabilityReport(report))),
  );

  for (const [index, report] of corpus.entries()) {
    const decoded = fromCliLine(cDecoded[index]);
    if (decoded !== null && capabilityReportsEqual(decoded, report)) tsToC++;
    else failures.push(`vector ${index}: TS encode -> C decode gave ${cDecoded[index]}`);
  }
} finally {
  rmSync(workDir, { recursive: true, force: true });
}

for (const failure of failures.slice(0, 10)) console.error(`MISMATCH ${failure}`);

console.log(`LH_METRIC test.cross_lang_cbor.c_to_ts value=${cToTs} unit=count budget=${corpus.length}`);
console.log(`LH_METRIC test.cross_lang_cbor.ts_to_c value=${tsToC} unit=count budget=${corpus.length}`);
console.log(`LH_METRIC test.cross_lang_cbor.identical_bytes value=${identical} unit=count budget=${corpus.length}`);
console.log(
  `LH_METRIC test.cross_lang_cbor value=${Math.min(cToTs, tsToC, identical)} unit=count budget=${corpus.length}`,
);
console.log(`LH_METRIC test.cross_lang_cbor.mismatches value=${failures.length} unit=count budget=0`);
// The budgeted figures come from the native harness, which builds the two
// reports the budgets are defined against. What this corpus can say is the
// largest anything in it encoded to, across 200 mixed vectors — a trend line,
// not a gate, so it carries no budget it could fail on a different draw.
console.log(`LH_METRIC size.cap_report.cbor.corpus_max value=${largest} unit=B`);
console.log(`LH_METRIC size.cap_report.cbor.corpus_max_8comp value=${largestFull} unit=B`);

if (failures.length > 0) {
  console.error(`\n${failures.length} capability vectors disagree between C and TypeScript.`);
  process.exit(1);
}
console.log(
  `OK       capability cross-language — ${corpus.length} reports, both directions, byte-identical`,
);
