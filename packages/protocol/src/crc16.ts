/**
 * CRC-16/CCITT-FALSE — twin of firmware/common/src/crc16.c.
 *
 * Parameters: poly 0x1021, init 0xFFFF, no input reflection, no output
 * reflection, no final XOR.
 *
 * These two implementations must agree byte for byte forever; that is the whole
 * point of the shared vector file in test/fixtures/crc16-vectors.json. Keep the
 * algorithms structurally identical (same nibble table, same nibble order) so a
 * reviewer can diff them by eye rather than by trust.
 */

/** CRC contribution of each 4-bit input value under poly 0x1021. */
const NIBBLE_TABLE = new Uint16Array([
  0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7, 0x8108, 0x9129, 0xa14a, 0xb16b,
  0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
]);

export const CRC16_INIT = 0xffff;

/**
 * Catalogue check value for CRC("123456789").
 *
 * The roadmap quotes 0xE5CC, which belongs to CRC-16/AUG-CCITT (init 0x1D0F).
 * With init=0xFFFF — which the roadmap also specifies, and which every golden
 * vector in this repo already uses — the value is 0x29B1.
 */
export const CRC16_CHECK = 0x29b1;

export function crc16(data: Uint8Array): number {
  let crc = CRC16_INIT;

  for (let i = 0; i < data.length; i++) {
    const byte = data[i]!;

    // High nibble, then low nibble: four bits of shift per table lookup.
    let index = ((crc >>> 12) ^ (byte >>> 4)) & 0x0f;
    crc = ((crc << 4) ^ NIBBLE_TABLE[index]!) & 0xffff;

    index = ((crc >>> 12) ^ (byte & 0x0f)) & 0x0f;
    crc = ((crc << 4) ^ NIBBLE_TABLE[index]!) & 0xffff;
  }

  return crc;
}

/**
 * Reference bit-by-bit implementation, kept as a cross-check on the table.
 *
 * Not used on any hot path. It exists so the table-driven version above is
 * verified against the polynomial definition itself rather than against a table
 * someone once copied from a web page — if the two ever disagree, the table is
 * wrong, and the tests say so.
 */
export function crc16Reference(data: Uint8Array): number {
  let crc = CRC16_INIT;

  for (let i = 0; i < data.length; i++) {
    crc ^= data[i]! << 8;
    for (let bit = 0; bit < 8; bit++) {
      crc = (crc & 0x8000) !== 0 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }

  return crc;
}
