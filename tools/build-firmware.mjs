/**
 * Links the real ESP32 firmware images and records what they cost.
 *
 * Until now the firmware was only ever compiled a translation unit at a time
 * (tools/compile-targets.mjs), which proves the headers and the C are sound but
 * says nothing about whether the thing links, and nothing at all about the two
 * budgets in roadmap §0.6 that are properties of the whole image: static RAM
 * and flash footprint. Those are the numbers that decide whether a node still
 * boots with 180 kB of heap free and whether two OTA slots still fit in 4 MB.
 *
 * PlatformIO already knows how to compute them per board — it subtracts the
 * flash-resident sections that a naive `size` column total would wrongly count
 * as RAM — so its own report is parsed rather than re-derived here. Getting
 * that arithmetic subtly wrong is how a budget ends up tracking the wrong
 * number for a year.
 *
 * With no PlatformIO installed the script prints SKIPPED and contributes
 * nothing. Only the `esp32dev` environments are built: firmware/node also
 * declares a `native` environment for host unit tests, which needs g++ and is
 * not what this measures.
 */
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));

/**
 * Images to link, with the §0.6 budgets that apply to each.
 *
 * The roadmap budgets the *node* explicitly — it is the constrained device, and
 * the one that has to hold two OTA slots plus NVS in 4 MB. The bridge is
 * recorded without budgets rather than given invented ones: §0.6 is a contract,
 * and adding a line to it takes a `chore(budget):` commit, not a tool author's
 * opinion.
 */
const IMAGES = [
  { id: 'bridge', dir: join(REPO_ROOT, 'firmware', 'bridge'), env: 'esp32dev' },
  {
    id: 'node',
    dir: join(REPO_ROOT, 'firmware', 'node'),
    env: 'esp32dev',
    ramBudget: 48 * 1024, // §0.6: static RAM (.bss+.data) <= 48 kB
    flashBudget: 700 * 1024, // §0.6: node image <= 700 kB
  },
];

function findPlatformIO() {
  const candidates = [
    join(homedir(), '.platformio', 'penv', 'Scripts', 'platformio.exe'),
    join(homedir(), '.platformio', 'penv', 'bin', 'platformio'),
    'platformio',
    'pio',
  ];

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

/** `RAM:   [=    ]   8.0% (used 26136 bytes from 327680 bytes)` */
function parseUsage(output, label) {
  const match = output.match(
    new RegExp(`^${label}:.*?\\(used\\s+(\\d+)\\s+bytes\\s+from\\s+(\\d+)\\s+bytes\\)`, 'm'),
  );
  return match === null ? null : { used: Number(match[1]), total: Number(match[2]) };
}

const pio = findPlatformIO();

if (pio === null) {
  console.log('SKIPPED  PlatformIO not installed — firmware images not linked or measured');
  console.log('LH_METRIC firmware.images_linked value=0 unit=count');
  process.exit(0);
}

try {
  const version = execFileSync(pio, ['--version'], { encoding: 'utf8', stdio: 'pipe' }).trim();
  console.log(`LH_ENV toolchain.platformio=${version}`);
} catch {
  /* version is provenance, not a gate */
}

let linked = 0;
let failed = 0;
const breaches = [];

for (const image of IMAGES) {
  let output;
  try {
    output = execFileSync(pio, ['run', '-d', image.dir, '-e', image.env], {
      encoding: 'utf8',
      stdio: 'pipe',
      maxBuffer: 64 * 1024 * 1024,
    });
  } catch (error) {
    console.error(
      `FAIL     ${image.id} — link failed\n${`${error?.stdout ?? ''}${error?.stderr ?? ''}`.trim()}`,
    );
    failed++;
    continue;
  }

  const ram = parseUsage(output, 'RAM');
  const flash = parseUsage(output, 'Flash');

  if (ram === null || flash === null) {
    // An up-to-date build can skip the link step and with it the size report.
    // Reporting SKIPPED beats reporting nothing, and beats inventing a number.
    console.log(`note     ${image.id} — linked, but PlatformIO printed no size report`);
    console.log(`LH_METRIC size.firmware.${image.id}.static_ram value=SKIPPED unit=B`);
    console.log(`LH_METRIC size.firmware.${image.id}.flash value=SKIPPED unit=B`);
    linked++;
    continue;
  }

  const ramBudget = image.ramBudget === undefined ? '' : ` budget=${image.ramBudget}`;
  const flashBudget = image.flashBudget === undefined ? '' : ` budget=${image.flashBudget}`;

  console.log(
    `LH_METRIC size.firmware.${image.id}.static_ram value=${ram.used} unit=B${ramBudget}` +
      ` (${((100 * ram.used) / ram.total).toFixed(1)}% of ${ram.total})`,
  );
  console.log(
    `LH_METRIC size.firmware.${image.id}.flash value=${flash.used} unit=B${flashBudget}` +
      ` (${((100 * flash.used) / flash.total).toFixed(1)}% of ${flash.total})`,
  );

  if (image.ramBudget !== undefined && ram.used > image.ramBudget) {
    breaches.push(`size.firmware.${image.id}.static_ram = ${ram.used} B (budget: ${image.ramBudget} B)`);
  }
  if (image.flashBudget !== undefined && flash.used > image.flashBudget) {
    breaches.push(`size.firmware.${image.id}.flash = ${flash.used} B (budget: ${image.flashBudget} B)`);
  }

  console.log(`OK       ${image.id.padEnd(8)} — RAM ${ram.used} B, flash ${flash.used} B`);
  linked++;
}

console.log(`LH_METRIC firmware.images_linked value=${linked} unit=count budget=${IMAGES.length}`);

for (const breach of breaches) console.error(`BUDGET BREACH: ${breach}`);

if (failed > 0 || breaches.length > 0) process.exit(1);
