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
import { copyFileSync, existsSync, mkdtempSync, readdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { exeName, findHostToolchains } from './host-cc.mjs';
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

/**
 * Distinguishes "the OS refused to start this" from "this test failed".
 *
 * On this Windows machine, Application Control refuses to execute a just-linked
 * unsigned binary for roughly one suite in five — `[WinError 4551] Zasady
 * kontroli aplikacji zablokowały ten plik`. PlatformIO reports the suite as
 * ERRORED, its cases vanish from the total, and the run fails having tested
 * nothing. run-native.mjs documents the same phenomenon under a different
 * message (`spawnSync ... UNKNOWN`).
 *
 * The distinction is the whole safety property here: a suite that ran and
 * failed an assertion has produced a result, and re-rolling it until it turns
 * green is exactly the habit a metric gate exists to prevent. Only a refused
 * launch is worked around, and only by running the binary that was already
 * built — never by rebuilding or re-deciding a test's outcome.
 */
function wasBlockedFromLaunching(output) {
  // WinError 4551 is Application Control. The ERRORED-without-FAILED shape
  // catches the same class of problem under a different message: PlatformIO
  // prints [FAILED] for an assertion and [ERRORED] when the suite never ran.
  if (/WinError 4551/.test(output)) return true;
  return /\[ERRORED\]/.test(output) && !/\[FAILED\]/.test(output);
}

/** Suite directories, so each one can be run — and if blocked, retried — alone. */
function suitesOf(project) {
  return readdirSync(join(project.dir, 'test'), { withFileTypes: true })
    .filter((entry) => entry.isDirectory() && entry.name.startsWith('test_'))
    .map((entry) => entry.name)
    .sort();
}

/**
 * Runs the suite's binary from a fresh path, when the OS refused PlatformIO's.
 *
 * PlatformIO links every suite to the same `program.exe`, and re-running it
 * there gets refused again — run-native.mjs found the same thing from the other
 * direction and worked around it the same way: a copy at a new path is
 * evaluated fresh and starts. The build has already succeeded at this point;
 * only the launch was blocked, so nothing is rebuilt and nothing is skipped.
 *
 * The binary is Unity's own, so its output is parsed directly rather than
 * through PlatformIO's summary. Returns null if there is nothing to copy.
 */
function runBuiltBinaryDirectly(project, suite) {
  const binary = join(project.dir, '.pio', 'build', project.env, exeName('program'));
  if (!existsSync(binary)) return null;

  const copy = join(
    mkdtempSync(join(tmpdir(), 'lh-unity-')),
    exeName(`${suite}-${Date.now().toString(36)}`),
  );
  copyFileSync(binary, copy);

  let output = '';
  let ok = true;
  try {
    output = execFileSync(copy, [], { encoding: 'utf8', stdio: 'pipe', maxBuffer: 32 * 1024 * 1024 });
  } catch (error) {
    output = `${error?.stdout ?? ''}${error?.stderr ?? ''}`;
    ok = false;
  } finally {
    rmSync(dirname(copy), { recursive: true, force: true });
  }

  // Unity's own summary: `6 Tests 0 Failures 0 Ignored`.
  const summary = output.match(/(\d+)\s+Tests\s+(\d+)\s+Failures\s+(\d+)\s+Ignored/);
  if (summary === null) return null;

  const cases = Number(summary[1]);
  const failures = Number(summary[2]);
  return { ok: ok && failures === 0, cases, passed: cases - failures, output };
}

/**
 * Runs one suite, working around only a launch the OS refused.
 *
 * Per suite rather than per project: with seven suites and roughly a one-in-five
 * block rate, re-running the whole project rarely gets all seven through at
 * once. It also keeps the workaround narrow — suites that already ran are not
 * re-run, so a passing result can never come from a second roll of the dice.
 */
function runSuite(project, suite) {
  let output = '';
  let ok = true;
  try {
    output = execFileSync(pio, ['test', '-d', project.dir, '-e', project.env, '-f', suite], {
      encoding: 'utf8',
      stdio: 'pipe',
      env: environmentWithCompiler(compiler),
      maxBuffer: 64 * 1024 * 1024,
    });
  } catch (error) {
    output = `${error?.stdout ?? ''}${error?.stderr ?? ''}`;
    ok = false;
  }

  if (ok || !wasBlockedFromLaunching(output)) return { ok, output, viaCopy: false };

  console.log(`note     ${suite} was blocked from launching by the OS; running a fresh copy`);
  const direct = runBuiltBinaryDirectly(project, suite);
  if (direct === null) return { ok: false, output, blocked: true, viaCopy: false };

  return { ok: direct.ok, output: direct.output, viaCopy: true, direct };
}

for (const project of PROJECTS) {
  for (const suite of suitesOf(project)) {
    const { ok, output, blocked, viaCopy, direct } = runSuite(project, suite);

    if (blocked === true) {
      console.error(
        `FAIL     ${suite} — blocked from launching, and there was no built binary to ` +
          `copy. That is this machine's Application Control, not the code — but it means ` +
          `the suite was not verified, so it is not being called green.`,
      );
      failed++;
      continue;
    }

    let cases;
    let passed;
    if (viaCopy) {
      cases = direct.cases;
      passed = direct.passed;
    } else {
      // `================= 6 test cases: 6 succeeded in 00:00:02.133 =================`
      const summary = output.match(/(\d+)\s+test cases?:\s+(\d+)\s+succeeded/);
      if (summary === null) {
        console.error(`FAIL     ${suite} — no test summary in PlatformIO's output`);
        console.error(output.split('\n').slice(-25).join('\n'));
        failed++;
        continue;
      }
      cases = Number(summary[1]);
      passed = Number(summary[2]);
    }

    totalCases += cases;
    succeeded += passed;

    const note = viaCopy ? ' (ran from a copy)' : '';
    if (!ok || passed !== cases) {
      console.error(`FAIL     ${suite} — ${cases - passed} of ${cases} cases failed${note}`);
      // The failing assertions, so a red run says what broke rather than only
      // how many things did.
      for (const line of output.split('\n')) {
        if (/:\d+:.*\[(FAILED|IGNORED)\]/.test(line)) console.error(`         ${line.trim()}`);
      }
      failed++;
    } else {
      console.log(`  ok   ${suite.padEnd(20)} ${passed}/${cases} cases${note}`);
    }
  }
}

if (failed === 0) {
  console.log(`OK       node     — ${succeeded}/${totalCases} Unity cases`);
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
