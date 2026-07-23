import { createRuleState, Rule, RuleEvaluatorState, stepRule } from '@lorahome/protocol';

/** A rule scoped to a specific source device — the global engine is the only place rules cross device boundaries. */
export interface GlobalRule extends Rule {
  src_device_id: number;
}

export interface RuleFireEvent {
  rule: GlobalRule;
  value: number;
  atMs: number;
}

/**
 * Cross-device rule engine. Reuses the shared hysteresis/debounce state
 * machine from @lorahome/protocol (see ARCHITECTURE.md §6) but keys state by
 * device+sensor rather than just sensor, since rules here may reference any
 * node in the network.
 */
export class GlobalRuleEngine {
  private rules: GlobalRule[] = [];
  private state = new Map<GlobalRule, RuleEvaluatorState>();

  setRules(rules: GlobalRule[]): void {
    this.rules = rules;
    this.state = new Map(rules.map((r) => [r, createRuleState()]));
  }

  /** Feeds a new sensor reading and returns any rules that fire as a result. */
  ingest(deviceId: number, sensorId: number, value: number, atMs = Date.now()): RuleFireEvent[] {
    const fired: RuleFireEvent[] = [];

    for (const rule of this.rules) {
      if (rule.src_device_id !== deviceId || rule.src_sensor_id !== sensorId) continue;
      const state = this.state.get(rule)!;
      if (stepRule(state, rule, value, atMs)) {
        fired.push({ rule, value, atMs });
      }
    }

    return fired;
  }
}
