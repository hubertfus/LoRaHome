/**
 * Builds and runs the firmware's host-native self-test harnesses.
 *
 * The cross compilers prove the firmware *compiles* for the ESP targets. This
 * runs it. Everything that needs observed behaviour rather than acceptance —
 * a million fuzz iterations, a microbenchmark, a bounds guard — lives behind
 * this script, and it is the only place in the repo that executes firmware C.
 *
 * Each harness is built twice where the toolchain allows it:
 *
 *   1. -O2, no instrumentation. This is the build the benchmark numbers come
 *      from; a sanitized binary is several times slower and its timings would
 *      describe the sanitizer, not the code.
 *   2. ASAN + UBSAN. This is the build the correctness claim comes from. The
 *      benchmark section still runs but its numbers are discarded.
 *
 * With no host toolchain at all the script exits 0 and says so. That is not
 * leniency: it prints SKIPPED, contributes no metrics, and the absence is
 * visible in the collector's source list. Silence would be the problem; a
 * fabricated zero would be worse.
 */
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  exeName,
  findHostToolchains,
  findSanitizingToolchain,
  spawnFailureDetail,
} from './host-cc.mjs';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));
const COMMON = join(REPO_ROOT, 'firmware', 'common');
const INCLUDE_DIR = join(COMMON, 'include');

/** Harnesses to build and run. Each is a `main()` that returns non-zero on failure. */
const HARNESSES = [
  {
    id: 'slip',
    sources: [join(COMMON, 'test', 'slip_selftest.c'), join(COMMON, 'src', 'slip.c')],
  },
  {
    id: 'ring',
    sources: [join(COMMON, 'test', 'ring_selftest.c'), join(COMMON, 'src', 'ring.c')],
  },
  {
    id: 'frame',
    sources: [
      join(COMMON, 'test', 'frame_selftest.c'),
      join(COMMON, 'src', 'protocol.c'),
      join(COMMON, 'src', 'crc16.c'),
    ],
  },
  {
    id: 'dedup',
    sources: [join(COMMON, 'test', 'dedup_selftest.c'), join(COMMON, 'src', 'dedup.c')],
  },
  {
    id: 'arq',
    sources: [join(COMMON, 'test', 'arq_selftest.c'), join(COMMON, 'src', 'arq.c')],
  },
  {
    id: 'frag',
    sources: [
      join(COMMON, 'test', 'frag_selftest.c'),
      join(COMMON, 'src', 'frag.c'),
      join(COMMON, 'src', 'crc16.c'),
    ],
  },
  {
    id: 'bridge',
    sources: [
      join(COMMON, 'test', 'bridge_selftest.c'),
      join(COMMON, 'src', 'bridge_core.c'),
      join(COMMON, 'src', 'bridge_stat.c'),
      join(COMMON, 'src', 'slip.c'),
      join(COMMON, 'src', 'protocol.c'),
      join(COMMON, 'src', 'crc16.c'),
    ],
  },
];

const toolchains = findHostToolchains();
const toolchain = toolchains[0] ?? null;

if (toolchain === null) {
  console.log('SKIPPED  no host C toolchain (gcc, clang or MSVC) — native harnesses not run');
  console.log('LH_METRIC native.harnesses_run value=0 unit=count');
  process.exit(0);
}

/**
 * The sanitized pass may use a different compiler from the timed one.
 *
 * On Windows with MinGW installed, the preferred toolchain has the GNU warning
 * set the firmware is written against but no libasan, while MSVC has ASAN.
 * Taking the best of each keeps both guarantees; insisting on one compiler for
 * both would quietly drop whichever it could not provide.
 */
const sanitizer = findSanitizingToolchain(toolchains);

console.log(`LH_ENV toolchain.native=${toolchain.version}`);
console.log(`LH_ENV native.sanitizers=${sanitizer === null ? 'none' : sanitizer.sanitizers}`);
if (sanitizer !== null && sanitizer !== toolchain) {
  console.log(`LH_ENV toolchain.native_sanitized=${sanitizer.version}`);
}

const workDir = mkdtempSync(join(tmpdir(), 'lh-native-'));
let failures = 0;
let harnessesRun = 0;
let sanitizedRuns = 0;

/**
 * Builds and runs one variant.
 *
 * Only the plain build's stdout is forwarded to the collector. Letting both
 * runs emit LH_METRIC lines would mean the sanitized timings silently
 * overwrite the real ones — last line wins in the parser — and the metric
 * series would record whichever build happened to run second.
 */
function runVariant(harness, { sanitize, forwardMetrics }) {
  const cc = sanitize ? sanitizer : toolchain;
  const label = sanitize ? cc.sanitizers : 'plain';

  for (let attempt = 1; ; attempt++) {
    // A fresh output path per attempt — see buildAndRun's note on why retrying
    // the same file is not enough.
    const suffix = attempt === 1 ? '' : `-r${attempt}`;
    const output = join(workDir, exeName(`${harness.id}-${sanitize ? 'san' : 'plain'}${suffix}`));

    try {
      cc.compile({
        sources: harness.sources,
        includeDirs: [INCLUDE_DIR],
        output,
        optimize: sanitize ? 'O0' : 'O2',
        sanitize,
        defines: sanitize ? ['LH_SANITIZED=1'] : [],
      });
    } catch (error) {
      console.error(
        `FAIL     ${harness.id} (${label}) — build failed\n${spawnFailureDetail(error)}`,
      );
      return false;
    }

    let stdout;
    try {
      stdout = execFileSync(output, [], {
        encoding: 'utf8',
        stdio: 'pipe',
        env: cc.env,
        maxBuffer: 32 * 1024 * 1024,
      });
    } catch (error) {
      if (couldNotStart(error) && attempt < LAUNCH_ATTEMPTS) {
        sleepMs(150 * attempt);
        continue;
      }
      // A sanitizer report arrives on stderr with a non-zero exit, and so does
      // a failed assertion. Both are the harness talking; print all of it.
      const reason = couldNotStart(error) ? ` — could not be started in ${attempt} attempts` : '';
      console.error(`FAIL     ${harness.id} (${label})${reason}\n${spawnFailureDetail(error)}`);
      return false;
    }

    if (attempt > 1) console.log(`note     ${harness.id} (${label}) ran on attempt ${attempt}`);
    if (forwardMetrics) process.stdout.write(stdout);
    else console.log(`OK       ${harness.id} (${label}) — ${countChecks(stdout)}`);
    return true;
  }
}

/**
 * How many times to rebuild-and-relaunch before calling it a failure.
 *
 * On this Windows machine, launching a just-linked unsigned binary fails with
 * `spawnSync ... UNKNOWN` for roughly one run in three. Relaunching the *same*
 * file does not help — measured: eight tries over nine seconds, all refused —
 * so whatever holds it is not a scan that finishes. Rebuilding to a new path
 * does help, which is why the retry wraps the compile as well.
 *
 * Six rather than four: across a 12-run sample every run eventually launched,
 * but one needed a fourth attempt, and a limit sitting exactly on the observed
 * worst case is a limit that fails next week. On Linux the first attempt has
 * always succeeded, so this costs CI nothing.
 *
 * The distinction that keeps this honest: only a failure to *start* is retried.
 * A process that ran and exited non-zero has produced a result, and re-rolling
 * it until it turns green is exactly the habit a metric gate exists to prevent.
 */
const LAUNCH_ATTEMPTS = 6;

/** Errors that mean "the binary did not launch", never "the test failed". */
const SPAWN_LEVEL_CODES = new Set(['UNKNOWN', 'ETXTBSY', 'EBUSY', 'EPERM', 'EACCES', 'EAGAIN']);

function couldNotStart(error) {
  return (
    error?.status === null || error?.status === undefined || SPAWN_LEVEL_CODES.has(error?.code)
  );
}

/** Pulls the harness's own summary line out for the one-line OK message. */
function countChecks(stdout) {
  return stdout.trim().split('\n').at(-1)?.trim() ?? 'no summary';
}

/** Blocking sleep; this script is synchronous throughout and has nothing else to do. */
function sleepMs(ms) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

try {
  for (const harness of HARNESSES) {
    // Plain build first: it produces the metrics, and if the code is simply
    // broken there is no point paying for a sanitized run to say so again.
    if (!runVariant(harness, { sanitize: false, forwardMetrics: true })) {
      failures++;
      continue;
    }
    harnessesRun++;

    if (sanitizer === null) {
      console.log(`SKIPPED  ${harness.id} (sanitized) — no toolchain here provides one`);
      continue;
    }
    if (runVariant(harness, { sanitize: true, forwardMetrics: false })) sanitizedRuns++;
    else failures++;
  }
} finally {
  rmSync(workDir, { recursive: true, force: true });
}

console.log(`LH_METRIC native.harnesses_run value=${harnessesRun} unit=count`);
console.log(`LH_METRIC native.sanitized_runs value=${sanitizedRuns} unit=count`);

if (failures > 0) {
  console.error(`\n${failures} native harness run(s) failed.`);
  process.exit(1);
}
