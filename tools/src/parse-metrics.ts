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
];

function isHigherBetter(name: string): boolean {
  const lower = name.toLowerCase();
  return HIGHER_IS_BETTER.some((token) => lower.includes(token));
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

    if (changeRatio > tolerance) {
      regressions.push({ name: metric.name, baseline: previous, current: metric.value, changeRatio });
    }
  }

  const seen = new Set(metrics.map((m) => m.name));
  const missing = Object.keys(baseline).filter((name) => !seen.has(name));

  const passed =
    breaches.length === 0 &&
    (regressions.length === 0 || options.regressionJustified === true) &&
    (options.requireAllBaselineMetrics !== true || missing.length === 0);

  return { metrics, breaches, regressions, missing, passed };
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
      const match = line.match(/^\s+([\w.]+)\s*:\s*([-\d.]+)/);
      if (match) result[match[1]!] = Number(match[2]);
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
  for (const name of result.missing) {
    lines.push(`MISSING         ${name} (present in baseline, absent from this run)`);
  }
  if (lines.length === 0) lines.push(`OK — ${result.metrics.length} metrics, no breaches or regressions`);
  return lines.join('\n');
}
