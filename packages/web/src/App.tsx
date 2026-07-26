import { ComponentManifest } from '@lorahome/components';
import { useState } from 'react';
import { RuleGraphEditor } from './editor/RuleGraphEditor.js';
import { ManifestForm } from './manifest-form/ManifestForm.js';
import { SimulatorPanel } from './simulator/SimulatorPanel.js';
import { TimeTravelSlider } from './time-travel/TimeTravelSlider.js';

/**
 * A manifest inlined for the scaffold, in schema v1.
 *
 * Kept byte-compatible with packages/components/manifests/bme680.json rather
 * than loaded from it, because this page has no data layer yet. When Etap 6
 * builds one, this constant goes and the manifests are loaded properly — an
 * inlined copy is exactly the kind of second source of truth this project
 * spends its effort eliminating everywhere else.
 */
const BME680_MANIFEST: ComponentManifest = {
  id: 'bme680',
  driver_type_id: 16,
  display_name: 'BME680 Environmental Sensor',
  bus: { type: 'i2c', addresses: ['0x76', '0x77'] },
  warmup_ms: 200,
  min_interval_ms: 3000,
  channels: [
    { index: 0, id: 'temperature', unit: '°C', scale: 0.001, range: [-40000, 85000] },
    { index: 1, id: 'humidity', unit: '%', scale: 0.001, range: [0, 100000] },
    { index: 2, id: 'pressure', unit: 'hPa', scale: 0.01, range: [30000, 110000] },
    { index: 3, id: 'gas', unit: 'Ω', scale: 1, range: [0, 500000] },
  ],
  power: { active_ma: 12.1, sleep_ua: 0.9, measure_ms: 200 },
  ui: { icon: 'thermometer', color: '#e07b39', group: 'environment' },
};

export function App() {
  const [timeMs, setTimeMs] = useState(0);

  return (
    <main>
      <h1>LoRaHome</h1>

      <section>
        <h2>Rule graph editor</h2>
        <RuleGraphEditor />
      </section>

      <section>
        <h2>Component configuration</h2>
        <ManifestForm manifest={BME680_MANIFEST} />
      </section>

      <SimulatorPanel />

      <section>
        <h2>Time-travel debug</h2>
        <TimeTravelSlider minMs={0} maxMs={60_000} valueMs={timeMs} onChange={setTimeMs} />
      </section>
    </main>
  );
}
