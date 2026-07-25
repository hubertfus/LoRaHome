#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The Bridge's health readout, as it travels to the Host.
 *
 * The Bridge is the one component in this system whose memory nobody can see.
 * It has no display, no network stack, and its serial port carries frames
 * rather than a console — so a slow leak in it is invisible until the device
 * stops answering some weeks later. This payload is how the 24-hour soak reads
 * `heap_free` and `heap_largest_block` off it, and those are the numbers the
 * roadmap makes a release depend on:
 *
 * > `heap_free.end - heap_free.start` is the most important number in the whole
 * > project. If the drift is non-zero we have a leak and the release must not
 * > be tagged.
 *
 * `heap_largest_block` is carried alongside deliberately. 180 kB free in
 * fragments too small to hold a 4 kB buffer is a device that dies in week
 * three, and the free total alone cannot tell you that.
 *
 * **Not CBOR.** The Bridge does not interpret CBOR anywhere else
 * (ARCHITECTURE.md §7) and this is not the place to introduce a decoder to it.
 * Fixed offsets, big-endian, the same byte order as the frame header.
 */

/** Bumped whenever a field moves. The Host refuses a version it does not know. */
#define LH_BRIDGE_STAT_VERSION 1u

/** Wire size of the payload. Well inside LH_MAX_PAYLOAD (220). */
#define LH_BRIDGE_STAT_SIZE 82u

typedef struct {
  uint32_t uptime_ms;

  /* §0.4 memory probe, the three numbers that matter over a long run. */
  uint32_t heap_free_internal;
  uint32_t heap_largest_block;
  uint32_t heap_min_free_ever;

  /* Forwarding counters — lh_bridge_stats_t, flattened. */
  uint32_t serial_frames_in;
  uint32_t serial_frames_out;
  uint32_t radio_frames_in;
  uint32_t radio_frames_out;
  uint32_t rejected_crc;
  uint32_t rejected_magic;
  uint32_t rejected_len;
  uint32_t rejected_duty_cycle;
  uint32_t radio_tx_errors;
  uint32_t serial_tx_errors;

  /* SLIP framing health on the serial link. */
  uint32_t slip_frames_ok;
  uint32_t slip_overflow;
  uint32_t slip_bad_escape;
  uint32_t slip_dropped;

  /* UART ring. `ring_hwm` is the early-warning number, not a curiosity. */
  uint32_t ring_overrun;
  uint16_t ring_hwm;
  uint16_t ring_capacity;
} lh_bridge_stat_t;

/**
 * Serialises into `out`. Returns bytes written, or 0 if `cap` is too small —
 * nothing is written in that case.
 */
uint16_t lh_bridge_stat_encode(const lh_bridge_stat_t *stat, uint8_t *out, uint16_t cap);

/**
 * Parses a payload. Returns 1 on success, 0 if the buffer is the wrong length
 * or carries an unknown version.
 *
 * Present mainly so the encoder has something to be tested against on the host;
 * the Bridge itself never decodes one.
 */
int lh_bridge_stat_decode(const uint8_t *in, uint16_t len, lh_bridge_stat_t *out);

#ifdef __cplusplus
}
#endif
