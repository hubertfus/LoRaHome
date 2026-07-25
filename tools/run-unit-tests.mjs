/**
 * Runs the firmware's Unity test suites on the host.
 *
 * `firmware/node/platformio.ini` has declared a `native` environment since the
 * scaffold, and until now nothing ever ran it: it shells out to bare `gcc`/`g++`
 * and there was no host GNU toolchain on this machine. So `test_protocol`,
 * `test_slip`, `test_ring` and `test_rule_evaluator` — 22 cases — had never been
 * executed anywhere, not locally and not in CI. They passed on first run, which
 * is luck rather than evidence, and exactly why this now runs every time.
 *
 * These are not a duplicate of the harnesses in tools/run-native.mjs. Those
 * measure and fuzz at volume; these are Unity suites that also compile for the
 * ESP32 target, so keeping them green on the host is what keeps them honest as
 * on-target tests — a Unity file that has drifted out of compiling is a test
 * nobody will run when the board finally arrives.
 *
 * Skips cleanly with no host g++ or no PlatformIO, contributing nothing rather
 * than a zero.
 */
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { findHostToolchains } from './host-cc.mjs';
import { environmentWithCompiler, findPlatformIO, platformioVersion } from './platformio.mjs';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));

/** Projects with a `native` test environment. */
const PROJECTS = [{ id: 'node', dir: join(REPO_ROOT, 'firmware', 'node'), env: 'native' }];

function skip(reason) {
  console.log(`SKIPPED  ${reason}`);
  console.log('LH_METRIC test.unity.native.cases value=SKIPPED unit=count');
  process.exit(0);
}

const pio = findPlatformIO();
if (pio === null) skip('PlatformIO not installed — the native Unity suites were not run');

/**
 * A GNU toolchain that also ships g++.
 *
 * The Unity suites are C++ translation units, and PlatformIO's native platform
 * invokes `g++` by name. MSVC cannot stand in: it is neither called `g++` nor
 * command-line compatible with it.
 */
function findGxx() {
  for (const toolchain of findHostToolchains()) {
    if (toolchain.kind !== 'gnu') continue;
    // On PATH already — let PlatformIO find it itself.
    if (!toolchain.id.includes('/') && !toolchain.id.includes('\\')) return toolchain.id;

    const gxx = join(dirname(toolchain.id), process.platform === 'win32' ? 'g++.exe' : 'g++');
    if (existsSync(gxx)) return toolchain.id;
  }
  return null;
}

const compiler = findGxx();
if (compiler === null) skip('no host g++ — the native Unity suites were not run');

console.log(`LH_ENV toolchain.platformio=${platformioVersion(pio)}`);
console.log(`LH_ENV toolchain.unity_native=${compiler}`);

let totalCases = 0;
let succeeded = 0;
let failed = 0;

for (const project of PROJECTS) {
  let output = '';
  let ok = true;
  try {
    output = execFileSync(pio, ['test', '-d', project.dir, '-e', project.env], {
      encoding: 'utf8',
      stdio: 'pipe',
      env: environmentWithCompiler(compiler),
      maxBuffer: 64 * 1024 * 1024,
    });
  } catch (error) {
    output = `${error?.stdout ?? ''}${error?.stderr ?? ''}`;
    ok = false;
  }

  // `================= 22 test cases: 22 succeeded in 00:00:14.304 =================`
  const summary = output.match(/(\d+)\s+test cases?:\s+(\d+)\s+succeeded/);
  if (summary === null) {
    console.error(`FAIL     ${project.id} — no test summary in PlatformIO's output`);
    console.error(output.split('\n').slice(-25).join('\n'));
    failed++;
    continue;
  }

  const cases = Number(summary[1]);
  const passed = Number(summary[2]);
  totalCases += cases;
  succeeded += passed;

  // Per-suite lines, so a failure names itself rather than hiding in a total.
  for (const line of output.split('\n')) {
    const suite = line.match(/^-+\s*\S+:(\S+)\s+\[(PASSED|FAILED)\]/);
    if (suite !== null) console.log(`  ${suite[2] === 'PASSED' ? 'ok  ' : 'FAIL'} ${suite[1]}`);
  }

  if (!ok || passed !== cases) {
    console.error(`FAIL     ${project.id} — ${cases - passed} of ${cases} cases failed`);
    failed++;
  } else {
    console.log(`OK       ${project.id.padEnd(8)} — ${passed}/${cases} Unity cases`);
  }
}

console.log(`LH_METRIC test.unity.native.cases value=${totalCases} unit=count`);
console.log(
  `LH_METRIC test.unity.native.succeeded value=${succeeded} unit=count budget=${totalCases}`,
);
console.log(`LH_METRIC test.unity.native.failed value=${totalCases - succeeded} unit=count budget=0`);

if (failed > 0) {
  console.error(`\n${failed} native Unity project(s) failed.`);
  process.exit(1);
}
