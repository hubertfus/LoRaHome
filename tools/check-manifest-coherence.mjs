/**
 * Manifest <-> firmware coherence. Roadmap T3.6, risk R3.4.
 *
 * The most important check in Etap 3, and the cheapest one to skip.
 *
 * A manifest and a driver are tied together by exactly one number, the
 * `driver_type_id`. Nothing at runtime notices when they disagree: the node
 * reports type 16, the host looks up type 16, finds a manifest that says
 * "temperature, humidity, pressure, gas" and renders four channels of whatever
 * the driver actually produced. The readings are wrong, they are wrong
 * plausibly, and every layer involved believes it is working. There is no error
 * message anywhere in that story.
 *
 * So the check runs at build time and fails the build. It reads the firmware's
 * registry by *running* it — compiling driver_registry.c against the real
 * drivers and printing what the linked image declares — rather than by parsing
 * the source. Those differ exactly when it matters: a driver behind an #ifdef,
 * a vtable built through a macro, a translation unit somebody excluded. A check
 * that re-reads the text a human already read is not much of a check.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, readdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { exeName, findHostToolchain, spawnFailureDetail } from './host-cc.mjs';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));
const COMMON = join(REPO_ROOT, 'firmware', 'common');
const NODE_DRIVERS = join(REPO_ROOT, 'firmware', 'node', 'src', 'drivers');
const MANIFEST_DIR = join(REPO_ROOT, 'packages', 'components', 'manifests');
const SCHEMA_MODULE = join(
  REPO_ROOT, 'packages', 'components', 'dist', 'src', 'manifest-schema.js',
);

function skip(reason) {
  console.log(`SKIPPED  ${reason}`);
  console.log('LH_METRIC test.manifest_fw_coherence value=SKIPPED unit=count');
  process.exit(0);
}

if (!existsSync(SCHEMA_MODULE)) {
  skip('packages/components is not built — run `pnpm -r build` first');
}

const toolchain = findHostToolchain();
if (toolchain === null) skip('no host C toolchain — the firmware registry was not read');

const { validateManifest, validateManifestSet, BUS_TYPE_IDS } = await import(
  pathToFileURL(SCHEMA_MODULE).href
);

console.log(`LH_ENV toolchain.manifest_check=${toolchain.version}`);

/* -------------------------------------------------------------------------- */
/* Manifests                                                                  */
/* -------------------------------------------------------------------------- */

const started = Date.now();
const manifests = [];
const problems = [];

for (const file of readdirSync(MANIFEST_DIR).filter((f) => f.endsWith('.json')).sort()) {
  try {
    manifests.push(validateManifest(JSON.parse(readFileSync(join(MANIFEST_DIR, file), 'utf8'))));
  } catch (error) {
    problems.push(`${file}: ${error.message}`);
  }
}

try {
  validateManifestSet(manifests);
} catch (error) {
  problems.push(error.message);
}

const validateMs = Date.now() - started;

/* -------------------------------------------------------------------------- */
/* The firmware's own account of itself                                       */
/* -------------------------------------------------------------------------- */

const workDir = mkdtempSync(join(tmpdir(), 'lh-coherence-'));
let firmware = [];

/**
 * Builds and runs the registry CLI, retrying a launch the OS refused.
 *
 * Same phenomenon and same workaround as run-native.mjs: on this Windows
 * machine Application Control refuses to execute a just-linked unsigned binary
 * roughly one run in three, and rebuilding to a *new path* is what clears it —
 * relaunching the same file does not. Only a failure to start is retried. A
 * process that ran and exited non-zero has produced a result, and re-rolling it
 * is exactly the habit a metric gate exists to prevent.
 */
function readRegistry() {
  const LAUNCH_ATTEMPTS = 6;
  const SPAWN_LEVEL_CODES = new Set(['UNKNOWN', 'ETXTBSY', 'EBUSY', 'EPERM', 'EACCES', 'EAGAIN']);
  const couldNotStart = (error) =>
    error?.status === null || error?.status === undefined || SPAWN_LEVEL_CODES.has(error?.code);

  for (let attempt = 1; ; attempt++) {
    const binary = join(workDir, exeName(`driver_registry_cli-${attempt}`));
    try {
      toolchain.compile({
        sources: [
          join(COMMON, 'test', 'driver_registry_cli.c'),
          join(NODE_DRIVERS, 'driver_registry.c'),
          join(COMMON, 'src', 'driver.c'),
          join(COMMON, 'src', 'bme680.c'),
          join(COMMON, 'src', 'gpio_digital.c'),
        ],
        includeDirs: [join(COMMON, 'include')],
        output: binary,
        optimize: 'O2',
      });
    } catch (error) {
      console.error(`FAIL     could not build the registry CLI\n${spawnFailureDetail(error)}`);
      process.exit(1);
    }

    try {
      const output = execFileSync(binary, [], { encoding: 'utf8', env: toolchain.env });
      if (attempt > 1) console.log(`note     the registry CLI ran on attempt ${attempt}`);
      return output;
    } catch (error) {
      if (couldNotStart(error) && attempt < LAUNCH_ATTEMPTS) continue;
      console.error(
        `FAIL     could not run the registry CLI\n${spawnFailureDetail(error)}`,
      );
      process.exit(1);
    }
  }
}

try {
  firmware = readRegistry()
    .trimEnd()
    .split(/\r?\n/)
    .filter((line) => line.length > 0)
    .map((line) => {
      const [name, typeId, channels, warmup, interval] = line.split('\t');
      return {
        name,
        typeId: Number(typeId),
        channels: Number(channels),
        warmupMs: Number(warmup),
        minIntervalMs: Number(interval),
      };
    });
} finally {
  rmSync(workDir, { recursive: true, force: true });
}

/* -------------------------------------------------------------------------- */
/* The comparison                                                             */
/* -------------------------------------------------------------------------- */

const manifestsById = new Map(manifests.map((m) => [m.id, m]));
const firmwareByName = new Map(firmware.map((d) => [d.name, d]));
let matched = 0;

for (const driver of firmware) {
  const manifest = manifestsById.get(driver.name);
  if (manifest === undefined) {
    problems.push(
      `firmware ships driver "${driver.name}" (type ${driver.typeId}) with no manifest — ` +
        `the host would discover a component it cannot describe`,
    );
    continue;
  }

  let ok = true;

  if (manifest.driver_type_id !== driver.typeId) {
    // The one that matters most. Nothing at runtime notices.
    problems.push(
      `"${driver.name}": manifest says driver_type_id ${manifest.driver_type_id}, ` +
        `firmware declares ${driver.typeId}`,
    );
    ok = false;
  }
  if (manifest.channels.length !== driver.channels) {
    problems.push(
      `"${driver.name}": manifest describes ${manifest.channels.length} channels, ` +
        `firmware declares ${driver.channels}`,
    );
    ok = false;
  }
  if (manifest.warmup_ms !== driver.warmupMs) {
    // Not cosmetic: the scheduler enforces the firmware's figure and the energy
    // calculator spends the manifest's. A disagreement makes a battery estimate
    // wrong by however far apart they are.
    problems.push(
      `"${driver.name}": manifest declares warmup_ms ${manifest.warmup_ms}, ` +
        `firmware declares ${driver.warmupMs}`,
    );
    ok = false;
  }
  if (manifest.min_interval_ms !== driver.minIntervalMs) {
    problems.push(
      `"${driver.name}": manifest declares min_interval_ms ${manifest.min_interval_ms}, ` +
        `firmware declares ${driver.minIntervalMs}`,
    );
    ok = false;
  }

  if (ok) matched++;
}

for (const manifest of manifests) {
  if (!firmwareByName.has(manifest.id)) {
    // Not fatal on its own — a manifest can land before its driver — but it is
    // worth saying out loud, because the UI will offer a component the node
    // cannot actually instantiate.
    console.log(
      `note     manifest "${manifest.id}" (type ${manifest.driver_type_id}) has no driver in this image`,
    );
  }
}

for (const problem of problems) console.error(`DRIFT    ${problem}`);

console.log(`LH_METRIC test.manifest_fw_coherence value=${matched} unit=count budget=${firmware.length}`);
console.log(`LH_METRIC manifest.count value=${manifests.length} unit=count`);
console.log(`LH_METRIC manifest.drivers_in_image value=${firmware.length} unit=count`);
console.log(`LH_METRIC manifest.drift value=${problems.length} unit=count budget=0`);
console.log(`LH_METRIC bench.manifest.validate.ms value=${validateMs} unit=ms budget=100`);

if (problems.length > 0) {
  console.error(`\n${problems.length} manifest/firmware disagreement(s). This is risk R3.4.`);
  process.exit(1);
}
console.log(
  `OK       manifest coherence — ${matched}/${firmware.length} drivers match their manifests`,
);
