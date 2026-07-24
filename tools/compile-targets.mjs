/**
 * Compiles the generated protocol header on every C toolchain we can find.
 *
 * The roadmap wants three ABIs (host x86-64, xtensa-esp32, riscv32-esp32c3)
 * because padding and alignment rules differ between them and a `packed` struct
 * that is 8 bytes on one target is not guaranteed to be 8 bytes on the next.
 * Every _Static_assert in the generated header is inert until a compiler reads
 * it, so this script is what turns them into an actual gate.
 *
 * Toolchains are discovered, never assumed: a target with no compiler installed
 * is reported as SKIPPED, not as a pass. The summary prints how many of the
 * three were genuinely verified, so a partial local run cannot be mistaken for
 * full coverage.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));
const INCLUDE_DIR = join(REPO_ROOT, 'firmware', 'common', 'include');
const SOURCE = join(REPO_ROOT, 'firmware', 'common', 'test', 'test_generated_header.c');
const HOME_DIR = process.env.USERPROFILE ?? process.env.HOME ?? '';
const PIO_PACKAGES = join(HOME_DIR, '.platformio', 'packages');

/**
 * Candidate locations for a PlatformIO-installed cross compiler, covering both
 * the Windows (.exe) and Linux/macOS layouts so the same script gates a
 * developer laptop and a CI runner.
 */
function pioCandidates(packageName, binaryName) {
  return [
    join(PIO_PACKAGES, packageName, 'bin', `${binaryName}.exe`),
    join(PIO_PACKAGES, packageName, 'bin', binaryName),
    binaryName, // already on PATH (e.g. an ESP-IDF export.sh environment)
  ];
}

/** Bare names are resolved from PATH; anything with a separator must exist on disk. */
const TARGETS = [
  {
    id: 'x86-64',
    description: 'host gcc/clang',
    candidates: ['gcc', 'cc', 'clang'],
    extraFlags: [],
  },
  {
    id: 'xtensa',
    description: 'xtensa-esp32s3-elf-gcc (ESP32-S3)',
    candidates: pioCandidates('toolchain-xtensa-esp32s3', 'xtensa-esp32s3-elf-gcc'),
    extraFlags: [],
  },
  {
    id: 'riscv32',
    description: 'riscv32-esp-elf-gcc (ESP32-C3)',
    candidates: pioCandidates('toolchain-riscv32-esp', 'riscv32-esp-elf-gcc'),
    extraFlags: [],
  },
];

const COMMON_FLAGS = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-c'];

function resolveCompiler(candidates) {
  for (const candidate of candidates) {
    if (candidate.includes('/') || candidate.includes('\\')) {
      if (existsSync(candidate)) return candidate;
      continue;
    }
    try {
      execFileSync(candidate, ['--version'], { stdio: 'ignore' });
      return candidate;
    } catch {
      /* not on PATH */
    }
  }
  return null;
}

const workDir = mkdtempSync(join(tmpdir(), 'lh-compile-'));
let compiled = 0;
let skipped = 0;
let failed = 0;

try {
  for (const target of TARGETS) {
    const compiler = resolveCompiler(target.candidates);
    if (compiler === null) {
      console.log(`SKIPPED  ${target.id.padEnd(8)} — no compiler found (${target.description})`);
      skipped++;
      continue;
    }

    const objectFile = join(workDir, `${target.id}.o`);
    try {
      execFileSync(
        compiler,
        [...COMMON_FLAGS, ...target.extraFlags, '-I', INCLUDE_DIR, SOURCE, '-o', objectFile],
        { stdio: 'pipe' },
      );
      console.log(`OK       ${target.id.padEnd(8)} — ${target.description}`);
      compiled++;
    } catch (error) {
      const detail = error instanceof Error && 'stderr' in error ? String(error.stderr) : String(error);
      console.error(`FAIL     ${target.id.padEnd(8)} — ${target.description}\n${detail}`);
      failed++;
    }
  }
} finally {
  rmSync(workDir, { recursive: true, force: true });
}

console.log(`LH_METRIC targets.compiled_clean value=${compiled}/${TARGETS.length} unit=targets`);
console.log(`LH_METRIC targets.skipped value=${skipped} unit=targets`);

if (failed > 0) {
  console.error(`\n${failed} target(s) failed to compile the generated header.`);
  process.exit(1);
}
if (compiled === 0) {
  console.error('\nNo C toolchain available — the static assertions were not verified at all.');
  process.exit(1);
}
if (skipped > 0) {
  console.log(`\nNote: ${skipped} target(s) skipped for want of a toolchain; not verified here.`);
}
