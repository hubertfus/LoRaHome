import { createRuleState, Rule, stepRule } from '@lorahome/protocol';

export interface SimulatedSample {
  atMs: number;
  value: number;
}

export interface SimulatedFireEvent {
  atMs: number;
  value: number;
}

/**
 * Dry-runs a rule against a hand-authored (or recorded) time series of
 * sensor values, without touching any hardware. Reuses the exact same
 * hysteresis/debounce state machine the Host's global rule engine runs
 * (@lorahome/protocol), so what fires here is what would fire in the field.
 */
export function simulateRule(rule: Rule, samples: SimulatedSample[]): SimulatedFireEvent[] {
  const state = createRuleState();
  const fired: SimulatedFireEvent[] = [];

  for (const sample of [...samples].sort((a, b) => a.atMs - b.atMs)) {
    if (stepRule(state, rule, sample.value, sample.atMs)) {
      fired.push({ atMs: sample.atMs, value: sample.value });
    }
  }

  return fired;
}
