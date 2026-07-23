import { ComponentManifest } from '@lorahome/components';
import { useState } from 'react';
import { RuleGraphEditor } from './editor/RuleGraphEditor.js';
import { ManifestForm } from './manifest-form/ManifestForm.js';
import { SimulatorPanel } from './simulator/SimulatorPanel.js';
import { TimeTravelSlider } from './time-travel/TimeTravelSlider.js';

const BME680_MANIFEST: ComponentManifest = {
  id: 'bme680',
  bus: 'i2c',
  addresses: ['0x76', '0x77'],
  params: [
    { key: 'oversampling_temp', type: 'enum', options: [1, 2, 4, 8, 16], default: 2 },
    { key: 'warmup_ms', type: 'uint16', default: 300 },
  ],
  outputs: [{ id: 'temperature', unit: '°C', type: 'float' }],
  power: { active_ua: 3600, sleep_ua: 0.15, measurement_ms: 189 },
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
