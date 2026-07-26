/**
 * Cross-language verification of config fragmentation (T2.3).
 *
 * The Host splits and the Node reassembles. That is the only direction that
 * matters operationally, and it is precisely the direction that cannot be
 * tested by either side alone: a splitter verified against its own reassembler
 * agrees with itself about any mistake it makes consistently — an off-by-one in
 * the slice length, a byte order, a CRC over the wrong span.
 *
 * So both directions are crossed here, on the same corpus:
 *
 *   TS split -> C reassemble  (the real path: Host to Node)
 *   C split  -> TS reassemble (the return path, and the check on the C builder)
 *
 * Fragments are also delivered out of order and with duplicates injected, since
 * an ARQ over a lossy link produces both and a splitter that only ever gets
 * tested in order proves nothing about a link that is not.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { exeName, findHostToolchain, spawnFailureDetail } from './host-cc.mjs';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));
const COMMON = join(REPO_ROOT, 'firmware', 'common');
const PROTOCOL_FRAG = join(REPO_ROOT, 'packages', 'protocol', 'dist', 'src', 'fragment.js');

const VECTOR_COUNT = 200;

if (!existsSync(PROTOCOL_FRAG)) {
  console.log('SKIPPED  packages/protocol is not built — run `pnpm -r build` first');
  console.log('LH_METRIC test.cross_lang_frag value=SKIPPED unit=count');
  process.exit(0);
}

const toolchain = findHostToolchain();
if (toolchain === null) {
  console.log('SKIPPED  no host C toolchain — the C fragmenter was not compared');
  console.log('LH_METRIC test.cross_lang_frag value=SKIPPED unit=count');
  process.exit(0);
}

const { splitConfig, Reassembler, FragResult, FRAG_CONFIG_MAX, FRAG_PAYLOAD_MAX } = await import(
  pathToFileURL(PROTOCOL_FRAG).href
);

console.log(`LH_ENV toolchain.frag_check=${toolchain.version}`);

let rng = 0x1d872bfa;
function nextRandom() {
  rng ^= rng << 13;
  rng ^= rng >>> 17;
  rng ^= rng << 5;
  return rng >>> 0;
}

/**
 * Configs at the sizes where fragment arithmetic breaks, then random ones.
 *
 * The boundaries are named rather than left to the draw: an empty config, one
 * byte, exactly one full fragment, one byte past it, and the 1600 B maximum.
 */
function buildCorpus() {
  const lengths = [0, 1, FRAG_PAYLOAD_MAX - 1, FRAG_PAYLOAD_MAX, FRAG_PAYLOAD_MAX + 1,
    2 * FRAG_PAYLOAD_MAX, FRAG_CONFIG_MAX - 1, FRAG_CONFIG_MAX];

  while (lengths.length < VECTOR_COUNT) lengths.push(nextRandom() % (FRAG_CONFIG_MAX + 1));

  return lengths.map((length, index) => {
    const config = new Uint8Array(length);
    for (let i = 0; i < length; i++) config[i] = (nextRandom() >>> 8) & 0xff;
    return { cfgId: (0x1000 + index) & 0xffff, config };
  });
}

const toHex = (bytes) => Buffer.from(bytes).toString('hex').toUpperCase();
const fromHex = (hex) => new Uint8Array(Buffer.from(hex, 'hex'));

/**
 * Feed order for one transaction: shuffled, with one fragment repeated.
 *
 * Deterministic per vector, so a failure is reproducible. The duplicate is what
 * makes this more than a reordering test — both implementations have to agree
 * that a repeat changes nothing, not merely that they eventually complete.
 */
function feedOrder(count) {
  const order = Array.from({ length: count }, (_, i) => i);
  for (let i = count; i > 1; i--) {
    const j = nextRandom() % i;
    [order[i - 1], order[j]] = [order[j], order[i - 1]];
  }
  if (count > 1) order.splice(1, 0, order[0]);
  return order;
}

const corpus = buildCorpus();
const workDir = mkdtempSync(join(tmpdir(), 'lh-fragx-'));

let tsToC = 0;
let cToTs = 0;
const failures = [];

try {
  const binary = join(workDir, exeName('frag_cli'));
  try {
    toolchain.compile({
      sources: [
        join(COMMON, 'test', 'frag_cli.c'),
        join(COMMON, 'src', 'frag.c'),
        join(COMMON, 'src', 'crc16.c'),
      ],
      includeDirs: [join(COMMON, 'include')],
      output: binary,
      optimize: 'O2',
    });
  } catch (error) {
    console.error(`FAIL     could not build the fragment CLI\n${spawnFailureDetail(error)}`);
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

  // --- TypeScript split -> C reassemble -------------------------------------

  const tsFragments = corpus.map(({ cfgId, config }) => splitConfig(cfgId, config));
  const feedOrders = tsFragments.map((fragments) => feedOrder(fragments.length));

  const cLines = runCli(
    'reassemble',
    tsFragments.map((fragments, index) =>
      feedOrders[index].map((position) => toHex(fragments[position])).join(','),
    ),
  );

  for (const [index, { config }] of corpus.entries()) {
    const [code, hex = ''] = cLines[index].split(' ');
    if (Number(code) === FragResult.COMPLETE && hex === toHex(config)) cToTs++;
    else {
      failures.push(
        `vector ${index} (len ${config.length}): TS split -> C reassemble gave code ${code}`,
      );
    }
  }

  // --- C split -> TypeScript reassemble -------------------------------------

  const cFragmentLines = runCli(
    'split',
    corpus.map(({ cfgId, config }) => `${cfgId.toString(16)} ${toHex(config)}`),
  );

  for (const [index, { config }] of corpus.entries()) {
    const fragments = cFragmentLines[index].split(',').filter((hex) => hex.length > 0);
    const reassembler = new Reassembler();
    let result = FragResult.NEED_MORE;

    for (const position of feedOrders[index]) {
      result = reassembler.feed(fromHex(fragments[position]), 1000);
    }

    if (result === FragResult.COMPLETE && toHex(reassembler.assembled()) === toHex(config)) {
      tsToC++;
    } else {
      failures.push(
        `vector ${index} (len ${config.length}): C split -> TS reassemble gave code ${result}`,
      );
    }
  }
} finally {
  rmSync(workDir, { recursive: true, force: true });
}

for (const failure of failures.slice(0, 10)) console.error(`MISMATCH ${failure}`);

console.log(`LH_METRIC test.cross_lang_frag.ts_to_c value=${cToTs} unit=count budget=${corpus.length}`);
console.log(`LH_METRIC test.cross_lang_frag.c_to_ts value=${tsToC} unit=count budget=${corpus.length}`);
console.log(
  `LH_METRIC test.cross_lang_frag value=${Math.min(cToTs, tsToC)} unit=count budget=${corpus.length}`,
);
console.log(`LH_METRIC test.cross_lang_frag.mismatches value=${failures.length} unit=count budget=0`);

if (failures.length > 0) {
  console.error(`\n${failures.length} fragment vectors disagree between C and TypeScript.`);
  process.exit(1);
}
console.log(
  `OK       fragment cross-language — ${corpus.length} configs, both directions, shuffled with duplicates`,
);
