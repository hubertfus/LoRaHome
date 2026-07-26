/**
 * T2.5 benchmarks for the link simulator.
 *
 * Throughput is the metric that decides whether the chaos suite is a regression
 * test or a nightly job. 100k frames through five profiles has to finish while
 * somebody is still looking at the screen; the roadmap's floor of 10k frames/s
 * puts that run in the tens of seconds, which is the difference between a suite
 * that runs on every pull request and one that gets disabled.
 *
 * Determinism is re-measured here rather than left to the unit test, because it
 * is the property every other number in Etap 2 rests on: if the simulator drifts
 * between runs, every chaos result becomes an anecdote. Cheap to check, and the
 * metric belongs in the history alongside the throughput it enables.
 */
import { SimLink } from '../src/transport/sim-link.js';

const FRAME_BYTES = 230;
const BATCH = 20_000;
const ROUNDS = 20;
const WARMUP_ROUNDS = 3;

const frame = new Uint8Array(FRAME_BYTES);
for (let i = 0; i < frame.length; i++) frame[i] = (i * 37 + 11) & 0xff;

let sink = 0;

function report(name: string, value: number, unit: string, budget?: number, note = ''): void {
  const budgetNote = budget === undefined ? '' : ` budget=${budget}`;
  console.log(`LH_METRIC ${name} value=${value.toFixed(3)} unit=${unit}${budgetNote}${note}`);
}

/**
 * Frames per second through a link under a realistic mix of impairments.
 *
 * The delivery path is included — a listener is attached and the clock is
 * advanced — because a simulator measured with nothing listening would report
 * the speed of its queue rather than the speed of a test using it.
 */
function measureThroughput(profile: Record<string, number>): number {
  const framesPerSecond: number[] = [];

  for (let round = 0; round < WARMUP_ROUNDS + ROUNDS; round++) {
    const link = new SimLink({ seed: 0x8f2a, latencyMs: 10, ...profile });
    link.b.onFrame((received) => {
      sink += received.length;
    });

    const started = process.hrtime.bigint();
    for (let i = 0; i < BATCH; i++) {
      link.a.send(frame);
      link.advance(11);
    }
    link.drain(50);
    const seconds = Number(process.hrtime.bigint() - started) / 1e9;

    if (round >= WARMUP_ROUNDS) framesPerSecond.push(BATCH / seconds);
  }

  framesPerSecond.sort((a, b) => a - b);
  return framesPerSecond[Math.floor(framesPerSecond.length / 2)]!;
}

/** Same seed, same trace — compared as text, not as counters. */
function measureDeterminism(runs: number): number {
  const traceFor = (seed: number): string => {
    const link = new SimLink({
      seed,
      lossPct: 20,
      corruptPct: 10,
      reorderPct: 15,
      duplicatePct: 5,
      jitterMs: 50,
      latencyMs: 100,
    });
    link.startTrace();
    link.b.onFrame(() => {
      sink++;
    });
    for (let i = 0; i < 500; i++) {
      link.a.send(frame.subarray(0, 32));
      link.advance(120);
    }
    link.drain();
    return link.trace.join('\n');
  };

  const reference = traceFor(0x8f2a);
  let identical = 0;
  for (let run = 0; run < runs; run++) {
    if (traceFor(0x8f2a) === reference) identical++;
  }
  return identical;
}

const clean = measureThroughput({});
const chaotic = measureThroughput({ lossPct: 30, corruptPct: 10, reorderPct: 15, jitterMs: 40 });
const identical = measureDeterminism(100);

report('bench.sim.throughput', clean, 'fps', 10000);
report('bench.sim.throughput.chaotic', chaotic, 'fps', 10000, ' (loss30 + corrupt + reorder)');
console.log(`LH_METRIC test.determinism.runs value=${identical} unit=count budget=100`);

if (sink === Number.MAX_SAFE_INTEGER) console.log('unreachable', sink);
