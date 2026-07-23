#pragma once

/**
 * CBOR integer keys for the rule_t wire format. Must stay in lockstep with
 * packages/protocol/src/field-map.ts (RULE_FIELD_MAP) — see CONTRIBUTING.md
 * §3. Do not hand-edit one side without the other.
 */

#define LORAHOME_FIELD_SRC_SENSOR_ID 1
#define LORAHOME_FIELD_OP 2
#define LORAHOME_FIELD_THRESHOLD 3
#define LORAHOME_FIELD_HYSTERESIS 4
#define LORAHOME_FIELD_DEBOUNCE_MS 5
#define LORAHOME_FIELD_ACTION_ID 6
#define LORAHOME_FIELD_ACTION_PARAM 7
