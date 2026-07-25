/*
 * Host-native harness for cross-language verification of the bridge stat
 * payload.
 *
 * Reads 21 unsigned decimal values per line — the struct's fields in wire order
 * — and writes the encoded payload as hex.
 *
 * Same shape as crc16_cli.c and slip_cli.c, for the same reason: the TypeScript
 * side owns the value grid and pipes it in, so the only thing under test is
 * lh_bridge_stat_encode(). This one matters because the payload it produces
 * carries the heap figures a release decision rests on — a silent disagreement
 * about field order would show up as a plausible-looking wrong number, which is
 * the worst possible failure for a leak detector.
 */
#include <stdio.h>

#include "lorahome/bridge_stat.h"

int main(void) {
  lh_bridge_stat_t stat;
  unsigned long v[21];

  for (;;) {
    int read = 0;
    for (int i = 0; i < 21; i++) {
      if (scanf("%lu", &v[i]) != 1) break;
      read++;
    }
    if (read == 0) break;
    if (read != 21) {
      fprintf(stderr, "short line: %d of 21 values\n", read);
      return 1;
    }

    stat.uptime_ms = (uint32_t)v[0];
    stat.heap_free_internal = (uint32_t)v[1];
    stat.heap_largest_block = (uint32_t)v[2];
    stat.heap_min_free_ever = (uint32_t)v[3];
    stat.serial_frames_in = (uint32_t)v[4];
    stat.serial_frames_out = (uint32_t)v[5];
    stat.radio_frames_in = (uint32_t)v[6];
    stat.radio_frames_out = (uint32_t)v[7];
    stat.rejected_crc = (uint32_t)v[8];
    stat.rejected_magic = (uint32_t)v[9];
    stat.rejected_len = (uint32_t)v[10];
    stat.rejected_duty_cycle = (uint32_t)v[11];
    stat.radio_tx_errors = (uint32_t)v[12];
    stat.serial_tx_errors = (uint32_t)v[13];
    stat.slip_frames_ok = (uint32_t)v[14];
    stat.slip_overflow = (uint32_t)v[15];
    stat.slip_bad_escape = (uint32_t)v[16];
    stat.slip_dropped = (uint32_t)v[17];
    stat.ring_overrun = (uint32_t)v[18];
    stat.ring_hwm = (uint16_t)v[19];
    stat.ring_capacity = (uint16_t)v[20];

    uint8_t out[LH_BRIDGE_STAT_SIZE];
    const uint16_t written = lh_bridge_stat_encode(&stat, out, (uint16_t)sizeof out);
    if (written == 0) {
      fprintf(stderr, "encode refused a full-size buffer\n");
      return 1;
    }

    for (uint16_t i = 0; i < written; i++) printf("%02X", out[i]);
    printf("\n");
  }

  return 0;
}
