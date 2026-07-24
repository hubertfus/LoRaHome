/**
 * T0.1 measurement harness — build/typecheck timing + dependency footprint.
 *
 * Run with `pnpm measure:build`. Emits LH_METRIC lines (the same parsable
 * format the embedded probes use, see roadmap §0.4) so that one parser can
 * consume host and target metrics alike.
 *
 * Deliberately dependency-free and un-transpiled: a harness that measures the
 * build must not itself require the build to run.
 */
import { exec } from 'node:child_process';
import { lstat, readdir, readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const execAsync = promisify(exec);
const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));

/**
 * Runs a pnpm script and returns wall-clock duration in seconds.
 *
 * Goes through a shell because pnpm is a .cmd shim on Windows, which Node
 * refuses to execute directly. Every command here is a literal from this file —
 * nothing interpolated from outside.
 */
async function timePnpm(command) {
  const started = process.hrtime.bigint();
  await execAsync(`pnpm ${command}`, {
    cwd: REPO_ROOT,
    maxBuffer: 32 * 1024 * 1024,
  });
  return Number(process.hrtime.bigint() - started) / 1e9;
}

/**
 * Recursive on-disk size in bytes.
 *
 * Uses lstat and never follows symlinks: pnpm's node_modules is a symlink farm
 * pointing into .pnpm, so following links would count the same bytes many times
 * over and can cycle.
 */
async function dirSize(path) {
  let total = 0;
  let entries;
  try {
    entries = await readdir(path, { withFileTypes: true });
  } catch {
    return 0; // not installed yet
  }
  for (const entry of entries) {
    const full = join(path, entry.name);
    if (entry.isSymbolicLink()) continue;
    if (entry.isDirectory()) {
      total += await dirSize(full);
    } else if (entry.isFile()) {
      total += (await lstat(full)).size;
    }
  }
  return total;
}

/** Direct (non-transitive) dependencies declared across the workspace. */
async function countDirectDeps() {
  const manifests = [
    'package.json',
    'packages/protocol/package.json',
    'packages/host/package.json',
    'packages/web/package.json',
    'packages/components/package.json',
  ];
  const names = new Set();
  for (const rel of manifests) {
    const pkg = JSON.parse(await readFile(join(REPO_ROOT, rel), 'utf8'));
    for (const field of ['dependencies', 'devDependencies']) {
      for (const [name, spec] of Object.entries(pkg[field] ?? {})) {
        if (!String(spec).startsWith('workspace:')) names.add(name);
      }
    }
  }
  return names.size;
}

const metrics = [];
const metric = (name, value, unit, budget) => {
  metrics.push({ name, value, unit, budget });
  const budgetNote = budget === undefined ? '' : ` budget=${budget}`;
  console.log(`LH_METRIC ${name} value=${value} unit=${unit}${budgetNote}`);
};

// Cold build: wipe every dist/ first, otherwise tsc reuses output and we would
// be reporting an incremental time under a "cold" label.
await timePnpm('-r clean');
metric('bench.build.cold', (await timePnpm('-r build')).toFixed(1), 's', 60);
metric('bench.typecheck', (await timePnpm('-r typecheck')).toFixed(1), 's', 15);
metric(
  'size.node_modules',
  Math.round((await dirSize(join(REPO_ROOT, 'node_modules'))) / 1024 / 1024),
  'MB',
);
metric('pkg.direct_deps', await countDirectDeps(), 'count');

const breached = metrics.filter((m) => m.budget !== undefined && Number(m.value) > m.budget);
if (breached.length > 0) {
  for (const m of breached) {
    console.error(`BUDGET BREACH: ${m.name} = ${m.value}${m.unit} (budget: ${m.budget}${m.unit})`);
  }
  process.exit(1);
}
