import { encode } from 'cbor-x';
import { Rule, ruleToWireMap } from '@lorahome/protocol';

/**
 * Encodes a node's local rule table as CBOR with integer keys, ready to be
 * split into CONFIG_FRAG frames by the transport layer. The React Flow
 * graph -> Rule[] reduction lives in a separate step upstream of this
 * function; this is the one place JSON and CBOR meet (ARCHITECTURE.md §5).
 */
export function compileRulesToCbor(rules: Rule[]): Uint8Array {
  // Pass Maps straight through — cbor-x preserves numeric Map keys as CBOR
  // integer keys. Converting to a plain object first would coerce them to
  // strings, which is exactly the encoding this project forbids.
  const wireRules = rules.map((rule) => ruleToWireMap(rule));
  return encode(wireRules);
}
