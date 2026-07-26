/**
 * Cross-language verification of the dedup window (T2.1).
 *
 * There are two implementations of one rule — firmware/common/src/dedup.c and
 * packages/protocol/src/dedup.ts — for the same unavoidable reason as SLIP: one
 * end runs on an ESP32 and the other on Node. A drift between them would not
 * announce itself. Both would keep working; they would simply disagree about
 * which frames are duplicates, and the symptom would be a relay that toggles
 * twice on a bad-signal day, months from now.
 *
 * So they are held together by evidence. One event stream, replayed through
 * both, verdict compared frame by frame — plus the final counters, because two
 * implementations can agree on every accept/reject and still disagree about
 * what they just did (a duplicate counted as too-old is a different diagnosis
 * in the field).
 *
 * The corpus is built to hit the cases that separate implementations, not to be
 * uniformly random: sequence wrap, replays at the exact window edge, reordering,
 * and more senders than the table holds.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { exeName, findHostToolchain, spawnFailureDetail } from './host-cc.mjs';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));
const COMMON = join(REPO_ROOT, 'firmware', 'common');
const PROTOCOL_DEDUP = join(REPO_ROOT, 'packages', 'protocol', 'dist', 'src', 'dedup.js');

const EVENT_COUNT = 5000;

if (!existsSync(PROTOCOL_DEDUP)) {
  console.log('SKIPPED  packages/protocol is not built — run `pnpm -r build` first');
  console.log('LH_METRIC test.cross_lang_dedup value=SKIPPED unit=count');
  process.exit(0);
}

const toolchain = findHostToolchain();
if (toolchain === null) {
  console.log('SKIPPED  no host C toolchain — the C window was not compared');
  console.log('LH_METRIC test.cross_lang_dedup value=SKIPPED unit=count');
  process.exit(0);
}

const { DedupWindow, DEDUP_PEERS, DEDUP_WINDOW } = await import(pathToFileURL(PROTOCOL_DEDUP).href);

console.log(`LH_ENV toolchain.dedup_check=${toolchain.version}`);

/** Deterministic corpus: same seed every run, so a failure is reproducible. */
let rng = 0x6c8e944a;
function nextRandom() {
  rng ^= rng << 13;
  rng ^= rng >>> 17;
  rng ^= rng << 5;
  return rng >>> 0;
}

/**
 * Events as `[srcId, seq, nowUs]`.
 *
 * The named cases come first so they are present regardless of the draw, and
 * so a mismatch in one of them is reported before the random tail buries it.
 */
function buildCorpus() {
  const events = [];
  let clock = 1000;
  const at = (src, seq) => events.push([src, seq & 0xff, (clock += 1000)]);

  // 1. A full lap of the sequence space plus a wrap — R2.1.
  for (let i = 0; i < 300; i++) at(0x0101, i);

  // 2. Every entry of the window replayed, then one step beyond it.
  for (let age = 0; age <= DEDUP_WINDOW; age++) at(0x0101, 299 - age);

  // 3. Reordering inside the window, then the same frames again.
  for (const seq of [40, 38, 44, 39, 43, 41, 38, 44, 42]) at(0x0202, seq);

  // 4. More senders than the table holds, with one deliberately left idle so
  //    the eviction victim is determined rather than incidental.
  for (let peer = 0; peer < DEDUP_PEERS + 3; peer++) at(0x0300 + peer, 7);
  for (let peer = 1; peer < DEDUP_PEERS; peer++) at(0x0300 + peer, 8);
  at(0x0300, 7); // the evicted peer's replay — new again, and both must agree

  // 5. Random traffic with clustered retransmissions, which is what an ARQ
  //    under loss actually produces.
  const nextSeq = new Map();
  while (events.length < EVENT_COUNT) {
    const draw = nextRandom();
    const src = 0x0400 + (draw % 6);
    const current = nextSeq.get(src) ?? 0;

    if ((draw >>> 8) % 3 === 0 && current > 24) {
      at(src, current - 1 - ((draw >>> 16) % 40)); // sometimes past the window edge
    } else {
      at(src, current);
      nextSeq.set(src, current + 1);
    }
  }

  return events;
}

const corpus = buildCorpus();
const workDir = mkdtempSync(join(tmpdir(), 'lh-dedupx-'));

let agreed = 0;
const failures = [];

try {
  const binary = join(workDir, exeName('dedup_cli'));
  try {
    toolchain.compile({
      sources: [join(COMMON, 'test', 'dedup_cli.c'), join(COMMON, 'src', 'dedup.c')],
      includeDirs: [join(COMMON, 'include')],
      output: binary,
      optimize: 'O2',
    });
  } catch (error) {
    console.error(`FAIL     could not build the dedup CLI\n${spawnFailureDetail(error)}`);
    process.exit(1);
  }

  const cOutput = execFileSync(binary, [], {
    input: corpus.map(([src, seq, nowUs]) => `${src} ${seq} ${nowUs}`).join('\n') + '\n',
    encoding: 'utf8',
    env: toolchain.env,
    maxBuffer: 32 * 1024 * 1024,
  })
    .trimEnd()
    .split(/\r?\n/);

  const cStats = cOutput.pop() ?? '';
  if (cOutput.length !== corpus.length) {
    console.error(`FAIL     C produced ${cOutput.length} verdicts for ${corpus.length} events`);
    process.exit(1);
  }

  const window = new DedupWindow();
  for (const [index, [src, seq, nowUs]] of corpus.entries()) {
    const ts = window.checkAndMark(src, seq, nowUs) ? '1' : '0';
    if (ts === cOutput[index]) agreed++;
    else {
      failures.push(
        `event ${index} (src 0x${src.toString(16)}, seq ${seq}): C said ${cOutput[index]}, TS said ${ts}`,
      );
    }
  }

  const { accepted, dupesDropped, tooOld, peerEvicted } = window.stats;
  const tsStats = `STATS ${accepted} ${dupesDropped} ${tooOld} ${peerEvicted}`;
  if (tsStats !== cStats) {
    failures.push(`counters disagree: C "${cStats}" vs TS "${tsStats}"`);
  }
  console.log(`         counters: ${tsStats.slice('STATS '.length)} (accepted dupes too_old evicted)`);
} finally {
  rmSync(workDir, { recursive: true, force: true });
}

for (const failure of failures.slice(0, 10)) console.error(`MISMATCH ${failure}`);

console.log(`LH_METRIC test.cross_lang_dedup value=${agreed} unit=count budget=${corpus.length}`);
console.log(`LH_METRIC test.cross_lang_dedup.mismatches value=${failures.length} unit=count budget=0`);

if (failures.length > 0) {
  console.error(`\n${failures.length} dedup verdicts disagree between C and TypeScript.`);
  process.exit(1);
}
console.log(`OK       dedup cross-language — ${corpus.length} events, identical verdicts`);
