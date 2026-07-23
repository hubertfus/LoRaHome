import assert from 'node:assert/strict';
import { test } from 'node:test';
import { loadManifests } from '../src/registry.js';

test('loadManifests loads and validates the bundled manifests', async () => {
  const manifests = await loadManifests();
  const ids = manifests.map((m) => m.id).sort();
  assert.deepEqual(ids, ['bme680', 'gpio_digital']);

  const bme680 = manifests.find((m) => m.id === 'bme680')!;
  assert.equal(bme680.bus, 'i2c');
  assert.ok(bme680.outputs.some((o) => o.id === 'temperature'));
});
