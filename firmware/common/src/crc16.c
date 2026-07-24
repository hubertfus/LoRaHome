#include "lorahome/crc16.h"

/*
 * CRC-16/CCITT-FALSE via a 16-entry nibble table.
 *
 * Why a nibble table and not the usual 256-entry byte table: the byte table
 * costs 512 B of .rodata to save roughly a third of the remaining time on a
 * 230 B frame. At our frame rate that time is free — the radio spends ~390 ms
 * on air per frame — while 512 B of flash is not, and .rodata competes with the
 * static buffers the zero-allocation design depends on. Half a table, most of
 * the speed, 32 B.
 *
 * The table holds the CRC contribution of each 4-bit input value, generated
 * from the polynomial rather than pasted from the internet, so the parameter
 * set is auditable at a glance.
 */

/* poly 0x1021, most-significant nibble first. */
static const uint16_t LH_CRC16_NIBBLE_TABLE[16] = {
    0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50A5u, 0x60C6u, 0x70E7u,
    0x8108u, 0x9129u, 0xA14Au, 0xB16Bu, 0xC18Cu, 0xD1ADu, 0xE1CEu, 0xF1EFu,
};

uint16_t lorahome_crc16(const uint8_t* data, size_t len) {
  uint16_t crc = LORAHOME_CRC16_INIT;

  for (size_t i = 0; i < len; i++) {
    /* High nibble, then low nibble; four bits of shift per lookup. */
    uint8_t index = (uint8_t)(((crc >> 12) ^ (data[i] >> 4)) & 0x0Fu);
    crc = (uint16_t)((crc << 4) ^ LH_CRC16_NIBBLE_TABLE[index]);

    index = (uint8_t)(((crc >> 12) ^ (data[i] & 0x0Fu)) & 0x0Fu);
    crc = (uint16_t)((crc << 4) ^ LH_CRC16_NIBBLE_TABLE[index]);
  }

  return crc;
}
