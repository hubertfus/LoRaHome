/**
 * Finds a C toolchain that can build *and run* code on this machine.
 *
 * The cross compilers in tools/compile-targets.mjs prove the firmware sources
 * compile for xtensa and riscv32, but they cannot execute a single instruction
 * here. Anything that has to observe behaviour rather than just acceptance —
 * a fuzz run, a native microbenchmark, a cross-language vector comparison —
 * needs a host toolchain, and the only ones this repo knew how to find were
 * gcc and clang. On a Windows developer machine that meant those checks did not
 * run at all locally; everything was taken on trust until CI got to it.
 *
 * So MSVC is accepted here as well. Two deliberate limits on that:
 *
 *   - It is not used for the three-ABI header check. MSVC has no
 *     `__attribute__((packed))` — it spells that `#pragma pack` — so the
 *     generated header does not compile under it today. That is a real
 *     portability gap, but it belongs to the T0.3 generator, not here.
 *   - It contributes no size metrics. Section sizes are a property of the
 *     compiler as much as of the code, and COFF objects are not comparable to
 *     ELF ones; mixing them into one metric series would make the baseline
 *     meaningless. compile-targets.mjs keeps that job.
 *
 * What it does give us is a native x86-64 binary we can actually run, which is
 * the whole point.
 */
import { execFileSync, execSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { delimiter, dirname, join } from 'node:path';

/** Optimisation level, expressed once and translated per compiler. */
const OPT_FLAGS = {
  gnu: { O0: '-O0', Os: '-Os', O2: '-O2' },
  msvc: { O0: '/Od', Os: '/O1', O2: '/O2' },
};

const WARNING_FLAGS = {
  gnu: ['-std=c11', '-Wall', '-Wextra', '-Werror'],
  // /W4 is MSVC's rough equivalent of -Wall -Wextra; /WX is -Werror.
  msvc: ['/nologo', '/std:c11', '/W4', '/WX'],
};

// ---------------------------------------------------------------------------
// GNU-style (gcc, clang, cc)
// ---------------------------------------------------------------------------

function firstVersionLine(binary) {
  try {
    const out = execFileSync(binary, ['--version'], { encoding: 'utf8', stdio: 'pipe' });
    return out.split('\n')[0]?.trim() ?? binary;
  } catch {
    return null;
  }
}

function gnuToolchain(binary, version) {
  return {
    id: binary,
    kind: 'gnu',
    version,
    /** ASAN and UBSAN both ship with GCC and Clang on the platforms CI runs on. */
    supportsSanitizer: true,
    sanitizers: 'asan+ubsan',
    env: process.env,
    compile({ sources, includeDirs = [], output, optimize = 'O2', sanitize = false, defines = [] }) {
      execFileSync(
        binary,
        [
          ...WARNING_FLAGS.gnu,
          OPT_FLAGS.gnu[optimize],
          ...(sanitize ? ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g'] : []),
          ...defines.map((d) => `-D${d}`),
          ...includeDirs.flatMap((dir) => ['-I', dir]),
          ...sources,
          '-o',
          output,
        ],
        { stdio: 'pipe', encoding: 'utf8' },
      );
    },
  };
}

// ---------------------------------------------------------------------------
// MSVC
// ---------------------------------------------------------------------------

/**
 * The environment cl.exe needs (INCLUDE, LIB, PATH), captured from vcvars64.bat.
 *
 * Captured once and reused rather than wrapping every compile in
 * `cmd /c "call vcvars && cl ..."`: that form re-runs the batch file on every
 * invocation, which is about a second each and would dominate a harness that
 * builds several binaries.
 */
function msvcEnvironment(vcvarsPath) {
  // execSync, not execFileSync: this is a cmd.exe command line, and cmd's own
  // quote handling needs the `cmd /d /s /c "..."` form that execSync builds.
  // The same string through execFileSync('cmd.exe', ['/c', ...]) gets re-quoted
  // by Node and cmd then fails to find the batch file — observed, not assumed.
  const output = execSync(`call "${vcvarsPath}" >nul 2>&1 && set`, {
    encoding: 'utf8',
    stdio: 'pipe',
  });

  const env = {};
  for (const line of output.split(/\r?\n/)) {
    const eq = line.indexOf('=');
    if (eq > 0) env[line.slice(0, eq)] = line.slice(eq + 1);
  }
  return env['INCLUDE'] === undefined ? null : env;
}

function findVcvars() {
  const programFilesX86 = process.env['ProgramFiles(x86)'];
  if (programFilesX86 === undefined) return null;

  const vswhere = join(programFilesX86, 'Microsoft Visual Studio', 'Installer', 'vswhere.exe');
  if (!existsSync(vswhere)) return null;

  let roots;
  try {
    roots = execFileSync(
      vswhere,
      [
        '-latest',
        '-products',
        '*',
        '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        '-property',
        'installationPath',
      ],
      { encoding: 'utf8', stdio: 'pipe' },
    )
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter((line) => line !== '');
  } catch {
    return null;
  }

  for (const root of roots) {
    const vcvars = join(root, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat');
    if (existsSync(vcvars)) return vcvars;
  }
  return null;
}

/**
 * Whether the ASAN runtime DLL is resolvable.
 *
 * /fsanitize=address compiles happily without it and then the binary fails to
 * start. Reporting a fuzz run as "1M iterations, 0 ASAN reports" when the
 * process never launched would be the worst possible outcome for a metric, so
 * sanitizer support is claimed only when the DLL is actually on the path.
 */
function hasAsanRuntime(env) {
  const path = env['PATH'] ?? env['Path'] ?? '';
  return path
    .split(delimiter)
    .some((dir) => dir !== '' && existsSync(join(dir, 'clang_rt.asan_dynamic-x86_64.dll')));
}

function msvcToolchain() {
  if (process.platform !== 'win32') return null;

  const vcvars = findVcvars();
  if (vcvars === null) return null;

  let env;
  try {
    env = msvcEnvironment(vcvars);
  } catch {
    return null;
  }
  if (env === null) return null;

  const asan = hasAsanRuntime(env);

  return {
    id: 'cl.exe',
    kind: 'msvc',
    // From the environment, not from a banner. cl.exe has no --version, and
    // invoked bare under a vcvars environment it prints only a usage line —
    // localised at that, so scraping it would be doubly fragile. vcvars exports
    // the toolset version, which is precisely the number that matters for
    // reproducing a measurement.
    version: `MSVC ${env['VCToolsVersion'] ?? 'unknown'} (${env['VSCMD_ARG_TGT_ARCH'] ?? '?'})`,
    supportsSanitizer: asan,
    // MSVC has ASAN but no UBSAN, so the sanitized run here is strictly weaker
    // than the GCC one on CI. Named rather than implied, so a clean local run
    // is not mistaken for the full check.
    sanitizers: asan ? 'asan' : 'none',
    env,
    compile({ sources, includeDirs = [], output, optimize = 'O2', sanitize = false, defines = [] }) {
      // Object files land next to the executable rather than in the caller's
      // working directory, which is where cl.exe puts them if you let it.
      const objDir = join(dirname(output), 'obj');
      mkdirSync(objDir, { recursive: true });

      execFileSync(
        'cl.exe',
        [
          ...WARNING_FLAGS.msvc,
          OPT_FLAGS.msvc[optimize],
          // /Fd alongside /Zi: without it cl.exe drops a vc140.pdb into the
          // current working directory, which here is the repository root.
          ...(sanitize ? ['/fsanitize=address', '/Zi', `/Fd${join(objDir, 'debug.pdb')}`] : []),
          ...defines.map((d) => `/D${d}`),
          ...includeDirs.map((dir) => `/I${dir}`),
          ...sources,
          `/Fo${objDir}\\`,
          `/Fe${output}`,
        ],
        { env, stdio: 'pipe', encoding: 'utf8' },
      );
    },
  };
}

// ---------------------------------------------------------------------------

/**
 * Resolves the host toolchain, preferring GNU.
 *
 * GNU first because CI runs on it: a developer disagreeing with CI over a
 * warning is a conversation better had locally than at review time. MSVC is the
 * fallback that makes a Windows machine able to run these tests at all.
 */
export function findHostToolchain() {
  for (const binary of ['cc', 'gcc', 'clang']) {
    const version = firstVersionLine(binary);
    if (version !== null) return gnuToolchain(binary, version);
  }
  return msvcToolchain();
}

/** Platform-appropriate name for a built executable. */
export function exeName(stem) {
  return process.platform === 'win32' ? `${stem}.exe` : stem;
}

/** Combined stdout+stderr of a failed spawn: GCC diagnoses on stderr, cl.exe on stdout. */
export function spawnFailureDetail(error) {
  const streams = `${error?.stdout ?? ''}${error?.stderr ?? ''}`.trim();
  return streams === '' ? String(error) : streams;
}
