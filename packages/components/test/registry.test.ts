import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  BUS_TYPE_IDS,
  ManifestValidationError,
  validateManifest,
  validateManifestSet,
  type ComponentManifest,
} from '../src/manifest-schema.js';
import { loadManifests } from '../src/registry.js';

/** A manifest that passes, as a base for mutating one field at a time. */
function validManifest(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    id: 'test_sensor',
    driver_type_id: 99,
    display_name: 'Test Sensor',
    bus: { type: 'i2c', addresses: ['0x40'] },
    warmup_ms: 100,
    min_interval_ms: 1000,
    channels: [{ index: 0, id: 'value', unit: '°C', scale: 0.001, range: [-40000, 85000] }],
    power: { active_ma: 1, sleep_ua: 1, measure_ms: 10 },
    ui: { icon: 'gauge', color: '#123456', group: 'test' },
    ...overrides,
  };
}

test('loadManifests loads and validates the bundled manifests', async () => {
  const manifests = await loadManifests();
  const ids = manifests.map((m) => m.id).sort();
  assert.deepEqual(ids, ['bme680', 'gpio_digital']);

  const bme680 = manifests.find((m) => m.id === 'bme680')!;
  assert.equal(bme680.bus.type, 'i2c');
  assert.ok(bme680.channels.some((c) => c.id === 'temperature'));
  validateManifestSet(manifests);
});

/**
 * The type ids, written out where a person reads them.
 *
 * Duplicated on purpose rather than imported from anywhere. The
 * machine-checked version — against the linked firmware, not against another
 * copy of the same constant — is tools/check-manifest-coherence.mjs. Having
 * both means changing an id has to be done deliberately in two places, which
 * for a value that must never be reused is the point (R3.4).
 */
test('the shipped manifests declare the type ids the firmware uses', async () => {
  const byId = new Map((await loadManifests()).map((m) => [m.id, m]));
  assert.equal(byId.get('bme680')?.driver_type_id, 16);
  assert.equal(byId.get('gpio_digital')?.driver_type_id, 17);
});

test('a valid manifest passes', () => {
  assert.doesNotThrow(() => validateManifest(validManifest()));
});

test('driver_type_id must fit the 16-bit wire field', () => {
  assert.throws(
    () => validateManifest(validManifest({ driver_type_id: 0x10000 })),
    ManifestValidationError,
  );
  assert.throws(
    () => validateManifest(validManifest({ driver_type_id: 0 })),
    ManifestValidationError,
  );
  assert.throws(
    () => validateManifest(validManifest({ driver_type_id: 1.5 })),
    ManifestValidationError,
  );
});

/**
 * Channel indices must be dense from zero.
 *
 * The firmware addresses channels by index, so a gap means `poll(ctx, out, 2)`
 * returns something the manifest calls channel 3 — every reading correctly
 * transported and attached to the wrong name.
 */
test('channel indices must be dense and in order', () => {
  assert.throws(
    () =>
      validateManifest(
        validManifest({
          channels: [
            { index: 0, id: 'a', unit: '', scale: 1, range: [0, 1] },
            { index: 2, id: 'b', unit: '', scale: 1, range: [0, 1] },
          ],
        }),
      ),
    ManifestValidationError,
  );

  assert.throws(
    () =>
      validateManifest(
        validManifest({ channels: [{ index: 1, id: 'a', unit: '', scale: 1, range: [0, 1] }] }),
      ),
    ManifestValidationError,
  );
});

test('channel ids must be unique within a manifest', () => {
  assert.throws(
    () =>
      validateManifest(
        validManifest({
          channels: [
            { index: 0, id: 'same', unit: '', scale: 1, range: [0, 1] },
            { index: 1, id: 'same', unit: '', scale: 1, range: [0, 1] },
          ],
        }),
      ),
    ManifestValidationError,
  );
});

/**
 * The range bounds the raw integer reading, not the displayed value.
 *
 * A fractional bound means somebody wrote 85.0 where 85000 belongs, and the
 * zombie-sensor guard (R3.6) then never triggers where they think it does.
 */
test('channel ranges must be integer, ordered, and fit an int32', () => {
  const withRange = (range: unknown) =>
    validManifest({ channels: [{ index: 0, id: 'v', unit: '', scale: 1, range }] });

  assert.throws(() => validateManifest(withRange([85.5, 100])), ManifestValidationError);
  assert.throws(() => validateManifest(withRange([100, 0])), ManifestValidationError);
  assert.throws(() => validateManifest(withRange([5, 5])), ManifestValidationError);
  assert.throws(() => validateManifest(withRange([0, 2147483648])), ManifestValidationError);
  assert.throws(() => validateManifest(withRange([0])), ManifestValidationError);
});

test('scale must be a non-zero finite number', () => {
  const withScale = (scale: unknown) =>
    validManifest({ channels: [{ index: 0, id: 'v', unit: '', scale, range: [0, 1] }] });

  assert.throws(() => validateManifest(withScale(0)), ManifestValidationError);
  assert.throws(() => validateManifest(withScale('0.001')), ManifestValidationError);
  assert.doesNotThrow(() => validateManifest(withScale(0.001)));
});

/**
 * An I2C address the scanner never visits describes a component nobody can add.
 *
 * The sweep covers 0x08..0x77 — the reserved blocks at both ends are never put
 * on the wire — so a manifest declaring 0x7A describes a device that capability
 * discovery will never find.
 */
test('i2c addresses must be inside the scannable range', () => {
  assert.throws(
    () => validateManifest(validManifest({ bus: { type: 'i2c', addresses: ['0x7A'] } })),
    ManifestValidationError,
  );
  assert.throws(
    () => validateManifest(validManifest({ bus: { type: 'i2c', addresses: ['0x03'] } })),
    ManifestValidationError,
  );
  assert.throws(
    () => validateManifest(validManifest({ bus: { type: 'i2c', addresses: ['118'] } })),
    ManifestValidationError,
  );
  assert.doesNotThrow(() =>
    validateManifest(validManifest({ bus: { type: 'i2c', addresses: ['0x08', '0x77'] } })),
  );
});

test('bus type must be one the protocol has a wire value for', () => {
  for (const busType of Object.keys(BUS_TYPE_IDS)) {
    assert.doesNotThrow(() => validateManifest(validManifest({ bus: { type: busType } })));
  }
  assert.throws(
    () => validateManifest(validManifest({ bus: { type: 'canbus' } })),
    ManifestValidationError,
  );
});

test('power and ui blocks are required and typed', () => {
  assert.throws(() => validateManifest(validManifest({ power: {} })), ManifestValidationError);
  assert.throws(
    () => validateManifest(validManifest({ power: { active_ma: -1, sleep_ua: 0, measure_ms: 1 } })),
    ManifestValidationError,
  );
  assert.throws(
    () => validateManifest(validManifest({ ui: { icon: 'x', color: 'red', group: 'g' } })),
    ManifestValidationError,
  );
  assert.doesNotThrow(() =>
    validateManifest(validManifest({ ui: { icon: 'x', color: '#ABCDEF', group: 'g' } })),
  );
});

/**
 * Two manifests can each be valid while the pair is broken.
 *
 * A reused type id is exactly the drift R3.4 describes, and neither file is
 * wrong on its own — which is why the set needs a check of its own.
 */
test('a set with a reused type id is rejected', () => {
  const a = validateManifest(validManifest({ id: 'first', driver_type_id: 42 }));
  const b = validateManifest(validManifest({ id: 'second', driver_type_id: 42 }));

  assert.throws(() => validateManifestSet([a, b]), ManifestValidationError);
  assert.doesNotThrow(() =>
    validateManifestSet([a, validateManifest(validManifest({ id: 'second', driver_type_id: 43 }))]),
  );
});

test('validating fifty manifests stays inside the budget', () => {
  const synthetic = Array.from({ length: 50 }, (_, i) =>
    validManifest({ id: `synthetic_${i}`, driver_type_id: 1000 + i }),
  );

  const started = process.hrtime.bigint();
  const validated = synthetic.map((m) => validateManifest(m) as ComponentManifest);
  validateManifestSet(validated);
  const elapsedMs = Number(process.hrtime.bigint() - started) / 1e6;

  assert.ok(elapsedMs < 100, `validating 50 manifests took ${elapsedMs.toFixed(1)} ms, budget 100`);
});
