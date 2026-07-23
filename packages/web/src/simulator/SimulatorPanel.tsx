import { Rule, RuleOp } from '@lorahome/protocol';
import { useMemo, useState } from 'react';
import { SimulatedSample, simulateRule } from './simulate.js';

const DEMO_RULE: Rule = {
  src_sensor_id: 10,
  op: RuleOp.GT,
  threshold: 25.5,
  hysteresis: 0.5,
  debounce_ms: 5000,
  action_id: 3,
  action_param: 1,
};

const DEMO_SAMPLES: SimulatedSample[] = [
  { atMs: 0, value: 22 },
  { atMs: 1000, value: 26 },
  { atMs: 3000, value: 27 },
  { atMs: 6000, value: 28 },
  { atMs: 9000, value: 24 },
];

/** Dry-runs a rule against a sample series in the browser — no hardware involved. */
export function SimulatorPanel() {
  const [samples] = useState<SimulatedSample[]>(DEMO_SAMPLES);
  const fired = useMemo(() => simulateRule(DEMO_RULE, samples), [samples]);

  return (
    <section>
      <h3>Simulator</h3>
      <p>
        Rule: value &gt; {DEMO_RULE.threshold} (hysteresis {DEMO_RULE.hysteresis}, debounce {DEMO_RULE.debounce_ms}ms)
      </p>
      <ul>
        {samples.map((s) => (
          <li key={s.atMs}>
            t={s.atMs}ms → {s.value}
          </li>
        ))}
      </ul>
      <p>{fired.length === 0 ? 'No fire events.' : `Fired at: ${fired.map((f) => `${f.atMs}ms`).join(', ')}`}</p>
    </section>
  );
}
