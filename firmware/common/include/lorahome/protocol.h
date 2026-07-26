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

/** Largest payload that fits one LoRa frame at the Etap 1 profile. */
#define LORAHOME_MAX_PAYLOAD 220

/** A frame carrying no payload is still header + CRC. Anything shorter is junk. */
#define LORAHOME_MIN_FRAME_SIZE (LORAHOME_HEADER_SIZE + LORAHOME_CRC_SIZE)

/** Header + full payload + CRC = the 230 B LoRa MTU. */
#define LORAHOME_MAX_FRAME_SIZE \
  (LORAHOME_HEADER_SIZE + LORAHOME_MAX_PAYLOAD + LORAHOME_CRC_SIZE)

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

/**
 * Why a frame was refused. Roadmap Etap 0 §"Architektura Danych"; wired into
 * the frame path in T2.2.
 *
 * A boolean answer was enough while the only caller was a test. It is not
 * enough for the reliability layer: an ARQ that cannot tell "corrupted on the
 * air" from "not addressed to us" will retransmit for the wrong reasons, and a
 * counter that merges the two answers no question anybody asks in the field.
 * Every value here maps to a different diagnosis and, on the Bridge, to a
 * different counter.
 */
typedef enum {
  LH_OK = 0,
  LH_ERR_TOO_SHORT = -1, /* shorter than header + CRC                        */
  LH_ERR_BAD_MAGIC = -2, /* not one of our frames at all                     */
  LH_ERR_BAD_CRC = -3,   /* ours, but corrupted in flight                    */
  LH_ERR_BAD_TYPE = -4,  /* structurally valid, type not in this build       */
  LH_ERR_TOO_LONG = -5,  /* longer than the MTU, or than the caller's buffer */
} lh_err_t;

/**
 * A parsed frame, without a copy.
 *
 * `payload` points into the caller's receive buffer and is valid exactly as
 * long as that buffer is — the zero-copy doctrine from ARCHITECTURE.md, which
 * is what lets a 230 B frame be parsed on a device with a 48 kB static RAM
 * budget without a scratch buffer.
 *
 * Both CRCs are recorded, not just the verdict. When a link starts failing, the
 * question is whether the corruption is random (the two differ unpredictably)
 * or structural — a length misread makes `crc_calc` land on the CRC of a
 * consistently wrong span, and seeing both numbers is what tells them apart.
 */
typedef struct {
  lorahome_header_t hdr;
  const uint8_t* payload; /* into the caller's buffer — never owned */
  uint16_t payload_len;
  uint16_t crc_rx;   /* the two trailing bytes, as received */
  uint16_t crc_calc; /* computed over header + payload      */
} lh_frame_view_t;

/** True for frame types this build knows how to route. */
bool lh_frame_type_is_known(uint8_t type);

/**
 * Parses and CRC-validates a frame in place. Never allocates, never copies.
 *
 * Checks run in the order length, magic, CRC, type, so each rejection names the
 * cheapest true cause: a two-byte fragment is a length problem even though its
 * CRC would also fail, and somebody else's traffic on our sync word is a magic
 * problem, not an RF one. Getting that order wrong sends whoever reads the
 * counters after a fault that does not exist.
 *
 * `*out` is filled as far as parsing got: on LH_ERR_BAD_CRC both CRCs are
 * populated, and on LH_ERR_BAD_TYPE the view is complete — an unknown type is
 * reported so a caller can count it, not so the frame becomes unreadable.
 */
lh_err_t lh_frame_parse(const uint8_t* buf, uint16_t len, lh_frame_view_t* out);

/**
 * Builds header + payload + CRC into a caller-owned buffer.
 *
 * Returns the number of bytes written, or a negative `lh_err_t`:
 * LH_ERR_TOO_LONG if the payload exceeds LORAHOME_MAX_PAYLOAD, LH_ERR_TOO_SHORT
 * if `out_cap` cannot hold the result. Nothing is written in either case.
 */
int lh_frame_build(
    const lorahome_header_t* header,
    const uint8_t* payload,
    uint16_t payload_len,
    uint8_t* out,
    uint16_t out_cap);

/**
 * CRC over a frame's header and payload — everything but the trailing two bytes.
 *
 * The single place the frame path's CRC span is defined. Everything that
 * validates a frame goes through this rather than recomputing `len - 2`, because
 * that expression written twice is exactly how a sender and a receiver end up
 * protecting different spans and blaming the radio.
 */
uint16_t lorahome_frame_crc(const uint8_t* buf, uint16_t body_len);

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
