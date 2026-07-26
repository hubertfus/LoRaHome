/**
 * Component manifest, schema v1. Roadmap T3.6, ARCHITECTURE.md §5.
 *
 * A manifest is the contract between firmware, host and UI. The firmware knows
 * how to talk to a BME680; the manifest says what its channels mean, how to
 * scale them, what they are called and what a plausible value looks like. That
 * separation is what makes "adding a sensor is a new file" true: the UI renders
 * from the manifest, the energy calculator reads its power block, and the
 * simulator uses its ranges — one description, three consumers, none of which
 * has a line of code that knows the word "bme680".
 *
 * Manifests never go over the radio. They live on the host and in the browser;
 * what crosses the air is integer-keyed CBOR referring to `driver_type_id`.
 *
 * The field that makes this dangerous is `driver_type_id`. It is the only thing
 * tying a manifest to a driver, it is permanent, and if a manifest and a
 * firmware disagree about one the host renders a BME680's name over readings a
 * GPIO pin produced — data that is wrong in a way nothing detects. That is risk
 * R3.4, and tools/check-manifest-coherence.mjs is what makes it a build failure
 * rather than a field report.
 */

export const MANIFEST_SCHEMA_VERSION = 1;

export type BusType = 'i2c' | 'gpio' | 'spi' | 'onewire' | 'adc';

/** Wire values for `bus.type`, matching lh_bus_type_t in capability.h. */
export const BUS_TYPE_IDS: Record<BusType, number> = {
  i2c: 0,
  gpio: 1,
  spi: 2,
  onewire: 3,
  adc: 4,
};

export interface ManifestChannel {
  /** Position in the driver's channel list. Must be dense from 0. */
  index: number;
  /** Stable identifier used by rules and the UI. */
  id: string;
  unit: string;
  /**
   * Multiply the integer reading by this to get the displayed value.
   *
   * Readings are `int32_t` on the wire and in the rule engine (R3.3); this is
   * how a UI turns 23450 into 23.45 °C. It belongs here rather than in the
   * firmware because the firmware never needs to know — only the presentation
   * layer does, and putting a float in the node would be a float in the rule
   * path by the next refactor.
   */
  scale: number;
  /**
   * Plausible range of the *integer* reading, inclusive.
   *
   * Not calibration. This is the zombie-sensor guard from R3.6: a device that
   * still acknowledges its address and returns rubbish produces values outside
   * this band, and the rule engine refuses them rather than clicking a relay at
   * three in the morning on a number nobody checked.
   */
  range: [number, number];
}

export interface ManifestPower {
  /** Milliamps while actively measuring. */
  active_ma: number;
  /** Microamps while asleep. */
  sleep_ua: number;
  /** Milliseconds one measurement takes, excluding warm-up. */
  measure_ms: number;
}

export interface ManifestUi {
  icon: string;
  color: string;
  group: string;
}

export interface ComponentManifest {
  $schema?: string;
  id: string;
  /** MUST match lh_driver_vtable_t.type_id in the firmware. */
  driver_type_id: number;
  display_name: string;
  bus: { type: BusType; addresses?: string[] };
  warmup_ms: number;
  min_interval_ms: number;
  channels: ManifestChannel[];
  power: ManifestPower;
  ui: ManifestUi;
}

export class ManifestValidationError extends Error {}

function fail(id: string, message: string): never {
  throw new ManifestValidationError(`manifest "${id}": ${message}`);
}

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

const isFiniteNumber = (value: unknown): value is number =>
  typeof value === 'number' && Number.isFinite(value);

/**
 * Structural validation. Not a JSON Schema implementation, deliberately.
 *
 * A schema validator would check shapes; most of what can go wrong here is not
 * a shape. A channel list with a gap, a range whose bounds are the wrong way
 * round, two channels sharing an id — all of those are structurally valid JSON
 * and all of them break something downstream in a way that is hard to trace
 * back. Those are the checks worth having, and they are why this is code.
 */
export function validateManifest(value: unknown): ComponentManifest {
  if (!isRecord(value)) throw new ManifestValidationError('manifest must be an object');

  const id = typeof value.id === 'string' ? value.id : '<no id>';
  if (typeof value.id !== 'string' || value.id.length === 0) {
    throw new ManifestValidationError('manifest.id must be a non-empty string');
  }

  if (!Number.isInteger(value.driver_type_id) || (value.driver_type_id as number) < 1) {
    fail(id, 'driver_type_id must be a positive integer');
  }
  if ((value.driver_type_id as number) > 0xffff) {
    // The wire field is 16 bits. A larger id would encode as something else.
    fail(id, `driver_type_id ${value.driver_type_id} does not fit the 16-bit wire field`);
  }
  if (typeof value.display_name !== 'string' || value.display_name.length === 0) {
    fail(id, 'display_name must be a non-empty string');
  }

  const bus = value.bus;
  if (!isRecord(bus) || typeof bus.type !== 'string' || !(bus.type in BUS_TYPE_IDS)) {
    fail(id, `bus.type must be one of ${Object.keys(BUS_TYPE_IDS).join(', ')}`);
  }
  const busType = (bus as Record<string, unknown>).type as BusType;
  const addresses = (bus as Record<string, unknown>).addresses;
  if (addresses !== undefined) {
    if (!Array.isArray(addresses) || addresses.some((a) => typeof a !== 'string')) {
      fail(id, 'bus.addresses must be an array of strings');
    }
    for (const address of addresses as string[]) {
      if (!/^0x[0-9a-fA-F]{2}$/.test(address)) {
        fail(id, `bus address "${address}" must look like 0x76`);
      }
      const numeric = Number.parseInt(address, 16);
      if (busType === 'i2c' && (numeric < 0x08 || numeric > 0x77)) {
        // The scanner sweeps 0x08..0x77; anything else can never be discovered,
        // so a manifest declaring it describes a component nobody can add.
        fail(id, `i2c address ${address} is outside the scannable range 0x08..0x77`);
      }
    }
  }

  if (!Number.isInteger(value.warmup_ms) || (value.warmup_ms as number) < 0) {
    fail(id, 'warmup_ms must be a non-negative integer');
  }
  if (!Number.isInteger(value.min_interval_ms) || (value.min_interval_ms as number) < 0) {
    fail(id, 'min_interval_ms must be a non-negative integer');
  }

  validateChannels(id, value.channels);
  validatePower(id, value.power);
  validateUi(id, value.ui);

  return value as unknown as ComponentManifest;
}

function validateChannels(id: string, value: unknown): void {
  if (!Array.isArray(value) || value.length === 0) {
    fail(id, 'channels must be a non-empty array');
  }

  const seenIds = new Set<string>();

  for (const [position, entry] of (value as unknown[]).entries()) {
    if (!isRecord(entry)) fail(id, `channel ${position} must be an object`);

    // Dense from zero, in order. The firmware addresses channels by index, so a
    // gap means poll(ctx, out, 2) reads something the manifest calls channel 3.
    if (entry.index !== position) {
      fail(id, `channel at position ${position} declares index ${entry.index}; indices must be dense from 0`);
    }
    if (typeof entry.id !== 'string' || entry.id.length === 0) {
      fail(id, `channel ${position} needs a non-empty id`);
    }
    if (seenIds.has(entry.id as string)) {
      // Rules reference channels by id. Two with the same name means a rule
      // that silently binds to whichever the loader saw last.
      fail(id, `duplicate channel id "${entry.id}"`);
    }
    seenIds.add(entry.id as string);

    if (typeof entry.unit !== 'string') fail(id, `channel "${entry.id}" needs a unit`);
    if (!isFiniteNumber(entry.scale) || entry.scale === 0) {
      fail(id, `channel "${entry.id}": scale must be a non-zero finite number`);
    }

    const range = entry.range;
    if (!Array.isArray(range) || range.length !== 2 || !range.every(isFiniteNumber)) {
      fail(id, `channel "${entry.id}": range must be a pair of numbers`);
    }
    const [low, high] = range as [number, number];
    if (low >= high) {
      fail(id, `channel "${entry.id}": range [${low}, ${high}] is empty or inverted`);
    }
    if (!Number.isInteger(low) || !Number.isInteger(high)) {
      // The range bounds the *integer* reading, so a fractional bound means
      // somebody wrote the display value and the guard will never trigger where
      // they think it does.
      fail(id, `channel "${entry.id}": range bounds must be integers (they bound the raw reading)`);
    }
    if (low < -2147483648 || high > 2147483647) {
      fail(id, `channel "${entry.id}": range does not fit an int32 reading`);
    }
  }
}

function validatePower(id: string, value: unknown): void {
  if (!isRecord(value)) fail(id, 'power must be an object');
  for (const key of ['active_ma', 'sleep_ua', 'measure_ms'] as const) {
    if (!isFiniteNumber(value[key]) || (value[key] as number) < 0) {
      fail(id, `power.${key} must be a non-negative number`);
    }
  }
}

function validateUi(id: string, value: unknown): void {
  if (!isRecord(value)) fail(id, 'ui must be an object');
  if (typeof value.icon !== 'string' || value.icon.length === 0) fail(id, 'ui.icon is required');
  if (typeof value.group !== 'string' || value.group.length === 0) fail(id, 'ui.group is required');
  if (typeof value.color !== 'string' || !/^#[0-9a-fA-F]{6}$/.test(value.color)) {
    fail(id, 'ui.color must be a #rrggbb hex colour');
  }
}

/**
 * Checks a set of manifests against each other.
 *
 * Every manifest can be individually valid while the set is broken: two files
 * claiming the same `driver_type_id` is exactly the drift R3.4 describes, and
 * neither file is wrong on its own.
 */
export function validateManifestSet(manifests: ComponentManifest[]): void {
  const byTypeId = new Map<number, string>();
  const byId = new Map<string, string>();

  for (const manifest of manifests) {
    const existingType = byTypeId.get(manifest.driver_type_id);
    if (existingType !== undefined) {
      throw new ManifestValidationError(
        `driver_type_id ${manifest.driver_type_id} is claimed by both "${existingType}" and "${manifest.id}"`,
      );
    }
    byTypeId.set(manifest.driver_type_id, manifest.id);

    const existingId = byId.get(manifest.id);
    if (existingId !== undefined) {
      throw new ManifestValidationError(`two manifests share the id "${manifest.id}"`);
    }
    byId.set(manifest.id, manifest.id);
  }
}
