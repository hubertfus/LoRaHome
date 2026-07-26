#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fragmentation and reassembly of config payloads. Roadmap T2.3.
 *
 * A rule set of any interest runs past 220 B, and 220 B is all one LoRa frame
 * carries. So a config is split, transmitted as several frames, and put back
 * together on the far side — which introduces the failure this file mostly
 * exists to prevent: fragments from two different transactions assembled into
 * one config that is internally consistent, structurally valid, and wrong.
 *
 * That is not theoretical. With retransmissions in flight and a 16-bit
 * transaction id that eventually wraps, a late fragment from transaction N can
 * arrive during transaction N+1 and land in the same buffer slot. Two
 * independent defences: `cfg_id` is checked on every fragment, and `crc_total`
 * — a CRC over the whole assembled config, carried in every fragment header —
 * is verified after assembly. The per-frame CRC cannot catch this; it protects
 * transport, and each of those fragments was transmitted perfectly.
 *
 * Allocation: none. The reassembly buffer is a member of the struct, the struct
 * lives in .bss, and the whole path is measured to move 0 bytes of heap.
 */

/** Fragments per config: 8 * 200 B = 1600 B, the largest config we accept. */
#define LH_FRAG_MAX_FRAGMENTS 8u

/**
 * Payload bytes per fragment.
 *
 * 200 rather than the 212 that would fit (220 payload - 8 header). The 12 bytes
 * of slack are deliberate: a fragment header that grows by a field must not
 * force every stored config to be re-fragmented, and 1600 is a number a human
 * can hold in their head when reasoning about the RAM budget.
 */
#define LH_FRAG_PAYLOAD_MAX 200u

/** Largest config that can be sent in one transaction. */
#define LH_FRAG_CONFIG_MAX (LH_FRAG_MAX_FRAGMENTS * LH_FRAG_PAYLOAD_MAX)

/** On-wire size of the fragment header that precedes each slice. */
#define LH_FRAG_HDR_SIZE 8u

/**
 * How long an incomplete transaction holds the slot. Roadmap R2.4.
 *
 * Measured against a real clock passed in by the caller, never against a loop
 * counter: a loop that stalls stops expiring the transaction, and one lost
 * fragment would then block configuration until the device is power-cycled.
 */
#define LH_FRAG_TIMEOUT_MS 30000

/** Fragment header, host-order. Encoded big-endian on the wire. */
typedef struct {
  uint16_t cfg_id;     /* transaction id                                  */
  uint8_t frag_index;  /* 0 .. frag_total-1                               */
  uint8_t frag_total;  /* fragments in this transaction                   */
  uint16_t total_len;  /* assembled length, repeated in every fragment    */
  uint16_t crc_total;  /* CRC of the assembled config — see file comment  */
} lh_frag_hdr_t;

/**
 * Encodes the header big-endian into `out[0..7]`.
 *
 * The roadmap sketched this as a `__attribute__((packed))` struct written
 * straight to the wire. That would be little-endian on both the ESP32 and the
 * host — agreeing with itself, and disagreeing with the TypeScript twin and
 * with every other multi-byte field in this protocol, which are big-endian.
 * Risk R0.2 in its purest form. Explicit accessors cost a few instructions and
 * remove the question.
 */
void lh_frag_hdr_encode(const lh_frag_hdr_t *hdr, uint8_t *out);

/** Decodes a header from `in[0..7]`. */
void lh_frag_hdr_decode(const uint8_t *in, lh_frag_hdr_t *out);

/** Fragments a config of `total_len` bytes needs. 0 if it is too large. */
uint8_t lh_frag_count(uint16_t total_len);

/**
 * Builds fragment `index` of `config` into `out`: header, then the slice.
 *
 * Returns bytes written, or a negative `lh_err_t`-shaped value: -5
 * (LH_ERR_TOO_LONG) if the config exceeds LH_FRAG_CONFIG_MAX or the index is
 * past the last fragment, -1 (LH_ERR_TOO_SHORT) if `out_cap` is too small.
 *
 * `crc_total` is computed over the whole config on every call. That is
 * deliberate redundancy — the alternative, computing it once and passing it in,
 * puts the caller in charge of the one value that guards against assembling
 * two transactions into one.
 */
int lh_frag_build(uint16_t cfg_id, uint8_t index, const uint8_t *config, uint16_t total_len,
                  uint8_t *out, uint16_t out_cap);

/** Outcome of feeding one fragment. */
typedef enum {
  LH_FRAG_NEED_MORE = 0,      /* stored; waiting for the rest              */
  LH_FRAG_COMPLETE = 1,       /* assembled and CRC-verified in `buf`       */
  LH_FRAG_DUPLICATE = 2,      /* already had this one; nothing changed     */
  LH_FRAG_ERR_HEADER = -1,    /* malformed header or impossible geometry   */
  LH_FRAG_ERR_TOO_LARGE = -2, /* config larger than the buffer             */
  LH_FRAG_ERR_FOREIGN = -3,   /* another transaction is in progress        */
  LH_FRAG_ERR_CRC = -4,       /* assembled, and the whole-config CRC failed */
} lh_frag_result_t;

typedef struct {
  uint8_t buf[LH_FRAG_CONFIG_MAX];
  uint16_t cfg_id;
  uint8_t received_mask; /* bit N: fragment N stored                     */
  uint8_t expected_total;
  uint16_t total_len;
  uint16_t crc_expected;
  int64_t first_frag_us; /* start of the timeout window                  */
  bool active;
  uint32_t stat_completed;
  uint32_t stat_timeouts;
  uint32_t stat_crc_fail;
  uint32_t stat_duplicates;
  uint32_t stat_foreign;
} lh_reassembler_t;

void lh_frag_reset(lh_reassembler_t *reasm);

/**
 * Feeds one fragment payload (header + slice), as carried by a CONFIG_FRAG frame.
 *
 * On LH_FRAG_COMPLETE the config occupies `reasm->buf[0 .. reasm->total_len)`
 * and the slot is released, so the next fragment starts a new transaction. On
 * LH_FRAG_ERR_CRC the slot is released too: an assembly that fails its CRC is
 * not recoverable by waiting, and holding the slot would block the retry.
 *
 * `now_us` is supplied by the caller for the same reason as everywhere else in
 * firmware/common — this file has no platform. It is also what makes the
 * timeout testable in microseconds instead of in half-minutes.
 */
lh_frag_result_t lh_frag_feed(lh_reassembler_t *reasm, const uint8_t *payload, uint16_t len,
                              int64_t now_us);

/**
 * Expires a transaction that has been incomplete for LH_FRAG_TIMEOUT_MS.
 *
 * Returns true if it just expired one. Call it from the main loop; without it a
 * transaction that lost a fragment holds the slot until something else forces
 * the issue, which is R2.4 — one lost fragment, configuration blocked for ever.
 */
bool lh_frag_tick(lh_reassembler_t *reasm, int64_t now_us);

#ifdef __cplusplus
}
#endif
