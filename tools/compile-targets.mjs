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

/**
 * Translation units compiled on every available target.
 *
 * `measureText` marks the ones whose .text size is a tracked budget. The
 * generated-header test unit is compile-only — it exists to run assertions, and
 * its size means nothing.
 */
const UNITS = [
  {
    id: 'generated_header',
    path: join(REPO_ROOT, 'firmware', 'common', 'test', 'test_generated_header.c'),
    measureText: false,
    extraFlags: [],
  },
  {
    id: 'mem_probe',
    path: join(REPO_ROOT, 'firmware', 'common', 'src', 'mem_probe.c'),
    measureText: true,
    textBudget: 512,
    // -Os because that is how the firmware ships; measuring .text at -O0 would
    // report a number no released build ever has.
    extraFlags: ['-Os'],
  },
  {
    id: 'crc16',
    path: join(REPO_ROOT, 'firmware', 'common', 'src', 'crc16.c'),
    measureText: true,
    // -ffunction-sections/-fdata-sections so .text and .rodata are attributed
    // per symbol; without them the nibble table's 32 B is invisible in the
    // section totals, and that 32 B is the whole point of the design choice.
    extraFlags: ['-Os', '-ffunction-sections', '-fdata-sections'],
  },
];
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

/** Locates the matching `size` binary alongside a given compiler. */
function sizeToolFor(compiler) {
  const candidate = compiler.replace(/gcc(\.exe)?$/, (match) => match.replace('gcc', 'size'));
  if (candidate === compiler) return null;
  if (candidate.includes('/') || candidate.includes('\\')) {
    return existsSync(candidate) ? candidate : null;
  }
  try {
    execFileSync(candidate, ['--version'], { stdio: 'ignore' });
    return candidate;
  } catch {
    return null;
  }
}

/**
 * Per-section sizes via `size -A` (SysV format).
 *
 * Berkeley format folds .rodata into the `text` column, and the roadmap budgets
 * .text and .rodata separately — the nibble-table decision in T0.5 is precisely
 * a trade of one against the other, so they have to be visible apart.
 */
function sectionSizes(sizeTool, objectFile) {
  const output = execFileSync(sizeTool, ['-A', objectFile], { encoding: 'utf8' });
  const sections = new Map();
  for (const line of output.split('\n')) {
    const match = line.trim().match(/^(\.[\w.]+)\s+(\d+)/);
    if (match) sections.set(match[1], Number(match[2]));
  }
  return sections;
}

/** Sums a section and its per-function/per-object split (.text.foo, .rodata.bar). */
function sectionTotal(sections, prefix) {
  let total = 0;
  for (const [name, size] of sections) {
    if (name === prefix || name.startsWith(`${prefix}.`)) total += size;
  }
  return total;
}

const workDir = mkdtempSync(join(tmpdir(), 'lh-compile-'));
let compiled = 0;
let skipped = 0;
let failed = 0;
const textSizes = [];

try {
  for (const target of TARGETS) {
    const compiler = resolveCompiler(target.candidates);
    if (compiler === null) {
      console.log(`SKIPPED  ${target.id.padEnd(8)} — no compiler found (${target.description})`);
      skipped++;
      continue;
    }

    let targetOk = true;
    for (const unit of UNITS) {
      const objectFile = join(workDir, `${target.id}-${unit.id}.o`);
      try {
        execFileSync(
          compiler,
          [
            ...COMMON_FLAGS,
            ...target.extraFlags,
            ...unit.extraFlags,
            '-I',
            INCLUDE_DIR,
            unit.path,
            '-o',
            objectFile,
          ],
          { stdio: 'pipe' },
        );

        if (unit.measureText) {
          const sizeTool = sizeToolFor(compiler);
          if (sizeTool !== null) {
            const sections = sectionSizes(sizeTool, objectFile);
            textSizes.push({
              target: target.id,
              unit: unit.id,
              bytes: sectionTotal(sections, '.text'),
              rodata: sectionTotal(sections, '.rodata'),
              budget: unit.textBudget,
            });
          }
        }
      } catch (error) {
        const detail =
          error instanceof Error && 'stderr' in error ? String(error.stderr) : String(error);
        console.error(`FAIL     ${target.id.padEnd(8)} ${unit.id} — ${target.description}\n${detail}`);
        targetOk = false;
      }
    }

    if (targetOk) {
      console.log(`OK       ${target.id.padEnd(8)} — ${target.description} (${UNITS.length} units)`);
      compiled++;
    } else {
      failed++;
    }
  }
} finally {
  rmSync(workDir, { recursive: true, force: true });
}

for (const entry of textSizes) {
  const budget = entry.budget === undefined ? '' : ` budget=${entry.budget}`;
  console.log(
    `LH_METRIC size.${entry.unit}.text.${entry.target} value=${entry.bytes} unit=B${budget}`,
  );
  console.log(`LH_METRIC size.${entry.unit}.rodata.${entry.target} value=${entry.rodata} unit=B`);
}

const oversized = textSizes.filter((e) => e.budget !== undefined && e.bytes > e.budget);
for (const entry of oversized) {
  console.error(
    `BUDGET BREACH: size.${entry.unit}.text.${entry.target} = ${entry.bytes} B (budget: ${entry.budget} B)`,
  );
}

console.log(`LH_METRIC targets.compiled_clean value=${compiled}/${TARGETS.length} unit=targets`);
console.log(`LH_METRIC targets.skipped value=${skipped} unit=targets`);

if (failed > 0 || oversized.length > 0) {
  if (failed > 0) console.error(`\n${failed} target(s) failed to compile.`);
  process.exit(1);
}
if (compiled === 0) {
  console.error('\nNo C toolchain available — the static assertions were not verified at all.');
  process.exit(1);
}
if (skipped > 0) {
  console.log(`\nNote: ${skipped} target(s) skipped for want of a toolchain; not verified here.`);
}
