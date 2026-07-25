#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lorahome/bridge_stat.h"
#include "lorahome/protocol.h"
#include "lorahome/slip.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The Bridge's forwarding logic, with the hardware taken out. Roadmap T1.4.
 *
 * Everything the bridge decides — is this frame well formed, may it go on air,
 * which counter does a rejection belong to — lives here as plain C. The UART,
 * the radio and the clock reach it through function pointers. That is not
 * abstraction for its own sake: it is the difference between a forwarding path
 * verified by a thousand round trips in a test, and one verified by holding two
 * boards and hoping.
 *
 * Zero copy throughout. A frame decoded out of the serial buffer is handed to
 * the radio as a pointer into that same buffer; nothing is copied on the way to
 * the air.
 */

/** Largest SLIP-decoded frame accepted from the Host. */
#define LH_BRIDGE_RX_SERIAL_BUF 256u

/** Largest frame accepted from the radio, before SLIP encoding. */
#define LH_BRIDGE_RADIO_BUF 256u

/**
 * Serial transmit buffer.
 *
 * Derived from LH_SLIP_ENCODED_MAX rather than written down, which makes it 514
 * and not the 512 the planning sketch carried. Two bytes short is exactly
 * enough to fail on a maximum-length frame whose every byte needs escaping —
 * risk R1.1, arriving precisely as described. Deriving it means the number
 * cannot be wrong.
 */
#define LH_BRIDGE_TX_SERIAL_BUF (LH_SLIP_ENCODED_MAX(LH_BRIDGE_RADIO_BUF))

/** Why a frame was refused. Each maps to its own counter. */
typedef enum {
  LH_BRIDGE_ACCEPT = 0,
  LH_BRIDGE_REJECT_LEN = 1,   /* shorter than header+CRC, or longer than the MTU */
  LH_BRIDGE_REJECT_MAGIC = 2, /* not one of our frames at all */
  LH_BRIDGE_REJECT_CRC = 3,   /* ours, but corrupted */
} lh_bridge_verdict_t;

/**
 * Counters.
 *
 * Split by cause because in the field the three rejections mean entirely
 * different things: a length rejection is a misconfigured sender, a magic
 * rejection is somebody else's traffic on our sync word, and a CRC rejection is
 * a marginal RF link. One combined "dropped" counter would answer none of those.
 */
typedef struct {
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
  /** Diagnostic requests answered locally rather than forwarded. */
  uint32_t local_requests;
} lh_bridge_stats_t;

/** Hands a frame to the radio, or to the serial port. False means it did not go. */
typedef bool (*lh_bridge_emit_fn)(void *user, const uint8_t *data, uint16_t len);

/**
 * Asks whether `len` bytes may legally go on air right now.
 *
 * Kept as a callback rather than built in because answering it needs a clock
 * and a rolling window, and this file deliberately has neither. NULL means
 * "always allowed", which is what the tests use.
 */
typedef bool (*lh_bridge_allow_tx_fn)(void *user, uint16_t len);

/**
 * Fills in the platform half of a health readout: heap figures and uptime.
 *
 * A callback rather than a direct call to `esp_get_free_heap_size()`, because
 * this file has no platform and the tests would not have one either. The core
 * fills in the counters it owns; this supplies the rest.
 */
typedef void (*lh_bridge_health_fn)(void *user, lh_bridge_stat_t *out);

typedef struct {
  uint8_t serial_rx[LH_BRIDGE_RX_SERIAL_BUF];
  uint8_t serial_tx[LH_BRIDGE_TX_SERIAL_BUF];
  lh_slip_decoder_t slip_dec;

  lh_bridge_emit_fn to_radio;
  void *to_radio_user;
  lh_bridge_emit_fn to_host;
  void *to_host_user;
  lh_bridge_allow_tx_fn allow_tx;
  void *allow_tx_user;
  lh_bridge_health_fn read_health;
  void *read_health_user;

  lh_bridge_stats_t stats;
} lh_bridge_ctx_t;

/**
 * Validates a frame without copying it: length, magic byte, then CRC over
 * header and payload.
 *
 * Checked in that order so the counters mean what they say — a two-byte
 * fragment is a length problem, not a CRC problem, even though its CRC would
 * also fail.
 */
lh_bridge_verdict_t lh_bridge_validate(const uint8_t *frame, uint16_t len);

void lh_bridge_init(lh_bridge_ctx_t *ctx, lh_bridge_emit_fn to_radio, void *to_radio_user,
                    lh_bridge_emit_fn to_host, void *to_host_user);

/** Optional duty-cycle veto. Without it, every well-formed frame is transmitted. */
void lh_bridge_set_duty_cycle_guard(lh_bridge_ctx_t *ctx, lh_bridge_allow_tx_fn allow_tx,
                                    void *user);

/**
 * Enables the diagnostic reply.
 *
 * Without it, a BRIDGE_STAT_REQ is still answered — with the forwarding
 * counters and zeroes for the heap. That is deliberate: a Host reading zero
 * free heap will notice, where a Host receiving no reply at all cannot tell a
 * silent bridge from an unconfigured one.
 */
void lh_bridge_set_health_source(lh_bridge_ctx_t *ctx, lh_bridge_health_fn read_health, void *user);

/**
 * Fills a health readout from the core's own counters plus the health source.
 *
 * Exposed so the firmware can log it locally without going through a frame.
 */
void lh_bridge_collect_stat(const lh_bridge_ctx_t *ctx, lh_bridge_stat_t *out);

/**
 * Host → radio. Feed whatever came off the UART; frames are dispatched as they
 * complete.
 *
 * Validation before transmission is not optional. A malformed frame costs the
 * same second of airtime as a good one and the same slice of a duty cycle
 * budget that permits roughly one full frame every two minutes. Spending that
 * on a frame already known to be broken is the most expensive mistake this
 * component can make.
 */
void lh_bridge_feed_serial(lh_bridge_ctx_t *ctx, const uint8_t *bytes, uint16_t len);

/** Radio → host. Validated, then SLIP-encoded into `serial_tx` and emitted. */
void lh_bridge_on_radio_frame(lh_bridge_ctx_t *ctx, const uint8_t *frame, uint16_t len);

#ifdef __cplusplus
}
#endif
