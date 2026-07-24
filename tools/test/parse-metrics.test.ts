import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  evaluateGate,
  formatGateResult,
  hasRegressionJustification,
  parseCommitMetricsBlock,
  parseMetrics,
  toBaseline,
} from '../src/parse-metrics.js';

test('parses a scalar metric line with unit and budget', () => {
  const metrics = parseMetrics('LH_METRIC bench.crc16.230B value=40.8 unit=us budget=100');
  assert.deepEqual(metrics, [
    { name: 'bench.crc16.230B', value: 40.8, unit: 'us', budget: 100 },
  ]);
});

test('ignores lines that are not metrics', () => {
  const log = ['starting run', 'LH_METRIC a value=1', 'I (233) wifi: connected', ''].join('\n');
  assert.equal(parseMetrics(log).length, 1);
});

test('finds metrics behind a serial log prefix', () => {
  // ESP-IDF prefixes output with a level and timestamp; requiring column zero
  // would silently drop every on-target metric.
  const metrics = parseMetrics('I (12345) test: LH_METRIC boot.heap value=214512 unit=B');
  assert.equal(metrics.length, 1);
  assert.equal(metrics[0]!.name, 'boot.heap');
  assert.equal(metrics[0]!.value, 214512);
});

test('expands a §0.4 memory-probe line into one metric per field', () => {
  const metrics = parseMetrics(
    'LH_METRIC cfg_apply heap_delta=128 frag_ratio=0.9812 stack_hwm=1104 dt_us=873',
  );
  assert.deepEqual(toBaseline(metrics), {
    'cfg_apply.heap_delta': 128,
    'cfg_apply.frag_ratio': 0.9812,
    'cfg_apply.stack_hwm': 1104,
    'cfg_apply.dt_us': 873,
  });
  assert.equal(metrics.find((m) => m.name === 'cfg_apply.stack_hwm')?.unit, 'B');
});

test('handles a negative heap delta (memory freed)', () => {
  const metrics = parseMetrics('LH_METRIC teardown heap_delta=-4096 frag_ratio=0.5 stack_hwm=900 dt_us=12');
  assert.equal(metrics.find((m) => m.name === 'teardown.heap_delta')?.value, -4096);
});

test('drops unmeasured sentinels rather than coercing them to a number', () => {
  // "SKIPPED" becoming NaN — or worse, 0 — would let an unmeasured metric pass
  // a budget check. Better to have no metric than a fictional one.
  assert.deepEqual(parseMetrics('LH_METRIC mem.header value=SKIPPED unit=B/op'), []);
  assert.deepEqual(parseMetrics('LH_METRIC bench.x value=ABORTED'), []);
  assert.deepEqual(parseMetrics('LH_METRIC bench.y value=N/A'), []);
});

test('flags a budget breach for lower-is-better metrics', () => {
  const metrics = parseMetrics('LH_METRIC bench.parse value=180 unit=us budget=150');
  const result = evaluateGate(metrics, {});
  assert.equal(result.passed, false);
  assert.equal(result.breaches.length, 1);
  assert.equal(result.breaches[0]!.name, 'bench.parse');
});

test('treats a budget as a floor for higher-is-better metrics', () => {
  // heap_free has a *minimum* budget: 170 kB free against a 180 kB floor is a
  // breach, even though the number is smaller than the budget.
  const low = evaluateGate(parseMetrics('LH_METRIC heap.free.post_init value=170000 budget=180000'), {});
  assert.equal(low.passed, false);

  const ok = evaluateGate(parseMetrics('LH_METRIC heap.free.post_init value=214512 budget=180000'), {});
  assert.equal(ok.passed, true);
});

test('passes a 4% regression and fails a 6% one', () => {
  const baseline = { 'bench.parse': 100 };
  assert.equal(evaluateGate(parseMetrics('LH_METRIC bench.parse value=104'), baseline).passed, true);

  const over = evaluateGate(parseMetrics('LH_METRIC bench.parse value=106'), baseline);
  assert.equal(over.passed, false);
  assert.equal(over.regressions.length, 1);
  assert.ok(Math.abs(over.regressions[0]!.changeRatio - 0.06) < 1e-9);
});

test('an improvement is never a regression', () => {
  const result = evaluateGate(parseMetrics('LH_METRIC bench.parse value=50'), { 'bench.parse': 100 });
  assert.equal(result.passed, true);
  assert.equal(result.regressions.length, 0);
});

test('a drop in a higher-is-better metric counts as a regression', () => {
  const result = evaluateGate(parseMetrics('LH_METRIC heap.free value=90000'), { 'heap.free': 100000 });
  assert.equal(result.regressions.length, 1);
  assert.equal(result.passed, false);
});

test('a rise in a higher-is-better metric does not', () => {
  const result = evaluateGate(parseMetrics('LH_METRIC heap.free value=110000'), { 'heap.free': 100000 });
  assert.equal(result.passed, true);
});

test('REGRESSION-JUSTIFIED allows a regression but never a budget breach', () => {
  const baseline = { 'bench.parse': 100 };

  const justified = evaluateGate(parseMetrics('LH_METRIC bench.parse value=106'), baseline, {
    regressionJustified: true,
  });
  assert.equal(justified.passed, true);
  assert.equal(justified.regressions.length, 1, 'still reported, just not fatal');

  const breach = evaluateGate(parseMetrics('LH_METRIC bench.parse value=106 budget=105'), baseline, {
    regressionJustified: true,
  });
  assert.equal(breach.passed, false, 'a budget is a contract, not a preference');
});

test('a zero baseline treats any movement in the bad direction as a regression', () => {
  const worse = evaluateGate(parseMetrics('LH_METRIC heap.delta value=512'), { 'heap.delta': 0 });
  assert.equal(worse.passed, false);

  const still = evaluateGate(parseMetrics('LH_METRIC heap.delta value=0'), { 'heap.delta': 0 });
  assert.equal(still.passed, true);
});

test('missing baseline metrics are reported, and fatal only when required', () => {
  const baseline = { 'bench.a': 1, 'bench.b': 2 };
  const metrics = parseMetrics('LH_METRIC bench.a value=1');

  assert.deepEqual(evaluateGate(metrics, baseline).missing, ['bench.b']);
  assert.equal(evaluateGate(metrics, baseline).passed, true);
  assert.equal(evaluateGate(metrics, baseline, { requireAllBaselineMetrics: true }).passed, false);
});

test('detects the REGRESSION-JUSTIFIED block in a commit message', () => {
  assert.equal(hasRegressionJustification('feat: x\n\nREGRESSION-JUSTIFIED: radio needs it\n'), true);
  assert.equal(hasRegressionJustification('feat: x\n\nnope\n'), false);
  // Must be its own line, not a mention inside prose.
  assert.equal(hasRegressionJustification('we should add REGRESSION-JUSTIFIED: later'), false);
});

test('parses the METRICS: block out of a commit message', () => {
  const message = [
    'feat(protocol): something',
    '',
    'Body text.',
    '',
    'METRICS:',
    '  bench.crc16.230B     : 40.8 us     (budget: 100)',
    '  size.crc16.text      : 196 B',
    'ENV:',
    '  hw=ESP32-S3  idf=5.2.1',
    'BASELINE: a91f3c2',
  ].join('\n');

  assert.deepEqual(parseCommitMetricsBlock(message), {
    'bench.crc16.230B': 40.8,
    'size.crc16.text': 196,
  });
});

test('formats a clean run and a failing run readably', () => {
  const clean = evaluateGate(parseMetrics('LH_METRIC bench.a value=1 budget=2'), {});
  assert.match(formatGateResult(clean), /^OK — 1 metrics/);

  const failing = evaluateGate(parseMetrics('LH_METRIC bench.a value=9 budget=2'), { 'bench.a': 1 });
  const text = formatGateResult(failing);
  assert.match(text, /BUDGET BREACH\s+bench\.a/);
  assert.match(text, /REGRESSION\s+bench\.a: 1 -> 9/);
});

test('parses a full multi-metric run end to end', () => {
  const runOutput = [
    '> pnpm bench',
    'LH_METRIC bench.header.encode.p50 value=75.2 unit=ns/op budget=200',
    'LH_METRIC bench.header.decode.p50 value=61.7 unit=ns/op budget=200',
    'LH_METRIC mem.header.encode_into.b_op value=0 unit=B/op budget=1',
    'I (940) app: LH_METRIC boot heap_delta=0 frag_ratio=0.9531 stack_hwm=1104 dt_us=118000',
    'done',
  ].join('\n');

  const metrics = parseMetrics(runOutput);
  assert.equal(metrics.length, 7);

  const result = evaluateGate(metrics, {
    'bench.header.encode.p50': 74.0,
    'boot.stack_hwm': 1100,
  });
  assert.equal(result.passed, true, formatGateResult(result));
});
