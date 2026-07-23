#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SLIP framing (RFC 1055) used on the Bridge's Serial link to the Host.
 * See ARCHITECTURE.md §7.
 */

#define LORAHOME_SLIP_END 0xC0
#define LORAHOME_SLIP_ESC 0xDB
#define LORAHOME_SLIP_ESC_END 0xDC
#define LORAHOME_SLIP_ESC_ESC 0xDD

/**
 * Encodes `data` as a SLIP frame (leading + trailing 0xC0, with 0xC0/0xDB
 * bytes escaped) into out_buf. Returns bytes written, or -1 if out_buf_cap
 * is too small. Worst case output size is 2*len + 2.
 */
int lorahome_slip_encode(const uint8_t* data, size_t len, uint8_t* out_buf, size_t out_buf_cap);

/** Streaming SLIP decoder: feed one byte at a time as it arrives off the wire. */
typedef struct {
  uint8_t* buf;
  size_t cap;
  size_t len;
  bool in_escape;
} lorahome_slip_decoder_t;

void lorahome_slip_decoder_init(lorahome_slip_decoder_t* dec, uint8_t* buf, size_t cap);

/**
 * Feeds one incoming byte. Returns true when `byte` completed a frame
 * (0xC0 received with a non-empty buffer) — the decoded frame is then
 * available at dec->buf[0..dec->len). The buffer is left intact so the
 * caller can read it; call lorahome_slip_decoder_reset() before feeding the
 * next frame's bytes. Silently drops bytes that would overflow `cap` rather
 * than corrupt memory (the resulting frame will fail its CRC check).
 */
bool lorahome_slip_decoder_feed(lorahome_slip_decoder_t* dec, uint8_t byte);

/** Must be called after consuming a completed frame, before feeding the next one. */
void lorahome_slip_decoder_reset(lorahome_slip_decoder_t* dec);

#ifdef __cplusplus
}
#endif
