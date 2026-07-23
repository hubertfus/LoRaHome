/**
 * Field-name <-> CBOR integer-key mapping. This is the single source of
 * truth: `scripts/generate-field-map-header.ts` (see package README) turns
 * this table into firmware/common/protocol/field_map.h. Never hand-edit the
 * generated header — edit this file and regenerate.
 */

export const RULE_FIELD_MAP = {
  src_sensor_id: 1,
  op: 2,
  threshold: 3,
  hysteresis: 4,
  debounce_ms: 5,
  action_id: 6,
  action_param: 7,
} as const;

export type RuleFieldName = keyof typeof RULE_FIELD_MAP;

export enum RuleOp {
  GT = 0,
  LT = 1,
  GTE = 2,
  LTE = 3,
  EQ = 4,
  NEQ = 5,
}

export interface Rule {
  src_sensor_id: number;
  op: RuleOp;
  threshold: number;
  hysteresis: number;
  debounce_ms: number;
  action_id: number;
  action_param: number;
}

/** Encodes a Rule as an integer-keyed map, ready for CBOR encoding. */
export function ruleToWireMap(rule: Rule): Map<number, number> {
  const map = new Map<number, number>();
  for (const [name, key] of Object.entries(RULE_FIELD_MAP) as [RuleFieldName, number][]) {
    map.set(key, rule[name]);
  }
  return map;
}

export function ruleFromWireMap(map: Map<number, number>): Rule {
  const byKey = new Map<number, RuleFieldName>(
    Object.entries(RULE_FIELD_MAP).map(([name, key]) => [key, name as RuleFieldName]),
  );
  const out = {} as Rule;
  for (const [key, value] of map) {
    const name = byKey.get(key);
    if (!name) continue; // unknown key: forward-compat, ignore rather than throw
    (out as Record<RuleFieldName, number>)[name] = value;
  }
  return out;
}
