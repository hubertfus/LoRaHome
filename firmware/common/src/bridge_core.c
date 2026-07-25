#include "lorahome/bridge_core.h"

#include <string.h>

#include "lorahome/crc16.h"

/*
 * Roadmap Etap 1 budget: the whole bridge context under 2560 B.
 *
 * Deviation worth naming. The planning sketch put `lh_ring_t uart_ring` inside
 * this struct and then asserted 2560 B, which cannot both be true — the ring
 * alone is 2060 B and the frame buffers another 770. The ring belongs to
 * SerialLink, which owns the UART; this context owns the *protocol* state. With
 * that split the assertion is satisfiable and still means something, which a
 * budget nobody can meet does not.
 *
 * There is also no radio_tx buffer here, on purpose. A frame decoded out of
 * serial_rx goes to the radio as a pointer into serial_rx. Copying it to a
 * second 256 B buffer would cost the RAM and buy nothing.
 */
_Static_assert(sizeof(lh_bridge_ctx_t) < 2560, "bridge ctx budget breach");

/* R1.1 again, from the other direction: if the encode buffer ever stops being
 * able to hold the worst case, that is a build failure and not a field one. */
_Static_assert(LH_BRIDGE_TX_SERIAL_BUF >= LH_SLIP_ENCODED_MAX(LH_BRIDGE_RADIO_BUF),
               "serial TX buffer cannot hold a fully escaped radio frame");

lh_bridge_verdict_t lh_bridge_validate(const uint8_t *frame, uint16_t len) {
  if (len < LORAHOME_HEADER_SIZE + LORAHOME_CRC_SIZE) return LH_BRIDGE_REJECT_LEN;
  if (len > LH_BRIDGE_RADIO_BUF) return LH_BRIDGE_REJECT_LEN;

  /* Magic before CRC so the counters stay diagnostic. Somebody else's traffic
   * would fail both checks, and calling that a CRC error would send whoever is
   * reading the counters after an RF problem that does not exist. */
  if (frame[0] != LORAHOME_MAGIC_VER) return LH_BRIDGE_REJECT_MAGIC;

  const uint16_t body_len = (uint16_t)(len - LORAHOME_CRC_SIZE);
  const uint16_t received = (uint16_t)((frame[body_len] << 8) | frame[body_len + 1]);
  if (received != lorahome_crc16(frame, body_len)) return LH_BRIDGE_REJECT_CRC;

  return LH_BRIDGE_ACCEPT;
}

void lh_bridge_init(lh_bridge_ctx_t *ctx, lh_bridge_emit_fn to_radio, void *to_radio_user,
                    lh_bridge_emit_fn to_host, void *to_host_user) {
  memset(&ctx->stats, 0, sizeof ctx->stats);
  ctx->to_radio = to_radio;
  ctx->to_radio_user = to_radio_user;
  ctx->to_host = to_host;
  ctx->to_host_user = to_host_user;
  ctx->allow_tx = NULL;
  ctx->allow_tx_user = NULL;
  lh_slip_init(&ctx->slip_dec, ctx->serial_rx, (uint16_t)sizeof ctx->serial_rx);
}

void lh_bridge_set_duty_cycle_guard(lh_bridge_ctx_t *ctx, lh_bridge_allow_tx_fn allow_tx,
                                    void *user) {
  ctx->allow_tx = allow_tx;
  ctx->allow_tx_user = user;
}

/** Counts a rejection against the reason it was rejected for. */
static void count_rejection(lh_bridge_stats_t *stats, lh_bridge_verdict_t verdict) {
  switch (verdict) {
    case LH_BRIDGE_REJECT_LEN:
      stats->rejected_len++;
      break;
    case LH_BRIDGE_REJECT_MAGIC:
      stats->rejected_magic++;
      break;
    case LH_BRIDGE_REJECT_CRC:
      stats->rejected_crc++;
      break;
    case LH_BRIDGE_ACCEPT:
    default:
      break;
  }
}

/** One completed frame from the Host, already SLIP-decoded. */
static void forward_to_radio(lh_bridge_ctx_t *ctx, const uint8_t *frame, uint16_t len) {
  ctx->stats.serial_frames_in++;

  const lh_bridge_verdict_t verdict = lh_bridge_validate(frame, len);
  if (verdict != LH_BRIDGE_ACCEPT) {
    count_rejection(&ctx->stats, verdict);
    return;
  }

  /* Asked after validation, not before: a broken frame should not consume the
   * duty cycle budget even notionally, and the guard's own counters should
   * reflect real traffic. */
  if (ctx->allow_tx != NULL && !ctx->allow_tx(ctx->allow_tx_user, len)) {
    ctx->stats.rejected_duty_cycle++;
    return;
  }

  if (ctx->to_radio == NULL || !ctx->to_radio(ctx->to_radio_user, frame, len)) {
    ctx->stats.radio_tx_errors++;
    return;
  }

  ctx->stats.radio_frames_out++;
}

void lh_bridge_feed_serial(lh_bridge_ctx_t *ctx, const uint8_t *bytes, uint16_t len) {
  for (uint16_t i = 0; i < len; i++) {
    /* The frame is only valid until the next feed, so it is dealt with inside
     * the loop. LH_SLIP_ERROR needs no branch: the decoder has counted the
     * damaged frame and resynchronised itself. */
    if (lh_slip_feed(&ctx->slip_dec, bytes[i]) == LH_SLIP_FRAME_READY) {
      forward_to_radio(ctx, ctx->slip_dec.buf, ctx->slip_dec.len);
    }
  }
}

void lh_bridge_on_radio_frame(lh_bridge_ctx_t *ctx, const uint8_t *frame, uint16_t len) {
  ctx->stats.radio_frames_in++;

  /* Validated in this direction too. The Host has its own checks, but a bridge
   * that forwards corruption upstream turns one bad RF moment into a puzzle
   * somewhere far less able to explain it. */
  const lh_bridge_verdict_t verdict = lh_bridge_validate(frame, len);
  if (verdict != LH_BRIDGE_ACCEPT) {
    count_rejection(&ctx->stats, verdict);
    return;
  }

  const uint16_t encoded =
      lh_slip_encode(frame, len, ctx->serial_tx, (uint16_t)sizeof ctx->serial_tx);
  if (encoded == 0) {
    ctx->stats.serial_tx_errors++;
    return;
  }

  if (ctx->to_host == NULL || !ctx->to_host(ctx->to_host_user, ctx->serial_tx, encoded)) {
    ctx->stats.serial_tx_errors++;
    return;
  }

  ctx->stats.serial_frames_out++;
}
