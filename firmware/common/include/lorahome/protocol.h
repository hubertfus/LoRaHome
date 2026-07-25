#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 8-byte application-layer header + CBOR payload + CRC16. See ARCHITECTURE.md §3.1. */

#define LORAHOME_MAGIC_VER 0x4B
#define LORAHOME_HEADER_SIZE 8
#define LORAHOME_CRC_SIZE 2
#define LORAHOME_BROADCAST_ID 0xFFFF

/**
 * Addresses the Bridge itself. A frame carrying this destination is answered
 * locally and never transmitted. 0x0000 is reserved and is not a valid node id
 * — a magic address inside the normal range would eventually collide with a
 * real device and put diagnostics on the air.
 */
#define LORAHOME_BRIDGE_ID 0x0000

typedef enum {
  LORAHOME_FRAME_BEACON = 0x01,
  LORAHOME_FRAME_JOIN_REQ = 0x02,
  LORAHOME_FRAME_JOIN_ACK = 0x03,
  LORAHOME_FRAME_CONFIG_BEGIN = 0x10,
  LORAHOME_FRAME_CONFIG_FRAG = 0x11,
  LORAHOME_FRAME_CONFIG_COMMIT = 0x12,
  LORAHOME_FRAME_CONFIG_ACK = 0x13,
  LORAHOME_FRAME_TELEMETRY = 0x20,
  LORAHOME_FRAME_EVENT = 0x21,
  LORAHOME_FRAME_CMD = 0x30,
  LORAHOME_FRAME_CMD_ACK = 0x31,
  LORAHOME_FRAME_CAPABILITY_REQ = 0x40,
  LORAHOME_FRAME_CAPABILITY_RSP = 0x41,
  /* Bridge diagnostics — answered locally, never put on air. See bridge_stat.h. */
  LORAHOME_FRAME_BRIDGE_STAT_REQ = 0x50,
  LORAHOME_FRAME_BRIDGE_STAT_RSP = 0x51,
} lorahome_frame_type_t;

typedef enum {
  LORAHOME_FLAG_NONE = 0,
  LORAHOME_FLAG_ACK_REQ = 1 << 0,
  LORAHOME_FLAG_FRAG = 1 << 1,
  LORAHOME_FLAG_LAST = 1 << 2,
  LORAHOME_FLAG_ENCR = 1 << 3,
} lorahome_frame_flags_t;

typedef struct {
  uint8_t type; /* lorahome_frame_type_t */
  uint16_t src_id;
  uint16_t dst_id;
  uint8_t seq;
  uint8_t flags; /* bitmask of lorahome_frame_flags_t */
} lorahome_header_t;

/** Encodes the 8-byte header (big-endian multi-byte fields) into out[0..7]. */
void lorahome_encode_header(const lorahome_header_t* header, uint8_t out[LORAHOME_HEADER_SIZE]);

/** Decodes a header from buf (must be >= LORAHOME_HEADER_SIZE bytes). Returns false on bad magic byte. */
bool lorahome_decode_header(const uint8_t* buf, size_t len, lorahome_header_t* out);

/**
 * Encodes header + payload + trailing CRC16 into out_buf. Returns the total
 * number of bytes written, or -1 if out_buf_cap is too small.
 */
int lorahome_encode_frame(
    const lorahome_header_t* header,
    const uint8_t* payload,
    size_t payload_len,
    uint8_t* out_buf,
    size_t out_buf_cap);

/**
 * Decodes and CRC-validates a full frame. On success, *out_header is filled
 * in and *out_payload / *out_payload_len point into buf (no copy). Returns
 * false on a too-short buffer, bad magic byte, or CRC mismatch.
 */
bool lorahome_decode_frame(
    const uint8_t* buf,
    size_t len,
    lorahome_header_t* out_header,
    const uint8_t** out_payload,
    size_t* out_payload_len);

#ifdef __cplusplus
}
#endif
