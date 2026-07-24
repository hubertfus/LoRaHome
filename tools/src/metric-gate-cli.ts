/**
 * CLI wrapper around the metric gate.
 *
 *   node tools/dist/src/metric-gate-cli.js --commit-msg <file> [--metrics <file>]
 *                                          [--baseline bench/results/baseline.json]
 *
 * Used from .husky/commit-msg (locally, where it must be fast enough that nobody
 * resents it) and from the metrics-gate CI job.
 */
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { formatVerdict, runGate } from './metric-gate.js';
import type { Baseline } from './parse-metrics.js';

function repoRoot(): string {
  let dir = dirname(fileURLToPath(import.meta.url));
  while (!existsSync(join(dir, 'pnpm-workspace.yaml'))) {
    const parent = dirname(dir);
    if (parent === dir) throw new Error('repo root not found');
    dir = parent;
  }
  return dir;
}

function argValue(flag: string): string | undefined {
  const index = process.argv.indexOf(flag);
  return index === -1 ? undefined : process.argv[index + 1];
}

const started = process.hrtime.bigint();
const root = repoRoot();

const commitMsgPath = argValue('--commit-msg');
if (commitMsgPath === undefined) {
  console.error('usage: metric-gate-cli --commit-msg <file> [--metrics <file>] [--baseline <file>]');
  process.exit(2);
}

const commitMessage = readFileSync(resolve(commitMsgPath), 'utf8');

const baselinePath = resolve(argValue('--baseline') ?? join(root, 'bench/results/baseline.json'));
let baseline: Baseline = {};
if (existsSync(baselinePath)) {
  const parsed = JSON.parse(readFileSync(baselinePath, 'utf8')) as Record<string, unknown>;
  // Tolerate an annotated baseline file: keep the numeric entries, ignore any
  // metadata the humans added around them.
  baseline = Object.fromEntries(
    Object.entries(parsed).filter((entry): entry is [string, number] => typeof entry[1] === 'number'),
  );
}

const metricsPath = argValue('--metrics');
const metricsOutput =
  metricsPath !== undefined && existsSync(resolve(metricsPath))
    ? readFileSync(resolve(metricsPath), 'utf8')
    : undefined;

/**
 * Compares the environment the baseline was recorded in against this run's.
 *
 * Both files carry a `__env` block written by collect-metrics. If they disagree
 * — different platform, different compiler build — size and timing comparisons
 * are between incomparable numbers and the regression check on them is dropped.
 * Absent metadata means "cannot tell", which is treated as unchanged rather
 * than silently disabling half the gate.
 */
function readEnv(path: string | undefined): Record<string, string> | null {
  if (path === undefined || !existsSync(resolve(path))) return null;
  try {
    const parsed = JSON.parse(readFileSync(resolve(path), 'utf8')) as { __env?: unknown };
    const env = parsed.__env;
    return env !== null && typeof env === 'object' ? (env as Record<string, string>) : null;
  } catch {
    return null;
  }
}

const baselineEnv = readEnv(baselinePath);
const latestEnv = readEnv(argValue('--latest'));
const environmentChanged =
  baselineEnv !== null && latestEnv !== null && JSON.stringify(baselineEnv) !== JSON.stringify(latestEnv);

if (environmentChanged) {
  console.log('NOTE: baseline was recorded in a different environment —');
  console.log(`  baseline: ${JSON.stringify(baselineEnv)}`);
  console.log(`  this run: ${JSON.stringify(latestEnv)}`);
  console.log('  budgets still enforced; regression checks on size.*/bench.*/mem.* skipped.');
}

const verdict = runGate({ commitMessage, baseline, metricsOutput, environmentChanged });
const elapsedS = Number(process.hrtime.bigint() - started) / 1e9;

console.log(formatVerdict(verdict));
console.log(`LH_METRIC bench.gate.runtime.s value=${elapsedS.toFixed(3)} unit=s budget=5`);

if (elapsedS > 5) {
  console.error(`BUDGET BREACH: bench.gate.runtime.s = ${elapsedS.toFixed(3)} s (budget: 5 s)`);
  process.exit(1);
}

process.exit(verdict.passed ? 0 : 1);
