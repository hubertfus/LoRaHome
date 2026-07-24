import assert from 'node:assert/strict';
import { test } from 'node:test';
import { formatVerdict, runGate } from '../src/metric-gate.js';
import type { Baseline } from '../src/parse-metrics.js';

/** Builds a commit message with an optional METRICS: block, per §0.2. */
function commit(options: {
  subject?: string;
  metrics?: Record<string, string>;
  justified?: boolean;
}): string {
  const lines = [options.subject ?? 'feat(protocol): do a thing', '', 'Body explaining why.', ''];

  if (options.metrics !== undefined) {
    lines.push('METRICS:');
    for (const [name, value] of Object.entries(options.metrics)) {
      lines.push(`  ${name.padEnd(24)} : ${value}`);
    }
  }
  if (options.justified === true) {
    lines.push('', 'REGRESSION-JUSTIFIED: radio timing needs the extra buffer; agreed with architect.');
  }
  lines.push('ENV:', '  hw=N/A  node=24.18.0', 'BASELINE: deadbee');
  return lines.join('\n');
}

const BASELINE: Baseline = {
  'bench.parse': 100,
  'heap.free.post_init': 200000,
  'heap.delta': 0,
};

/**
 * The 12-case gate matrix from T0.6.
 *
 * Each row states the situation, the input, and the expected verdict. Kept as
 * data rather than twelve near-identical test bodies so that adding a rule
 * means adding a row.
 */
const MATRIX: {
  name: string;
  commitMessage: string;
  metricsOutput?: string;
  expectPass: boolean;
}[] = [
  {
    name: '1. no METRICS: block at all — rejected',
    commitMessage: commit({}),
    expectPass: false,
  },
  {
    name: '2. well-formed metrics, nothing in baseline — accepted',
    commitMessage: commit({ metrics: { 'bench.newthing': '12 ms' } }),
    expectPass: true,
  },
  {
    name: '3. metric over budget — rejected',
    commitMessage: commit({ metrics: { 'bench.parse': '180 us' } }),
    metricsOutput: 'LH_METRIC bench.parse value=180 unit=us budget=150',
    expectPass: false,
  },
  {
    name: '4. metric inside budget — accepted',
    commitMessage: commit({ metrics: { 'bench.parse': '90 us' } }),
    metricsOutput: 'LH_METRIC bench.parse value=90 unit=us budget=150',
    expectPass: true,
  },
  {
    name: '5. 4% regression — accepted (inside tolerance)',
    commitMessage: commit({ metrics: { 'bench.parse': '104' } }),
    expectPass: true,
  },
  {
    name: '6. 6% regression, no justification — rejected',
    commitMessage: commit({ metrics: { 'bench.parse': '106' } }),
    expectPass: false,
  },
  {
    name: '7. 6% regression with REGRESSION-JUSTIFIED — accepted',
    commitMessage: commit({ metrics: { 'bench.parse': '106' }, justified: true }),
    expectPass: true,
  },
  {
    name: '8. budget breach WITH justification — still rejected (budgets are not waivable)',
    commitMessage: commit({ metrics: { 'bench.parse': '180' }, justified: true }),
    metricsOutput: 'LH_METRIC bench.parse value=180 unit=us budget=150',
    expectPass: false,
  },
  {
    name: '9. large improvement — accepted, never counted as regression',
    commitMessage: commit({ metrics: { 'bench.parse': '40' } }),
    expectPass: true,
  },
  {
    name: '10. higher-is-better metric drops 10% — rejected',
    commitMessage: commit({ metrics: { 'heap.free.post_init': '180000' } }),
    expectPass: false,
  },
  {
    name: '11. higher-is-better metric rises — accepted',
    commitMessage: commit({ metrics: { 'heap.free.post_init': '214512' } }),
    expectPass: true,
  },
  {
    name: '12. merge commit with no metrics — exempt, accepted',
    commitMessage: 'Merge branch  feature/radio into main',
    expectPass: true,
  },
];

for (const row of MATRIX) {
  test(`gate matrix — ${row.name}`, () => {
    const verdict = runGate({
      commitMessage: row.commitMessage,
      baseline: BASELINE,
      metricsOutput: row.metricsOutput,
    });
    assert.equal(
      verdict.passed,
      row.expectPass,
      `expected ${row.expectPass ? 'PASS' : 'REJECT'}\n${formatVerdict(verdict)}`,
    );
  });
}

test('gate matrix covers all 12 specified cases', () => {
  assert.equal(MATRIX.length, 12);
});

// --- behaviours worth pinning beyond the matrix ----------------------------

test('a zero baseline rejects movement in the bad direction', () => {
  const verdict = runGate({
    commitMessage: commit({ metrics: { 'heap.delta': '512' } }),
    baseline: BASELINE,
  });
  assert.equal(verdict.passed, false, 'heap growth against a 0 B baseline must fail');
});

test('live benchmark output takes precedence over the hand-typed commit block', () => {
  // The commit says 90; the machine says 180 against a budget of 150. The
  // machine wins — otherwise the gate is only as honest as the typist.
  const verdict = runGate({
    commitMessage: commit({ metrics: { 'bench.parse': '90 us' } }),
    baseline: {},
    metricsOutput: 'LH_METRIC bench.parse value=180 unit=us budget=150',
  });
  assert.equal(verdict.passed, false);
  assert.equal(verdict.result?.breaches[0]?.value, 180);
});

test('rejection messages name the offending metric', () => {
  const verdict = runGate({
    commitMessage: commit({ metrics: { 'bench.parse': '180' } }),
    baseline: BASELINE,
    metricsOutput: 'LH_METRIC bench.parse value=180 unit=us budget=150',
  });
  const text = formatVerdict(verdict);
  assert.match(text, /bench\.parse/);
  assert.match(text, /REJECTED/);
});

test('a revert commit is exempt', () => {
  assert.equal(runGate({ commitMessage: 'Revert "feat: x"', baseline: BASELINE }).passed, true);
});

test('metrics block terminated by ENV: does not swallow the env lines', () => {
  const verdict = runGate({
    commitMessage: commit({ metrics: { 'bench.parse': '50' } }),
    baseline: BASELINE,
  });
  const names = verdict.result?.metrics.map((m) => m.name) ?? [];
  assert.deepEqual(names, ['bench.parse']);
});
