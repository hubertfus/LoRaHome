import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  evaluateGate,
  formatGateResult,
  hasRegressionJustification,
  isEnvironmentSensitive,
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

test('a large percentage change on a tiny absolute value is not a regression', () => {
  // 4 -> 5 B/op is "25% worse" and is jitter. Observed on consecutive runs of
  // unchanged code, which is exactly the sort of thing that gets gates disabled.
  const jitter = evaluateGate(parseMetrics('LH_METRIC mem.x.overhead_b_op value=5 unit=B/op'), {
    'mem.x.overhead_b_op': 4,
  });
  assert.equal(jitter.passed, true);
  assert.equal(jitter.regressions.length, 0);

  const codegen = evaluateGate(parseMetrics('LH_METRIC bench.codegen.ms value=1.4 unit=ms'), {
    'bench.codegen.ms': 1.3,
  });
  assert.equal(codegen.passed, true);
});

test('a real move clears both the relative and the absolute bar', () => {
  const real = evaluateGate(parseMetrics('LH_METRIC mem.x.overhead_b_op value=64 unit=B/op'), {
    'mem.x.overhead_b_op': 4,
  });
  assert.equal(real.passed, false, '4 -> 64 B/op is a genuine regression');

  const slower = evaluateGate(parseMetrics('LH_METRIC bench.codegen.ms value=90 unit=ms'), {
    'bench.codegen.ms': 1.3,
  });
  assert.equal(slower.passed, false);
});

test('the noise floor never softens a budget', () => {
  // Budgets are exact at any magnitude: 5 B against a 4 B budget is a breach
  // even though the delta is below the reporting floor.
  const result = evaluateGate(parseMetrics('LH_METRIC mem.x.b_op value=5 unit=B/op budget=4'), {});
  assert.equal(result.passed, false);
});

test('a changed environment suppresses size/timing regressions but not budgets', () => {
  // Real case: crc16.c compiled to 80 B on a dev laptop and 76 B on CI, purely
  // from a different GCC build. Comparing those measures the compiler.
  const metrics = parseMetrics('LH_METRIC size.crc16.text value=200 unit=B');
  const baseline = { 'size.crc16.text': 80 };

  assert.equal(evaluateGate(metrics, baseline).passed, false, 'same env: a real regression');
  assert.equal(
    evaluateGate(metrics, baseline, { environmentChanged: true }).passed,
    true,
    'different toolchain: not comparable, so not reported',
  );

  // A budget is absolute and survives an environment change.
  const overBudget = parseMetrics('LH_METRIC size.crc16.text value=900 unit=B budget=512');
  assert.equal(evaluateGate(overBudget, baseline, { environmentChanged: true }).passed, false);
});

test('a changed environment suppresses timing regressions named anything', () => {
  // The failure this rule was rewritten for: chaos.suite.runtime.s matches none
  // of the size/bench/mem prefixes, so a laptop's 2.2 s was compared against a
  // shared CI runner's 3.2 s and called a 45% regression in a suite the commit
  // had not touched. What decides comparability is the unit, not the name.
  const metrics = parseMetrics('LH_METRIC chaos.suite.runtime.s value=3.2 unit=s');
  const baseline = { 'chaos.suite.runtime.s': 2.2 };

  assert.equal(evaluateGate(metrics, baseline).passed, false, 'same env: still a regression');
  assert.equal(
    evaluateGate(metrics, baseline, { environmentChanged: true }).passed,
    true,
    'different machine: a wall-clock duration is not comparable',
  );

  // And the budget still bites, whatever the machine.
  const overBudget = parseMetrics('LH_METRIC chaos.suite.runtime.s value=3.2 unit=s budget=3');
  assert.equal(evaluateGate(overBudget, baseline, { environmentChanged: true }).passed, false);
});

test('a changed environment still gates non-environmental metrics', () => {
  // Test counts and assertion counts do not depend on the compiler, so a
  // toolchain change is no excuse for them moving.
  const result = evaluateGate(
    parseMetrics('LH_METRIC count.static_asserts value=3 budget=1'),
    { 'count.static_asserts': 15 },
    { environmentChanged: true },
  );
  assert.equal(result.regressions.length, 1, 'assertion count dropping is still a regression');

  // Counts and percentages stay comparable across machines — which is where
  // most real regressions show up, so the exemption must not swallow them.
  for (const line of [
    'LH_METRIC test.cross_lang_cbor value=100 unit=count',
    'LH_METRIC metric.interval_accuracy value=40 unit=pct',
    'LH_METRIC targets.compiled_clean value=1 unit=targets',
  ]) {
    const metric = parseMetrics(line)[0]!;
    assert.equal(
      isEnvironmentSensitive(metric.name, metric.unit),
      false,
      `${metric.unit} is deterministic across machines`,
    );
  }

  for (const unit of ['s', 'ms', 'us', 'ns', 'B', 'kB', 'MB', 'kB/s', 'MB/s']) {
    assert.equal(
      isEnvironmentSensitive('some.metric', unit),
      true,
      `${unit} is a property of the machine too`,
    );
  }
});

test('high-variance metrics are trended but never fatal', () => {
  // p99 and tinybench throughput move 12-29% between runs on an idle machine.
  // Gating on them at 5% would fail builds for scheduler noise.
  const noisy = evaluateGate(
    parseMetrics(
      [
        'LH_METRIC bench.header.encode.p99 value=300',
        'LH_METRIC bench.tinybench.encodeHeader.ops_s value=1000',
        'LH_METRIC bench.crc16.ts.speedup value=0.5',
      ].join('\n'),
    ),
    {
      'bench.header.encode.p99': 100,
      'bench.tinybench.encodeHeader.ops_s': 9000000,
      'bench.crc16.ts.speedup': 3,
    },
  );
  assert.equal(noisy.passed, true, 'noisy tails must not fail the build');
  assert.equal(noisy.regressions.length, 0);
  assert.equal(noisy.metrics.length, 3, 'but they are still parsed and recorded');
});

test('the stable statistic beside a noisy one still gates', () => {
  // p50 is the one that binds; this is what stops the exemption above from
  // quietly disabling timing regressions altogether.
  const result = evaluateGate(parseMetrics('LH_METRIC bench.header.encode.p50 value=300'), {
    'bench.header.encode.p50': 100,
  });
  assert.equal(result.passed, false);
});

test('compiled_clean treats its budget as a required floor', () => {
  // 2 of 3 ABIs verified is a gap in coverage, not a pass. This is the metric
  // that stops "we skipped a target" from looking identical to "it built".
  const partial = evaluateGate(parseMetrics('LH_METRIC targets.compiled_clean value=2 budget=3'), {});
  assert.equal(partial.passed, false);

  const full = evaluateGate(parseMetrics('LH_METRIC targets.compiled_clean value=3 budget=3'), {});
  assert.equal(full.passed, true);
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

test('adding test cases is an improvement, deleting them is the regression', () => {
  // Etap 2 added a Unity suite and took the native run from 22 cases to 30.
  // Under the default lower-is-better rule that is a 36% "regression" — a build
  // failing because more of the firmware is tested. The direction has to be the
  // other way round for counts of verification performed.
  const more = evaluateGate(parseMetrics('LH_METRIC test.unity.native.cases value=30'), {
    'test.unity.native.cases': 22,
  });
  assert.equal(more.passed, true);
  assert.equal(more.regressions.length, 0);

  const fewer = evaluateGate(parseMetrics('LH_METRIC test.unity.native.cases value=12'), {
    'test.unity.native.cases': 22,
  });
  assert.equal(fewer.passed, false);

  // Same for the assertion counts the native harnesses report.
  const weakened = evaluateGate(parseMetrics('LH_METRIC test.slip.checks value=10'), {
    'test.slip.checks': 40,
  });
  assert.equal(weakened.passed, false);
});

test('a failure count stays lower-is-better even next to its check count', () => {
  // `test.slip.checks` is higher-is-better and `test.slip.failures` must not be
  // swept along with it by a sloppy suffix match.
  const result = evaluateGate(parseMetrics('LH_METRIC test.slip.failures value=3 budget=0'), {});
  assert.equal(result.passed, false);
  assert.equal(result.breaches.length, 1);
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

test('non-numeric METRICS entries are skipped, not parsed as NaN', () => {
  // `compile.flags : -std=c11 -Wall` begins with a '-', which a looser pattern
  // reads as a number and files as NaN — which then appears in release notes
  // as a metric with a NaN trend.
  const message = [
    'feat: x',
    '',
    'METRICS:',
    '  compile.flags            : -std=c11 -Wall -Wextra',
    '  targets.compiled_clean   : 2/3',
    '  bench.real               : 12.5 ms',
    '  bench.absent             : NOT MEASURED',
    'ENV:',
  ].join('\n');

  assert.deepEqual(parseCommitMetricsBlock(message), {
    'targets.compiled_clean': 2,
    'bench.real': 12.5,
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
