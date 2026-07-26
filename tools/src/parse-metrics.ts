/**
 * Turns LH_METRIC lines into structured metrics, and compares them against a
 * committed baseline.
 *
 * This is the piece that makes "metrics in every commit" real rather than
 * ceremonial. Without it, the numbers in commit messages are retyped by hand,
 * which means they are eventually wrong, and a metric nobody trusts is worse
 * than no metric because it still costs review time.
 *
 * Two line shapes are accepted, because host benchmarks and the embedded probe
 * report different things:
 *
 *   LH_METRIC bench.crc16.230B value=40.8 unit=us budget=100
 *   LH_METRIC cfg_apply heap_delta=128 frag_ratio=0.9812 stack_hwm=1104 dt_us=873
 *
 * The first is a single scalar. The second is the §0.4 memory probe, which is
 * expanded into one metric per field (`cfg_apply.heap_delta`, and so on) so
 * that everything downstream — budgets, regressions, release notes — deals with
 * one flat shape.
 */

export interface Metric {
  /** Fully-qualified name, e.g. `bench.crc16.230B` or `cfg_apply.heap_delta`. */
  name: string;
  value: number;
  unit?: string | undefined;
  budget?: number | undefined;
}

export interface Regression {
  name: string;
  baseline: number;
  current: number;
  /** Positive means worse than baseline, expressed as a fraction (0.06 = +6%). */
  changeRatio: number;
}

export interface Breach {
  name: string;
  value: number;
  budget: number;
  unit?: string | undefined;
}

export interface GateResult {
  metrics: Metric[];
  breaches: Breach[];
  regressions: Regression[];
  /** Metrics present in the baseline but missing from this run. */
  missing: string[];
  /**
   * Whether `missing` is meaningful for this comparison.
   *
   * A commit message lists a handful of metrics while the baseline holds
   * dozens, so "missing" is the normal case there and printing it would bury
   * the real findings under noise — which is how a gate ends up switched off.
   * Only a full benchmark run is expected to produce every metric.
   */
  missingIsFatal: boolean;
  passed: boolean;
}

const LH_METRIC_PREFIX = 'LH_METRIC ';

/** Units for the memory-probe line, keyed by field name. */
const PROBE_UNITS: Record<string, string> = {
  heap_delta: 'B',
  frag_ratio: 'ratio',
  stack_hwm: 'B',
  dt_us: 'us',
};

/**
 * Metrics where a larger number is better, so a drop is the regression.
 *
 * Everything else is treated as lower-is-better (times, sizes, byte counts),
 * which is the common case. Matching is by suffix so that
 * `soak.heap_free.start` and `heap.free.post_init` both land correctly.
 */
const HIGHER_IS_BETTER = [
  'heap_free',
  'heap.free',
  'largest_block',
  'heap_largest_block',
  'frag_ratio',
  'throughput',
  'tput',
  'ops_s',
  'hz',
  'cov.lines',
  'count.static_asserts',
  // Compiling fewer ABIs than required is the regression, so its budget is a
  // floor: 2 of 3 targets must fail the gate, not sail through it.
  'compiled_clean',
  // Counts of verification *performed* — fuzz iterations, round trips, bytes
  // pushed through a wraparound test. Etap 1 introduced these and they broke
  // the default assumption in both directions: a budget read as a ceiling made
  // "we checked more than asked" a breach, while quietly dropping the fuzz run
  // from 1,000,000 iterations to 1,000 would have sailed through as an
  // improvement. Doing less checking is the regression.
  'iterations',
  'round_trip',
  'wraparound_bytes',
  'harnesses_run',
  'sanitized_runs',
  'cross_lang',
  'images_linked',
  'e2e_frames',
  'succeeded',
  // Test suites and assertion counts, same argument one step further. Adding a
  // Unity suite took the native run from 22 cases to 30, which the default
  // lower-is-better rule read as a 36% regression — a green build failing
  // because more of the firmware is now tested. The inverse is the real risk:
  // deleting a suite should be what trips the gate.
  'unity.native.cases',
  '.checks',
  'property_events',
  // Delivery rates and the jitter spread, both introduced by the ARQ in T2.4.
  // Their budgets are floors: 99% delivered is a failure against a 99.9% floor,
  // and a jitter standard deviation *below* 100 ms is the retransmission storm
  // of R2.2 waiting for a power cut.
  'delivery',
  'jitter_stddev',
];

function isHigherBetter(name: string): boolean {
  const lower = name.toLowerCase();
  return HIGHER_IS_BETTER.some((token) => lower.includes(token));
}

/**
 * Metrics recorded and trended, but never used to fail a build.
 *
 * Measured, not guessed: across consecutive runs on an unloaded machine, p50
 * held within ~5% while p99 moved 12–29% and tinybench throughput 5–10%. A 5%
 * gate on a statistic with 30% run-to-run spread fires on scheduler noise, and
 * a gate that cries wolf is one people route around — which is the failure mode
 * R0.5 names directly ("metryki skaczą ±40%, tracimy zaufanie do liczb").
 *
 * So the gate binds on the stable statistics — p50, byte sizes, allocation
 * counts, assertion counts, all either deterministic or low-variance — and the
 * tails are kept as trend data a human reads. Tail latency does matter for the
 * RX window, but that budget belongs on-target with the CPU frequency pinned
 * (R0.5), not on a host benchmark sharing a laptop with a browser.
 */
const INFORMATIONAL_PATTERNS = ['.p99', 'tinybench.', '.speedup', 'bitwise_p50', 'alloc_floor'];

export function isInformational(name: string): boolean {
  const lower = name.toLowerCase();
  return INFORMATIONAL_PATTERNS.some((token) => lower.includes(token));
}

/**
 * Smallest absolute change worth calling a regression, by unit.
 *
 * A percentage threshold alone is unusable at small magnitudes: 4 B/op -> 5 B/op
 * is "25% worse" and 1.3 ms -> 1.4 ms is "7.7% worse", and both are measurement
 * jitter. Observed exactly that on consecutive no-op runs. A regression must
 * therefore clear *both* the 5% relative bar and an absolute floor, so the gate
 * only speaks when a human would agree something moved.
 *
 * These are noise floors, not budgets. They govern when a change is worth
 * reporting, never what value is acceptable — budgets stay exact at any size.
 */
const NOISE_FLOOR_BY_UNIT: Record<string, number> = {
  ms: 1,
  s: 0.5,
  us: 1,
  'ns/op': 5,
  B: 8,
  'B/op': 8,
  MB: 1,
  ratio: 0.01,
  targets: 1,
  vectors: 1,
  count: 1,
};

function noiseFloorFor(unit: string | undefined): number {
  return unit === undefined ? 0 : (NOISE_FLOOR_BY_UNIT[unit] ?? 0);
}

function parseNumber(raw: string): number | null {
  // Reject the sentinels the benchmarks emit when something could not be
  // measured. Silently coercing "SKIPPED" to NaN — or worse, to 0 — would let
  // an unmeasured metric look like a passing one.
  if (raw === '' || raw === 'SKIPPED' || raw === 'ABORTED' || raw === 'N/A') return null;
  const value = Number(raw);
  return Number.isFinite(value) ? value : null;
}

/** Extracts every LH_METRIC line from arbitrary text (serial log, CI output). */
export function parseMetrics(text: string): Metric[] {
  const metrics: Metric[] = [];

  for (const rawLine of text.split(/\r?\n/)) {
    const index = rawLine.indexOf(LH_METRIC_PREFIX);
    if (index === -1) continue;

    // Serial output is frequently prefixed with timestamps or log levels, so
    // the marker is located rather than required at column zero.
    const payload = rawLine.slice(index + LH_METRIC_PREFIX.length).trim();
    const [name, ...pairs] = payload.split(/\s+/);
    if (name === undefined || name === '') continue;

    const fields = new Map<string, string>();
    for (const pair of pairs) {
      const eq = pair.indexOf('=');
      if (eq <= 0) continue;
      fields.set(pair.slice(0, eq), pair.slice(eq + 1));
    }

    if (fields.has('value')) {
      const value = parseNumber(fields.get('value')!);
      if (value === null) continue;
      const budget = fields.has('budget') ? parseNumber(fields.get('budget')!) : null;
      metrics.push({
        name,
        value,
        unit: fields.get('unit'),
        budget: budget ?? undefined,
      });
      continue;
    }

    // Memory-probe shape: expand each field into its own metric.
    for (const [field, raw] of fields) {
      const value = parseNumber(raw);
      if (value === null) continue;
      metrics.push({ name: `${name}.${field}`, value, unit: PROBE_UNITS[field] });
    }
  }

  return metrics;
}

/** Baseline file shape: `{ "bench.crc16.230B": 40.8, ... }`. */
export type Baseline = Record<string, number>;

export function toBaseline(metrics: Metric[]): Baseline {
  const baseline: Baseline = {};
  for (const metric of metrics) baseline[metric.name] = metric.value;
  return baseline;
}

export interface GateOptions {
  /** Fractional regression tolerated before failing. Roadmap §0.2 says 5%. */
  regressionTolerance?: number;
  /** Set when the commit message carries a REGRESSION-JUSTIFIED: block. */
  regressionJustified?: boolean;
  /** Fail when a metric present in the baseline is absent from this run. */
  requireAllBaselineMetrics?: boolean;
  /**
   * Set when the baseline was collected on a different platform or toolchain.
   *
   * Suppresses regression checks on environment-sensitive metrics (sizes and
   * timings) while leaving budgets fully enforced. Comparing a byte count from
   * GCC 8.4 against one from GCC 13 measures the compiler, not the change —
   * observed as an unexplained 5% "regression" in size.crc16.text between a
   * developer laptop and CI. Budgets still apply because they are absolute
   * contracts: 80 B or 76 B, both must fit under the limit.
   */
  environmentChanged?: boolean;
}

/** Metrics whose value depends on the compiler or machine, not only the code. */
export function isEnvironmentSensitive(name: string): boolean {
  return /^(size|bench|mem)\./.test(name);
}

/**
 * Applies budgets and the regression rule.
 *
 * A budget breach always fails. A regression beyond tolerance fails unless the
 * commit carries an explicit justification, per §0.2.
 */
export function evaluateGate(
  metrics: Metric[],
  baseline: Baseline,
  options: GateOptions = {},
): GateResult {
  const tolerance = options.regressionTolerance ?? 0.05;
  const breaches: Breach[] = [];
  const regressions: Regression[] = [];

  for (const metric of metrics) {
    // Trended, never fatal. Still reported by the collector and still written to
    // the baseline, so the history is complete.
    if (isInformational(metric.name)) continue;

    if (metric.budget !== undefined) {
      const overBudget = isHigherBetter(metric.name)
        ? metric.value < metric.budget
        : metric.value > metric.budget;
      if (overBudget) {
        breaches.push({
          name: metric.name,
          value: metric.value,
          budget: metric.budget,
          unit: metric.unit,
        });
      }
    }

    const previous = baseline[metric.name];
    if (previous === undefined) continue;

    // Different compiler or machine: the budget above still bound, but a
    // percentage change against an incomparable baseline says nothing.
    if (options.environmentChanged === true && isEnvironmentSensitive(metric.name)) continue;

    // A zero baseline has no meaningful percentage change; any movement away
    // from zero is reported as a regression only if it is in the bad direction.
    let changeRatio: number;
    if (previous === 0) {
      const worsened = isHigherBetter(metric.name) ? metric.value < 0 : metric.value > 0;
      changeRatio = worsened ? Number.POSITIVE_INFINITY : 0;
    } else {
      const delta = (metric.value - previous) / Math.abs(previous);
      changeRatio = isHigherBetter(metric.name) ? -delta : delta;
    }

    // Must clear the relative bar *and* be big enough in absolute terms to be
    // distinguishable from run-to-run jitter.
    const absoluteDelta = Math.abs(metric.value - previous);
    if (changeRatio > tolerance && absoluteDelta > noiseFloorFor(metric.unit)) {
      regressions.push({ name: metric.name, baseline: previous, current: metric.value, changeRatio });
    }
  }

  const seen = new Set(metrics.map((m) => m.name));
  const missing = Object.keys(baseline).filter((name) => !seen.has(name));
  const missingIsFatal = options.requireAllBaselineMetrics === true;

  const passed =
    breaches.length === 0 &&
    (regressions.length === 0 || options.regressionJustified === true) &&
    (!missingIsFatal || missing.length === 0);

  return { metrics, breaches, regressions, missing, missingIsFatal, passed };
}

/** True when a commit message carries the §0.2 justification block. */
export function hasRegressionJustification(commitMessage: string): boolean {
  return /^REGRESSION-JUSTIFIED:/m.test(commitMessage);
}

/** Extracts the METRICS: block of a commit message as `name -> value`. */
export function parseCommitMetricsBlock(commitMessage: string): Baseline {
  const result: Baseline = {};
  const lines = commitMessage.split(/\r?\n/);
  let inBlock = false;

  for (const line of lines) {
    if (/^METRICS:\s*$/.test(line)) {
      inBlock = true;
      continue;
    }
    if (inBlock) {
      // The block ends at the next top-level section (ENV:, BASELINE:, ...).
      if (/^\S/.test(line)) break;
      // Require a digit: `compile.flags : -std=c11 -Wall` starts with a `-` and
      // would otherwise parse as NaN and land in the trend table as a metric.
      const match = line.match(/^\s+([\w.]+)\s*:\s*(-?\d[\d.]*)/);
      if (match) {
        const value = Number(match[2]);
        if (Number.isFinite(value)) result[match[1]!] = value;
      }
    }
  }
  return result;
}

export function formatGateResult(result: GateResult): string {
  const lines: string[] = [];
  for (const breach of result.breaches) {
    lines.push(
      `BUDGET BREACH   ${breach.name} = ${breach.value}${breach.unit ? ' ' + breach.unit : ''} ` +
        `(budget: ${breach.budget})`,
    );
  }
  for (const regression of result.regressions) {
    const pct = Number.isFinite(regression.changeRatio)
      ? `${(regression.changeRatio * 100).toFixed(1)}%`
      : 'inf';
    lines.push(
      `REGRESSION      ${regression.name}: ${regression.baseline} -> ${regression.current} (${pct} worse)`,
    );
  }
  if (result.missingIsFatal) {
    for (const name of result.missing) {
      lines.push(`MISSING         ${name} (present in baseline, absent from this run)`);
    }
  }
  if (lines.length === 0) lines.push(`OK — ${result.metrics.length} metrics, no breaches or regressions`);
  return lines.join('\n');
}
