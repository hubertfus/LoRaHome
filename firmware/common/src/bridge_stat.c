#include "lorahome/bridge_stat.h"

/*
 * Field order is the struct's order, and the two must stay in step. The size
 * assertion below is the cheap half of enforcing that; the other half is
 * tools/check-bridge-stat.mjs, which runs a value grid through this encoder and
 * the TypeScript decoder and requires them to agree.
 *
 * Big-endian, matching the frame header. Mixing byte orders inside one protocol
 * is how risk R0.2 gets in through a side door.
 */

static uint16_t put_u32(uint8_t *out, uint16_t offset, uint32_t value) {
  out[offset] = (uint8_t)(value >> 24);
  out[offset + 1] = (uint8_t)(value >> 16);
  out[offset + 2] = (uint8_t)(value >> 8);
  out[offset + 3] = (uint8_t)value;
  return (uint16_t)(offset + 4);
}

static uint16_t put_u16(uint8_t *out, uint16_t offset, uint16_t value) {
  out[offset] = (uint8_t)(value >> 8);
  out[offset + 1] = (uint8_t)value;
  return (uint16_t)(offset + 2);
}

static uint32_t get_u32(const uint8_t *in, uint16_t offset) {
  return ((uint32_t)in[offset] << 24) | ((uint32_t)in[offset + 1] << 16) |
         ((uint32_t)in[offset + 2] << 8) | (uint32_t)in[offset + 3];
}

static uint16_t get_u16(const uint8_t *in, uint16_t offset) {
  return (uint16_t)(((uint16_t)in[offset] << 8) | (uint16_t)in[offset + 1]);
}

uint16_t lh_bridge_stat_encode(const lh_bridge_stat_t *stat, uint8_t *out, uint16_t cap) {
  if (cap < LH_BRIDGE_STAT_SIZE) return 0;

  uint16_t at = 0;
  at = put_u16(out, at, (uint16_t)LH_BRIDGE_STAT_VERSION);
  at = put_u32(out, at, stat->uptime_ms);

  at = put_u32(out, at, stat->heap_free_internal);
  at = put_u32(out, at, stat->heap_largest_block);
  at = put_u32(out, at, stat->heap_min_free_ever);

  at = put_u32(out, at, stat->serial_frames_in);
  at = put_u32(out, at, stat->serial_frames_out);
  at = put_u32(out, at, stat->radio_frames_in);
  at = put_u32(out, at, stat->radio_frames_out);
  at = put_u32(out, at, stat->rejected_crc);
  at = put_u32(out, at, stat->rejected_magic);
  at = put_u32(out, at, stat->rejected_len);
  at = put_u32(out, at, stat->rejected_duty_cycle);
  at = put_u32(out, at, stat->radio_tx_errors);
  at = put_u32(out, at, stat->serial_tx_errors);

  at = put_u32(out, at, stat->slip_frames_ok);
  at = put_u32(out, at, stat->slip_overflow);
  at = put_u32(out, at, stat->slip_bad_escape);
  at = put_u32(out, at, stat->slip_dropped);

  at = put_u32(out, at, stat->ring_overrun);
  at = put_u16(out, at, stat->ring_hwm);
  at = put_u16(out, at, stat->ring_capacity);

  /* Caught at build time rather than by a Host that silently mis-parses: the
   * declared size and the fields written must be the same number. */
  return at == LH_BRIDGE_STAT_SIZE ? at : 0;
}

int lh_bridge_stat_decode(const uint8_t *in, uint16_t len, lh_bridge_stat_t *out) {
  if (len != LH_BRIDGE_STAT_SIZE) return 0;
  if (get_u16(in, 0) != (uint16_t)LH_BRIDGE_STAT_VERSION) return 0;

  uint16_t at = 2;
  out->uptime_ms = get_u32(in, at);
  at = (uint16_t)(at + 4);

  out->heap_free_internal = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->heap_largest_block = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->heap_min_free_ever = get_u32(in, at);
  at = (uint16_t)(at + 4);

  out->serial_frames_in = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->serial_frames_out = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->radio_frames_in = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->radio_frames_out = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->rejected_crc = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->rejected_magic = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->rejected_len = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->rejected_duty_cycle = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->radio_tx_errors = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->serial_tx_errors = get_u32(in, at);
  at = (uint16_t)(at + 4);

  out->slip_frames_ok = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->slip_overflow = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->slip_bad_escape = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->slip_dropped = get_u32(in, at);
  at = (uint16_t)(at + 4);

  out->ring_overrun = get_u32(in, at);
  at = (uint16_t)(at + 4);
  out->ring_hwm = get_u16(in, at);
  at = (uint16_t)(at + 2);
  out->ring_capacity = get_u16(in, at);
  at = (uint16_t)(at + 2);

  return at == LH_BRIDGE_STAT_SIZE ? 1 : 0;
}
