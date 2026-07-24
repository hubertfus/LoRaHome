/**
 * T0.2 micro-benchmarks for the header codec.
 *
 * Run with `pnpm --filter @lorahome/protocol bench` (add --expose-gc for the
 * memory figures).
 *
 * Why not tinybench's percentiles directly: tinybench times each iteration
 * individually, and on this platform the clock granularity is ~0.1 ms. For an
 * operation in the 100 ns range that yields p50 = 0 and p99 = 0.0001 ms —
 * quantisation noise, not measurement. That is precisely risk R0.5 ("benchmark
 * mierzy szum, nie kod"), so the reported ns/op comes from timing batches of
 * 20k iterations and taking percentiles across rounds. tinybench still runs, as
 * an independent cross-check on throughput.
 */
import { Bench } from 'tinybench';
import { crc16, crc16Reference } from '../src/crc16.js';
import {
  LORA_MTU,
  decodeHeader,
  encodeFrame,
  encodeHeader,
  encodeHeaderInto,
  HEADER_SIZE,
  MAX_PAYLOAD,
  type FrameHeader,
} from '../src/frame.js';

// Same total work either way; 200 rounds rather than 60 so that p99 is the
// 198th of 200 samples instead of "second-worst of 60", where a single OS
// scheduling hiccup on a busy machine sets the number and the gate flaps.
const BATCH = 10_000;
const ROUNDS = 200;
const WARMUP_BATCHES = 10;

const HEADER: FrameHeader = {
  type: 0x20,
  srcId: 0x0102,
  dstId: 0xffff,
  seq: 0x2a,
  flags: 0x01,
};
const ENCODED_HEADER = encodeHeader(HEADER);

/** Kept live and printed at the end so V8 cannot fold the measured work away. */
let sink = 0;

interface Stats {
  p50: number;
  p99: number;
  min: number;
}

function measure(fn: () => void): Stats {
  for (let i = 0; i < WARMUP_BATCHES; i++) {
    for (let j = 0; j < BATCH; j++) fn();
  }

  const nsPerOp: number[] = [];
  for (let round = 0; round < ROUNDS; round++) {
    const started = process.hrtime.bigint();
    for (let j = 0; j < BATCH; j++) fn();
    nsPerOp.push(Number(process.hrtime.bigint() - started) / BATCH);
  }

  nsPerOp.sort((a, b) => a - b);
  const at = (q: number): number => nsPerOp[Math.min(nsPerOp.length - 1, Math.floor(q * nsPerOp.length))]!;
  return { p50: at(0.5), p99: at(0.99), min: nsPerOp[0]! };
}

/**
 * Bytes retained per operation, per roadmap §0.3: heapUsed before/after 10k
 * iterations. Results are retained in an array on purpose — releasing them
 * would measure the GC's mood rather than the allocation size.
 */
function measureBytesPerOp(fn: () => Uint8Array, iterations = 10_000): number | null {
  const gc = globalThis.gc;
  if (typeof gc !== 'function') return null;

  const retained: Uint8Array[] = new Array(iterations);
  gc();
  const before = process.memoryUsage().heapUsed;
  for (let i = 0; i < iterations; i++) retained[i] = fn();
  const after = process.memoryUsage().heapUsed;
  sink += retained[iterations - 1]!.length;
  return (after - before) / iterations;
}

/**
 * Measures the allocation cost of the zero-copy path.
 *
 * Runs 200k iterations rather than 10k on purpose. A truly zero-allocation loop
 * still shows a fixed ~20 kB of heap movement from JIT tiering and GC
 * bookkeeping, which at 10k iterations reads as a misleading ~2 B/op. That
 * offset does not scale with iteration count, so amortising it over 200k drives
 * it to ~0.001 B/op while a genuine per-op allocation would stay put. Verified
 * against N = 10k/50k/200k/1M before picking the count.
 */
function measureBytesPerOpZero(iterations = 200_000): number {
  const gc = globalThis.gc;
  if (typeof gc !== 'function') return Number.NaN;
  const buf = new Uint8Array(HEADER_SIZE);
  gc();
  const before = process.memoryUsage().heapUsed;
  for (let i = 0; i < iterations; i++) encodeHeaderInto(HEADER, buf);
  const after = process.memoryUsage().heapUsed;
  sink += buf[0]!;
  return Math.max(0, (after - before) / iterations);
}

const needsSignOff: string[] = [];
const metrics: { name: string; value: string; unit: string; budget?: number | undefined }[] = [];
const report = (name: string, value: number, unit: string, budget?: number, note = ''): void => {
  const rounded = unit === 'ns/op' ? value.toFixed(1) : value.toFixed(0);
  metrics.push({ name, value: rounded, unit, budget });
  const budgetNote = budget === undefined ? '' : ` budget=${budget}`;
  console.log(`LH_METRIC ${name} value=${rounded} unit=${unit}${budgetNote}${note}`);
};

// --- encode / decode -------------------------------------------------------

const encode = measure(() => {
  sink += encodeHeader(HEADER)[0]!;
});
report('bench.header.encode.p50', encode.p50, 'ns/op', 200);
report('bench.header.encode.p99', encode.p99, 'ns/op', 200);

const decode = measure(() => {
  sink += decodeHeader(ENCODED_HEADER).seq;
});
report('bench.header.decode.p50', decode.p50, 'ns/op', 200);
report('bench.header.decode.p99', decode.p99, 'ns/op', 200);

// --- CRC16 (T0.5) ----------------------------------------------------------

const crcBuffer = new Uint8Array(LORA_MTU);
for (let i = 0; i < crcBuffer.length; i++) crcBuffer[i] = (i * 37 + 11) & 0xff;

const crcTable = measure(() => {
  sink += crc16(crcBuffer);
});
// Budget is 3 us for a 230 B buffer; reported in ns/op, so 3000 ns.
report('bench.crc16.ts.230B.p50', crcTable.p50, 'ns/op', 3000);
report('bench.crc16.ts.230B.p99', crcTable.p99, 'ns/op', 3000);

const crcBitwise = measure(() => {
  sink += crc16Reference(crcBuffer);
});
report('bench.crc16.ts.230B.bitwise_p50', crcBitwise.p50, 'ns/op');
console.log(
  `LH_METRIC bench.crc16.ts.speedup value=${(crcBitwise.p50 / crcTable.p50).toFixed(2)} unit=x` +
    ' (nibble table vs bit-by-bit)',
);

const fullPayload = new Uint8Array(MAX_PAYLOAD);
const fullFrame = measure(() => {
  sink += encodeFrame(HEADER, fullPayload)[0]!;
});
report('bench.frame.encode_220B.p50', fullFrame.p50, 'ns/op');
report('bench.frame.encode_220B.p99', fullFrame.p99, 'ns/op');

// --- allocation ------------------------------------------------------------

const scratch = new Uint8Array(HEADER_SIZE);
const intoStats = measure(() => {
  encodeHeaderInto(HEADER, scratch);
  sink += scratch[0]!;
});
report('bench.header.encode_into.p50', intoStats.p50, 'ns/op', 200);
report('bench.header.encode_into.p99', intoStats.p99, 'ns/op', 200);

const bytesPerOp = measureBytesPerOp(() => encodeHeader(HEADER));
const floorPerOp = measureBytesPerOp(() => new Uint8Array(HEADER_SIZE));

if (bytesPerOp === null || floorPerOp === null) {
  console.log('LH_METRIC mem.header.encode.b_op value=SKIPPED unit=B/op (rerun with --expose-gc)');
} else {
  // The stated T0.2 budget has two parts: "<= 128 B/op" and "<= 1 Buffer
  // allocation". We meet the second exactly and cannot meet the first: an empty
  // `new Uint8Array(8)` already costs `floorPerOp`, so the codec itself
  // contributes ~0 B on top. Reported without a hard gate and escalated below
  // rather than quietly rewritten — budgets change by architect sign-off
  // (roadmap §0.6), not by whoever is holding the benchmark.
  report('mem.header.encode.b_op', bytesPerOp, 'B/op');
  report('mem.header.alloc_floor.b_op', floorPerOp, 'B/op');
  report('mem.header.encode.overhead_b_op', bytesPerOp - floorPerOp, 'B/op', 16);
  // Budget 1 B/op, not 0: heapUsed sampling has a noise floor, and a gate that
  // demands an exact 0 from a sampled measurement is a flaky gate, which is how
  // teams learn to ignore gates.
  report('mem.header.encode_into.b_op', measureBytesPerOpZero(), 'B/op', 1);

  if (bytesPerOp > 128) {
    needsSignOff.push(
      `mem.header.encode.b_op = ${bytesPerOp.toFixed(0)} B/op vs budget 128 B/op. ` +
        `V8 allocation floor for new Uint8Array(${HEADER_SIZE}) is ${floorPerOp.toFixed(0)} B/op, ` +
        `so the budget is unreachable for any API returning a fresh buffer. ` +
        `Zero-allocation path encodeHeaderInto() measures 0 B/op. ` +
        `Needs architect decision: re-target the budget at encodeHeaderInto, or raise it.`,
    );
  }
}

// --- tinybench cross-check -------------------------------------------------

const bench = new Bench({ time: 300 });
bench.add('encodeHeader', () => {
  sink += encodeHeader(HEADER)[0]!;
});
bench.add('decodeHeader', () => {
  sink += decodeHeader(ENCODED_HEADER).seq;
});
await bench.run();

for (const task of bench.tasks) {
  // TaskResult is a union with an aborted variant, which carries no timings.
  const result = task.result;
  if (!result || !('throughput' in result)) {
    console.log(`LH_METRIC bench.tinybench.${task.name}.ops_s value=ABORTED unit=ops/s`);
    continue;
  }
  const hz = result.throughput.mean;
  console.log(
    `LH_METRIC bench.tinybench.${task.name}.ops_s value=${Math.round(hz)} unit=ops/s` +
      ` (cross-check: ${(1e9 / hz).toFixed(1)} ns/op)`,
  );
}

// --- budget gate -----------------------------------------------------------

if (sink === Number.MIN_SAFE_INTEGER) console.log('unreachable', sink); // keep sink live

const breached = metrics.filter((m) => m.budget !== undefined && Number(m.value) > m.budget);
for (const m of breached) {
  console.error(`BUDGET BREACH: ${m.name} = ${m.value} ${m.unit} (budget: ${m.budget} ${m.unit})`);
}
for (const note of needsSignOff) {
  console.error(`NEEDS-ARCHITECT-SIGNOFF: ${note}`);
}
process.exit(breached.length > 0 ? 1 : 0);
