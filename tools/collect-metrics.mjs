/**
 * Runs every measurable thing in the repo and aggregates the LH_METRIC lines.
 *
 *   node tools/collect-metrics.mjs                 -> bench/results/latest.json
 *   node tools/collect-metrics.mjs --write-baseline -> also updates baseline.json
 *
 * baseline.json is the zero point of the metric history: every future
 * regression is measured against it, so it is committed and latest.json is not.
 * Writing it is deliberately an explicit flag rather than a side effect — a
 * baseline that silently updates itself can never detect a regression, because
 * every run redefines "normal".
 *
 * Steps that cannot run in this environment (no target hardware, no host C
 * compiler) contribute nothing rather than contributing zeroes. Their absence
 * is visible in the `sources` list.
 */
import { execSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));
const RESULTS_DIR = join(REPO_ROOT, 'bench', 'results');

const STEPS = [
  { id: 'codegen', command: 'pnpm gen:c' },
  { id: 'targets', command: 'node tools/compile-targets.mjs' },
  { id: 'native', command: 'node tools/run-native.mjs' },
  { id: 'firmware', command: 'node tools/build-firmware.mjs' },
  { id: 'airtime', command: 'node tools/check-airtime.mjs' },
  { id: 'protocol-bench', command: 'pnpm --filter @lorahome/protocol bench' },
];

/** Parses `LH_METRIC <name> value=<v> [unit=<u>] [budget=<b>]` and probe lines. */
function parseLine(line) {
  const marker = line.indexOf('LH_METRIC ');
  if (marker === -1) return [];

  const [name, ...pairs] = line
    .slice(marker + 'LH_METRIC '.length)
    .trim()
    .split(/\s+/);
  if (!name) return [];

  const fields = new Map();
  for (const pair of pairs) {
    const eq = pair.indexOf('=');
    if (eq > 0) fields.set(pair.slice(0, eq), pair.slice(eq + 1));
  }

  const numeric = (raw) => {
    if (raw === undefined || raw === 'SKIPPED' || raw === 'ABORTED' || raw === 'N/A') return null;
    const value = Number(raw);
    return Number.isFinite(value) ? value : null;
  };

  if (fields.has('value')) {
    const value = numeric(fields.get('value'));
    return value === null ? [] : [[name, value]];
  }

  const out = [];
  for (const [field, raw] of fields) {
    const value = numeric(raw);
    if (value !== null) out.push([`${name}.${field}`, value]);
  }
  return out;
}

const metrics = {};
const sources = [];
/**
 * Raw LH_METRIC lines, kept verbatim.
 *
 * The gate consumes these rather than the JSON summary: budgets live on the
 * emitted line and are lost once a metric is flattened to name -> value, so a
 * gate fed the JSON would silently check no budgets at all.
 */
const rawLines = [];

/**
 * Which machine produced these numbers.
 *
 * Stored alongside the metrics because byte sizes and timings are properties of
 * the environment as much as of the code: the same crc16.c measured 80 B on a
 * developer laptop and 76 B on CI, from nothing but a different GCC build. A
 * gate that cannot tell a toolchain upgrade from a code regression will report
 * the upgrade as a regression, and get switched off.
 */
const env = {
  platform: `${process.platform}-${process.arch}`,
  node: process.versions.node,
};

for (const step of STEPS) {
  let output = '';
  let ok = true;
  try {
    output = execSync(step.command, { cwd: REPO_ROOT, encoding: 'utf8', stdio: 'pipe' });
  } catch (error) {
    // A step can exit non-zero because a budget was breached while still having
    // produced perfectly good measurements. Keep them; the gate decides.
    output = `${error.stdout ?? ''}${error.stderr ?? ''}`;
    ok = false;
  }

  let count = 0;
  for (const line of output.split('\n')) {
    const envMarker = line.indexOf('LH_ENV ');
    if (envMarker !== -1) {
      const payload = line.slice(envMarker + 'LH_ENV '.length).trim();
      const eq = payload.indexOf('=');
      if (eq > 0) env[payload.slice(0, eq)] = payload.slice(eq + 1);
      continue;
    }

    const parsed = parseLine(line);
    if (parsed.length > 0) rawLines.push(line.slice(line.indexOf('LH_METRIC ')).trimEnd());
    for (const [name, value] of parsed) {
      metrics[name] = value;
      count++;
    }
  }
  sources.push({ id: step.id, command: step.command, exitOk: ok, metrics: count });
  console.log(`${ok ? 'ok  ' : 'warn'} ${step.id.padEnd(16)} ${count} metrics`);
}

mkdirSync(RESULTS_DIR, { recursive: true });

writeFileSync(
  join(RESULTS_DIR, 'latest.json'),
  JSON.stringify({ __env: env, sources, metrics }, null, 2) + '\n',
  'utf8',
);
writeFileSync(join(RESULTS_DIR, 'latest.log'), rawLines.join('\n') + '\n', 'utf8');
console.log(
  `\nwrote bench/results/latest.json and latest.log (${Object.keys(metrics).length} metrics)`,
);

if (process.argv.includes('--write-baseline')) {
  const baselinePath = join(RESULTS_DIR, 'baseline.json');
  const previous = existsSync(baselinePath)
    ? JSON.parse(readFileSync(baselinePath, 'utf8'))
    : {};

  // __env first so a reader sees the provenance before the numbers. The gate
  // ignores non-numeric baseline entries, so this key needs no special casing
  // there beyond the comparison it enables.
  writeFileSync(
    baselinePath,
    JSON.stringify({ __env: env, ...metrics }, null, 2) + '\n',
    'utf8',
  );

  const before = Object.keys(previous).filter((k) => k !== '__env').length;
  const after = Object.keys(metrics).length;
  console.log(`wrote bench/results/baseline.json (${before} -> ${after} metrics)`);
  console.log(`  env: ${Object.entries(env).map(([k, v]) => `${k}=${v}`).join('  ')}`);
}
