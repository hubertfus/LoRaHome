/**
 * The metric gate. Roadmap T0.6 and §0.2.
 *
 * Rules, in the order they bite:
 *
 *   1. A commit with no METRICS: block is rejected. §0.1 is explicit that a
 *      commit passing its correctness tests but carrying no numbers is not a
 *      complete commit.
 *   2. Any metric over (or, for higher-is-better metrics, under) its budget
 *      fails. Budgets are contracts; they are never waivable by the committer.
 *   3. Any metric more than 5% worse than the baseline fails, unless the commit
 *      carries a REGRESSION-JUSTIFIED: block.
 *
 * The asymmetry between 2 and 3 is the whole design. A regression is a local
 * judgement call an engineer can make and defend in the commit message. A
 * budget is a system-wide invariant somebody agreed to; changing it takes a
 * separate `chore(budget):` commit, per §0.6.
 */
import { evaluateGate, formatGateResult, hasRegressionJustification, parseCommitMetricsBlock, parseMetrics } from './parse-metrics.js';
import type { Baseline, GateResult } from './parse-metrics.js';

export interface GateInput {
  /** Full commit message, including the METRICS: block. */
  commitMessage: string;
  /** Committed baseline: metric name -> value. */
  baseline: Baseline;
  /**
   * Raw output from a benchmark run, if available. Live LH_METRIC lines carry
   * budgets, which a commit message's METRICS: block does not, so this is the
   * only input that can trigger a budget breach.
   */
  metricsOutput?: string | undefined;
  /** Exempt merge/revert/fixup commits, which carry no work of their own. */
  allowMissingMetrics?: boolean | undefined;
}

export interface GateVerdict {
  passed: boolean;
  reasons: string[];
  result: GateResult | null;
  metricCount: number;
}

/** Commits that legitimately carry no measurements of their own. */
function isExemptCommit(message: string): boolean {
  return /^(Merge |Revert |fixup!|squash!)/.test(message.trimStart());
}

export function runGate(input: GateInput): GateVerdict {
  const reasons: string[] = [];
  const { commitMessage, baseline } = input;

  if (isExemptCommit(commitMessage) || input.allowMissingMetrics === true) {
    return { passed: true, reasons: ['exempt commit (merge/revert/fixup)'], result: null, metricCount: 0 };
  }

  const fromCommit = parseCommitMetricsBlock(commitMessage);
  const commitMetricNames = Object.keys(fromCommit);

  if (commitMetricNames.length === 0) {
    reasons.push(
      'commit message has no METRICS: block — see roadmap §0.2 for the required format',
    );
    return { passed: false, reasons, result: null, metricCount: 0 };
  }

  // Live benchmark output is authoritative where present: it carries budgets and
  // has not been through a human's keyboard. The commit block fills in metrics
  // that only a human can supply (on-target numbers read off a board).
  const liveMetrics = input.metricsOutput === undefined ? [] : parseMetrics(input.metricsOutput);
  const liveNames = new Set(liveMetrics.map((m) => m.name));

  const commitOnlyMetrics = commitMetricNames
    .filter((name) => !liveNames.has(name))
    .map((name) => ({ name, value: fromCommit[name]!, unit: undefined, budget: undefined }));

  const metrics = [...liveMetrics, ...commitOnlyMetrics];

  const result = evaluateGate(metrics, baseline, {
    regressionJustified: hasRegressionJustification(commitMessage),
  });

  if (result.breaches.length > 0) {
    reasons.push(`${result.breaches.length} budget breach(es)`);
  }
  if (result.regressions.length > 0 && !hasRegressionJustification(commitMessage)) {
    reasons.push(
      `${result.regressions.length} regression(s) over 5% without a REGRESSION-JUSTIFIED: block`,
    );
  }

  return { passed: result.passed, reasons, result, metricCount: metrics.length };
}

export function formatVerdict(verdict: GateVerdict): string {
  const lines: string[] = [];
  if (verdict.result !== null) lines.push(formatGateResult(verdict.result));
  for (const reason of verdict.reasons) lines.push(verdict.passed ? `note: ${reason}` : `REJECTED: ${reason}`);
  return lines.join('\n');
}
