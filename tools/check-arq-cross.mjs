/**
 * Cross-language verification of the ARQ state machine (T2.4).
 *
 * Two implementations decide when to retransmit — firmware/common/src/arq.c on
 * the Node, packages/protocol/src/arq.ts on the Host. A disagreement between
 * them would not look like a bug. It would look like a flaky link: one side
 * retrying a beat earlier than the other, one side giving up while the other
 * still waits, duplicate ACKs treated differently at each end. The kind of
 * thing that gets blamed on the radio for a month.
 *
 * So both replay the same script — sends, ACKs, ticks, duty-cycle vetoes — and
 * must produce the same decision, state and retry count at every step. The
 * jitter values are scripted too, which is what makes the comparison possible:
 * with the same "random" numbers consumed in the same order, the retransmission
 * schedule is identical, and any difference left is a difference in the logic.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { exeName, findHostToolchain, spawnFailureDetail } from './host-cc.mjs';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));
const COMMON = join(REPO_ROOT, 'firmware', 'common');
const PROTOCOL_ARQ = join(REPO_ROOT, 'packages', 'protocol', 'dist', 'src', 'arq.js');

if (!existsSync(PROTOCOL_ARQ)) {
  console.log('SKIPPED  packages/protocol is not built — run `pnpm -r build` first');
  console.log('LH_METRIC test.cross_lang_arq value=SKIPPED unit=count');
  process.exit(0);
}

const toolchain = findHostToolchain();
if (toolchain === null) {
  console.log('SKIPPED  no host C toolchain — the C ARQ was not compared');
  console.log('LH_METRIC test.cross_lang_arq value=SKIPPED unit=count');
  process.exit(0);
}

const { Arq, ArqAction } = await import(pathToFileURL(PROTOCOL_ARQ).href);

console.log(`LH_ENV toolchain.arq_check=${toolchain.version}`);

let rng = 0x51a3d7c9;
function nextRandom() {
  rng ^= rng << 13;
  rng ^= rng >>> 17;
  rng ^= rng << 5;
  return rng >>> 0;
}

/**
 * The script.
 *
 * Named scenarios first — the ones with a wrong answer that still looks right:
 * a dead link that must end in a give-up rather than in waiting, a stale ACK
 * that must not acknowledge the frame currently in flight, a duty cycle that
 * must postpone a retry without spending one. Then random traffic, because the
 * interleavings nobody thought of are the point of doing this at all.
 */
function buildScript() {
  const events = [];
  let clock = 0;
  const at = (step = 1_000_000) => (clock += step);

  // A supply of jitter values, consumed in order by both implementations.
  for (let i = 0; i < 512; i++) events.push(`R ${nextRandom()}`);

  // 1. Send, tick before the timeout, ACK.
  events.push(`S 7 64 ${at(0)}`);
  events.push(`T ${at(100_000)}`);
  events.push(`A 7 ${at(400_000)}`);
  events.push(`T ${at()}`);

  // 2. A dead link: ticks until the give-up, and past it.
  events.push(`S 20 230 ${at()}`);
  for (let i = 0; i < 12; i++) events.push(`T ${at(4_000_000)}`);

  // 3. Stale and duplicate ACKs around a live transfer.
  events.push(`S 30 64 ${at()}`);
  events.push(`A 29 ${at(10_000)}`);
  events.push(`A 30 ${at(10_000)}`);
  events.push(`A 30 ${at(10_000)}`);
  events.push(`S 31 64 ${at(10_000)}`);
  events.push(`A 30 ${at(10_000)}`);
  events.push(`A 31 ${at(10_000)}`);

  // 4. Duty cycle refuses, then relents.
  events.push(`S 40 200 ${at()}`);
  events.push('G 0');
  for (let i = 0; i < 6; i++) events.push(`T ${at(3_000_000)}`);
  events.push('G 1');
  events.push(`T ${at(3_000_000)}`);
  events.push(`A 40 ${at(200_000)}`);

  // 5. Random traffic. Sends on a busy slot, ACKs for sequences never sent,
  //    ticks at irregular intervals — all of it legal input, all of it a chance
  //    for the two state machines to drift apart.
  let seq = 50;
  for (let i = 0; i < 600; i++) {
    const draw = nextRandom() % 10;
    if (draw < 2) events.push(`S ${seq++ & 0xff} ${64 + (nextRandom() % 160)} ${at(500_000)}`);
    else if (draw < 4) events.push(`A ${(seq - 1 - (nextRandom() % 3)) & 0xff} ${at(300_000)}`);
    else if (draw === 4) events.push(`G ${nextRandom() % 2}`);
    else events.push(`T ${at(1_500_000 + (nextRandom() % 4_000_000))}`);
  }

  return events;
}

const script = buildScript();
const workDir = mkdtempSync(join(tmpdir(), 'lh-arqx-'));

let agreed = 0;
const failures = [];

try {
  const binary = join(workDir, exeName('arq_cli'));
  try {
    toolchain.compile({
      sources: [join(COMMON, 'test', 'arq_cli.c'), join(COMMON, 'src', 'arq.c')],
      includeDirs: [join(COMMON, 'include')],
      output: binary,
      optimize: 'O2',
    });
  } catch (error) {
    console.error(`FAIL     could not build the ARQ CLI\n${spawnFailureDetail(error)}`);
    process.exit(1);
  }

  const cLines = execFileSync(binary, [], {
    input: script.join('\n') + '\n',
    encoding: 'utf8',
    env: toolchain.env,
    maxBuffer: 32 * 1024 * 1024,
  })
    .trimEnd()
    .split(/\r?\n/);

  // --- the same script, through the TypeScript twin -------------------------

  const queue = [];
  let queuePos = 0;
  const scriptedRandom = () => {
    if (queue.length === 0) return 0;
    if (queuePos >= queue.length) return queue[queue.length - 1];
    return queue[queuePos++];
  };

  let allowTx = true;
  const arq = new Arq({ random: scriptedRandom, allowTx: () => allowTx });
  const frame = Uint8Array.from({ length: 230 }, (_, i) => (i * 7 + 3) & 0xff);

  const tsLines = [];
  for (const event of script) {
    const [kind, ...args] = event.split(' ');
    let result = 0;

    switch (kind) {
      case 'R':
        queue.push(Number(args[0]) >>> 0);
        break;
      case 'S':
        result = arq.send(Number(args[0]), frame.subarray(0, Number(args[1])), Number(args[2]))
          ? 1
          : 0;
        break;
      case 'A':
        result = arq.onAck(Number(args[0]), Number(args[1])) ? 1 : 0;
        break;
      case 'T':
        result = arq.tick(Number(args[0])).action;
        break;
      case 'G':
        allowTx = args[0] !== '0';
        break;
      default:
        continue;
    }

    tsLines.push(`${result} ${arq.state} ${arq.retryCount}`);
  }

  if (cLines.length !== tsLines.length) {
    console.error(`FAIL     C produced ${cLines.length} lines for ${tsLines.length} events`);
    process.exit(1);
  }

  for (const [index, expected] of tsLines.entries()) {
    if (cLines[index] === expected) agreed++;
    else {
      failures.push(
        `step ${index} (${script[index]}): C "${cLines[index]}" vs TS "${expected}" ` +
          '(result, state, retries)',
      );
    }
  }

  // A script that never reached the interesting states would agree trivially.
  const retransmits = tsLines.filter((line) => line.startsWith(`${ArqAction.RETRANSMIT} `)).length;
  const giveups = tsLines.filter((line) => line.startsWith(`${ArqAction.GAVE_UP} `)).length;
  const deferrals = tsLines.filter((line) => line.startsWith(`${ArqAction.DEFERRED} `)).length;
  console.log(
    `         exercised: ${retransmits} retransmissions, ${giveups} give-ups, ${deferrals} deferrals`,
  );
  console.log(`LH_METRIC test.cross_lang_arq.retransmits value=${retransmits} unit=count budget=10`);
  console.log(`LH_METRIC test.cross_lang_arq.giveups value=${giveups} unit=count budget=1`);
  console.log(`LH_METRIC test.cross_lang_arq.deferrals value=${deferrals} unit=count budget=1`);
} finally {
  rmSync(workDir, { recursive: true, force: true });
}

for (const failure of failures.slice(0, 10)) console.error(`MISMATCH ${failure}`);

console.log(`LH_METRIC test.cross_lang_arq value=${agreed} unit=count budget=${script.length}`);
console.log(`LH_METRIC test.cross_lang_arq.mismatches value=${failures.length} unit=count budget=0`);

if (failures.length > 0) {
  console.error(`\n${failures.length} ARQ steps disagree between C and TypeScript.`);
  process.exit(1);
}
console.log(`OK       ARQ cross-language — ${script.length} scripted steps, identical decisions`);
