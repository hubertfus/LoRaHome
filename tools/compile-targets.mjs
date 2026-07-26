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
import { delimiter, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { windowsGnuCandidates } from './host-cc.mjs';

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
  {
    id: 'ring',
    path: join(REPO_ROOT, 'firmware', 'common', 'src', 'ring.c'),
    measureText: true,
    textBudget: 1024, // T1.2
    // Also where the ring's arithmetic invariants are enforced: ring.c asserts
    // that the size is a power of two AND divides the uint16_t counter range,
    // which is the difference between a ring that wraps cleanly and one that
    // corrupts a bufferful every 65536 bytes.
    extraFlags: ['-Os', '-ffunction-sections', '-fdata-sections'],
  },
  {
    id: 'bridge_core',
    path: join(REPO_ROOT, 'firmware', 'common', 'src', 'bridge_core.c'),
    measureText: true,
    textBudget: 2048, // T1.4
    // Carries the _Static_assert on sizeof(lh_bridge_ctx_t) — the Etap 1 RAM
    // budget for the whole forwarding context.
    extraFlags: ['-Os', '-ffunction-sections', '-fdata-sections'],
  },
  {
    id: 'arq',
    path: join(REPO_ROOT, 'firmware', 'common', 'src', 'arq.c'),
    measureText: true,
    textBudget: 1024, // T2.4
    extraFlags: ['-Os', '-ffunction-sections', '-fdata-sections'],
  },
  {
    // Compile-only: exists so the reliability layer's 2304 B budget is checked
    // on every ABI. Its .text is the stub that keeps the instance alive and
    // means nothing.
    id: 'reliability_ctx',
    path: join(REPO_ROOT, 'firmware', 'common', 'test', 'test_reliability_ctx.c'),
    measureText: false,
    extraFlags: ['-Os'],
  },
  {
    id: 'frag',
    path: join(REPO_ROOT, 'firmware', 'common', 'src', 'frag.c'),
    measureText: true,
    // 1280 rather than the 1024 this shipped with. See the chore(budget) commit:
    // riscv32 came in at 1018 B on the CI toolchain — six bytes of headroom,
    // which is a budget that fails on the next comment-sized change rather than
    // on a real regression.
    textBudget: 1280, // T2.3
    // Carries the _Static_assert on sizeof(lh_reassembler_t) — 1664 B of the
    // 2304 B reliability budget, and the one struct big enough that a target's
    // alignment rules could push it over on their own.
    extraFlags: ['-Os', '-ffunction-sections', '-fdata-sections'],
  },
  {
    id: 'protocol',
    path: join(REPO_ROOT, 'firmware', 'common', 'src', 'protocol.c'),
    measureText: true,
    textBudget: 1024, // T2.2 — the whole build/parse path, CRC excluded
    // Carries the _Static_asserts tying LORAHOME_MAX_FRAME_SIZE to the 230 B
    // MTU. The frame path is the one contract both languages and all three ABIs
    // have to agree on byte for byte.
    extraFlags: ['-Os', '-ffunction-sections', '-fdata-sections'],
  },
  {
    id: 'dedup',
    path: join(REPO_ROOT, 'firmware', 'common', 'src', 'dedup.c'),
    measureText: true,
    textBudget: 512, // T2.1
    // Carries the _Static_assert on sizeof(lh_dedup_t). The peer record holds an
    // int64_t next to a uint16_t, so its size is entirely a question of the
    // target's alignment rules — exactly the drift the three-ABI sweep exists
    // to catch, and not something the host build can answer on its own.
    extraFlags: ['-Os', '-ffunction-sections', '-fdata-sections'],
  },
  {
    id: 'driver',
    path: join(REPO_ROOT, 'firmware', 'common', 'src', 'driver.c'),
    measureText: true,
    // 1280 rather than 1024, decided when the budget was set rather than after
    // it broke: riscv32 measures 986 B, and 38 B of headroom is a budget that
    // fails on the next comment-sized change instead of on a real regression.
    // The frag budget learned this the expensive way — see its chore(budget)
    // commit.
    textBudget: 1280, // T3.1
    // Carries the _Static_assert tying LH_MAX_COMPONENTS * sizeof(lh_driver_ctx_t)
    // to the 512 B `mem.registry.static` budget. The context holds an int64_t
    // next to four bytes, so how much of that budget it spends is decided by
    // the target's alignment rules and by nothing the host build can see.
    extraFlags: ['-Os', '-ffunction-sections', '-fdata-sections'],
  },
  {
    id: 'i2c_scan',
    path: join(REPO_ROOT, 'firmware', 'common', 'src', 'i2c_scan.c'),
    measureText: true,
    textBudget: 768, // T3.2
    // Carries the assertion that ties the per-address timeout to the full-scan
    // budget: 112 x LH_I2C_PROBE_TIMEOUT_MS must fit LH_I2C_SCAN_BUDGET_MS.
    // Raising either one alone is meant to be a build failure, not a scan that
    // quietly runs long on an empty bus.
    extraFlags: ['-Os', '-ffunction-sections', '-fdata-sections'],
  },
  {
    id: 'slip',
    path: join(REPO_ROOT, 'firmware', 'common', 'src', 'slip.c'),
    measureText: true,
    textBudget: 1024, // T1.1
    // Also where the decoder's 32 B struct budget is enforced: slip.c carries a
    // _Static_assert on sizeof(lh_slip_decoder_t), so compiling it on both ESP
    // ABIs is what proves the budget holds where it matters, not just on the
    // host that happened to run the tests.
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
    // MSYS2 keeps its compilers off the Windows PATH on purpose — its own
    // shells add them, `cmd` does not. The candidate list comes from
    // host-cc.mjs so there is one answer in the repo to "where is gcc", not
    // one per tool that needs it.
    candidates: ['gcc', 'cc', 'clang', ...windowsGnuCandidates()],
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

/**
 * Environment for a compiler given by absolute path.
 *
 * A MinGW `gcc.exe` is a driver that spawns `cc1.exe`, `as.exe` and `ld.exe`,
 * and those load DLLs from the toolchain's own `bin`. Windows searches only the
 * *initial* executable's directory, so gcc starts, its children do not, and the
 * whole thing exits non-zero having printed nothing whatsoever. Prepending the
 * directory here keeps the fix scoped to this process instead of the machine's
 * PATH. The ESP cross compilers are self-contained and unaffected.
 */
function environmentFor(compiler) {
  if (!compiler.includes('/') && !compiler.includes('\\')) return process.env;
  const currentPath = process.env.PATH ?? process.env.Path ?? '';
  return { ...process.env, PATH: `${dirname(compiler)}${delimiter}${currentPath}` };
}

/**
 * First line of `<compiler> --version`.
 *
 * Emitted as LH_ENV so the collector can record which toolchain produced a size
 * metric. Byte sizes are a property of the compiler as much as of the code —
 * the same crc16.c measured 80 B locally and 76 B on CI purely because the two
 * machines had different GCC builds. Without this, the gate reads a toolchain
 * upgrade as a code regression.
 */
function compilerVersion(compiler) {
  try {
    const output = execFileSync(compiler, ['--version'], {
      encoding: 'utf8',
      stdio: 'pipe',
      env: environmentFor(compiler),
    });
    return output.split('\n')[0]?.trim() ?? 'unknown';
  } catch {
    return 'unknown';
  }
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
  const output = execFileSync(sizeTool, ['-A', objectFile], {
    encoding: 'utf8',
    env: environmentFor(sizeTool),
  });
  const sections = new Map();
  for (const line of output.split('\n')) {
    // `$` belongs in the character class: COFF names a per-symbol section
    // `.text$lorahome_crc16`, and without it those lines were silently skipped
    // and every section-split unit measured 0 B.
    const match = line.trim().match(/^(\.[\w.$]+)\s+(\d+)/);
    if (match) sections.set(match[1], Number(match[2]));
  }
  return sections;
}

/**
 * Sums a section and its per-symbol split across both object formats.
 *
 * ELF spells the split `.text.foo`; COFF — which MinGW produces — spells it
 * `.text$foo`, and calls read-only data `.rdata` rather than `.rodata`. Matching
 * only the ELF spelling reported a confident 0 B for every unit on the host
 * target, which is worse than reporting nothing: a size of zero sails under any
 * budget and looks like a pass.
 *
 * `.xdata` and `.pdata` are deliberately excluded. They are Windows SEH unwind
 * tables with no ESP32 equivalent, and folding them into `.text` would make the
 * host number describe the platform rather than the code.
 */
function sectionTotal(sections, prefixes) {
  const wanted = Array.isArray(prefixes) ? prefixes : [prefixes];
  let total = 0;
  for (const [name, size] of sections) {
    for (const prefix of wanted) {
      if (name === prefix || name.startsWith(`${prefix}.`) || name.startsWith(`${prefix}$`)) {
        total += size;
        break;
      }
    }
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

    console.log(`LH_ENV toolchain.${target.id}=${compilerVersion(compiler)}`);

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
          { stdio: 'pipe', env: environmentFor(compiler) },
        );

        if (unit.measureText) {
          const sizeTool = sizeToolFor(compiler);
          if (sizeTool !== null) {
            const sections = sectionSizes(sizeTool, objectFile);
            textSizes.push({
              target: target.id,
              unit: unit.id,
              bytes: sectionTotal(sections, ['.text']),
              rodata: sectionTotal(sections, ['.rodata', '.rdata']),
              budget: unit.textBudget,
            });
          }
        }
      } catch (error) {
        // Both streams, and the exit status when both are empty. A MinGW driver
        // whose cc1.exe cannot load its DLLs prints nothing at all, and a bare
        // "FAIL" line with no explanation under it cost real time to diagnose.
        const streams = `${error?.stdout ?? ''}${error?.stderr ?? ''}`.trim();
        const detail =
          streams !== ''
            ? streams
            : `exited with status ${error?.status ?? '?'} and produced no diagnostic` +
              ` (a MinGW driver does this when its helper executables cannot load their DLLs)`;
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

// Numeric, not "2/3": the metric parser rejects non-numeric values on purpose,
// and a headline coverage number that silently never reaches the baseline is
// exactly the kind of metric that quietly stops being tracked.
console.log(
  `LH_METRIC targets.compiled_clean value=${compiled} unit=targets budget=${TARGETS.length}` +
    ` (of ${TARGETS.length})`,
);
console.log(`LH_METRIC targets.total value=${TARGETS.length} unit=targets`);
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
