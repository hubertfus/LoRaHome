/**
 * Cross-language verification of the LoRa time-on-air calculation (T1.3).
 *
 * Two implementations exist because two components enforce the ETSI duty cycle
 * independently — the Host's Duty Cycle Guard in TypeScript, the Bridge's
 * tracker in C (ARCHITECTURE.md §7). Independent enforcement is only worth
 * anything if both agree on what a frame costs; if they drift, one of them is
 * wrong about a legal limit, and it is the kind of wrong that shows up as a
 * regulator's letter rather than a failing test.
 *
 * So the same parameter grid goes through both and the results must match to
 * within float precision. This is the airtime twin of the CRC vector check.
 *
 * It also prints the profile figure T1.3 actually cares about: what a full
 * 230 B frame costs at the chosen SF9/BW125/CR4-5 profile, and what that
 * implies for the 1% duty cycle.
 *
 * Not covered, and owed: verification against a real transmission. The roadmap
 * asks for measured-vs-computed within 5%, checked with an SDR or a logic
 * analyser. Both implementations agreeing proves they share a formula, not that
 * the formula matches the radio.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { exeName, findHostToolchain, spawnFailureDetail } from './host-cc.mjs';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));
const COMMON = join(REPO_ROOT, 'firmware', 'common');

/** The shipping profile — LoraProfile in firmware/common/include/lorahome/lora_transport.h. */
const PROFILE = { spreadingFactor: 9, bandwidthHz: 125000, codingRate: 1, preambleSymbols: 8 };
const MTU = 230;

/** ETSI EN 300 220: 1% of a rolling hour on this band. */
const DUTY_CYCLE = 0.01;

/**
 * Parameter grid.
 *
 * Spans every spreading factor (SF11/SF12 turn on low-data-rate optimisation,
 * which changes the divisor and is the most likely place for the two
 * implementations to part company), both common bandwidths, all four coding
 * rates, and payload lengths including the degenerate ones.
 */
function buildGrid() {
  const rows = [];
  for (const sf of [7, 8, 9, 10, 11, 12]) {
    for (const bandwidthHz of [125000, 250000]) {
      for (const codingRate of [1, 2, 3, 4]) {
        for (const bytes of [0, 1, 8, 51, 100, 230, 255]) {
          rows.push({ bytes, sf, bandwidthHz, codingRate, preambleSymbols: 8 });
        }
      }
    }
  }
  return rows;
}

const hostTsAirtime = join(REPO_ROOT, 'packages', 'host', 'dist', 'src', 'duty-cycle-guard', 'airtime.js');

if (!existsSync(hostTsAirtime)) {
  console.log('SKIPPED  packages/host is not built — run `pnpm -r build` first');
  console.log('LH_METRIC test.airtime.cross_lang value=SKIPPED unit=count');
  process.exit(0);
}

const { computeAirtimeMs } = await import(pathToFileURL(hostTsAirtime).href);

const toolchain = findHostToolchain();
const grid = buildGrid();

// ---------------------------------------------------------------------------
// The profile figure, from the TypeScript side (always available).
// ---------------------------------------------------------------------------

const profileMs = computeAirtimeMs({ bytes: MTU, ...PROFILE });
const minIntervalS = profileMs / 1000 / DUTY_CYCLE;

console.log(
  `LH_METRIC bench.lora.airtime.230B.sf9.computed value=${profileMs.toFixed(1)} unit=ms` +
    ` (SF9 BW125 CR4/5 preamble 8, explicit header)`,
);
console.log(
  `LH_METRIC lora.duty_cycle.min_interval.230B.sf9 value=${minIntervalS.toFixed(1)} unit=s` +
    ` (ETSI 1% — shortest legal gap between two full frames)`,
);
console.log(
  `LH_METRIC bench.lora.airtime.230B.sf9.measured value=SKIPPED unit=ms budget=${(profileMs * 1.05).toFixed(1)}` +
    ` (needs an SDR or logic analyser capture)`,
);

// ---------------------------------------------------------------------------
// Cross-language comparison.
// ---------------------------------------------------------------------------

if (toolchain === null) {
  console.log('SKIPPED  no host C toolchain — the C implementation was not compared');
  console.log('LH_METRIC test.airtime.cross_lang value=SKIPPED unit=count');
  process.exit(0);
}

console.log(`LH_ENV toolchain.airtime_check=${toolchain.version}`);

const workDir = mkdtempSync(join(tmpdir(), 'lh-airtime-'));
let mismatches = 0;

try {
  const binary = join(workDir, exeName('airtime_cli'));
  try {
    toolchain.compile({
      sources: [join(COMMON, 'test', 'airtime_cli.c'), join(COMMON, 'src', 'airtime.c')],
      includeDirs: [join(COMMON, 'include')],
      output: binary,
      optimize: 'O2',
    });
  } catch (error) {
    console.error(`FAIL     could not build the airtime CLI\n${spawnFailureDetail(error)}`);
    process.exit(1);
  }

  const input =
    grid
      .map((r) => `${r.bytes} ${r.sf} ${r.bandwidthHz} ${r.codingRate} ${r.preambleSymbols}`)
      .join('\n') + '\n';

  const stdout = execFileSync(binary, [], { input, encoding: 'utf8', env: toolchain.env });
  const fromC = stdout.trim().split(/\r?\n/).map(Number);

  if (fromC.length !== grid.length) {
    console.error(`FAIL     C produced ${fromC.length} results for ${grid.length} inputs`);
    process.exit(1);
  }

  for (const [index, row] of grid.entries()) {
    const expected = computeAirtimeMs({
      bytes: row.bytes,
      spreadingFactor: row.sf,
      bandwidthHz: row.bandwidthHz,
      codingRate: row.codingRate,
      preambleSymbols: row.preambleSymbols,
    });

    // The C side computes in float, the TypeScript side in double, so an exact
    // comparison would fail on representation alone. A relative tolerance of
    // 1e-4 is far tighter than any disagreement about the formula could be, and
    // far looser than float rounding — it separates the two cleanly.
    const tolerance = Math.max(expected * 1e-4, 1e-4);
    if (Math.abs(fromC[index] - expected) > tolerance) {
      mismatches++;
      if (mismatches <= 5) {
        console.error(
          `MISMATCH bytes=${row.bytes} sf=${row.sf} bw=${row.bandwidthHz} cr=${row.codingRate}:` +
            ` C=${fromC[index]} TS=${expected.toFixed(6)}`,
        );
      }
    }
  }
} finally {
  rmSync(workDir, { recursive: true, force: true });
}

console.log(
  `LH_METRIC test.airtime.cross_lang value=${grid.length - mismatches} unit=count budget=${grid.length}`,
);
console.log(`LH_METRIC test.airtime.mismatches value=${mismatches} unit=count budget=0`);

if (mismatches > 0) {
  console.error(`\n${mismatches} of ${grid.length} airtime values disagree between C and TypeScript.`);
  process.exit(1);
}
console.log(`OK       airtime — ${grid.length}/${grid.length} values agree between C and TypeScript`);
