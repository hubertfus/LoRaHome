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

/*
 * Validation is lh_frame_parse's job as of T2.2; this maps its verdict onto the
 * Bridge's counters.
 *
 * The check order (length, magic, CRC) and the reason for it now live in
 * protocol.c. What is decided here is what the Bridge does about each answer,
 * and there is one deliberate difference from a Node: an unknown frame type is
 * forwarded, not refused. The Bridge is a relay — it must not become the reason
 * a frame type added next year cannot reach a device that understands it.
 *
 * The length ceiling moved with this change, from the 256 B receive buffer to
 * the 230 B MTU. The two limits had quietly disagreed since Etap 1: the Host
 * enforced the MTU and the Bridge enforced its buffer, so a 240 B frame was
 * relayed onto the air and then rejected at the far end, spending the airtime
 * to learn nothing. One limit, and it is the one the radio profile can carry.
 */
lh_bridge_verdict_t lh_bridge_validate(const uint8_t *frame, uint16_t len) {
  lh_frame_view_t view;

  switch (lh_frame_parse(frame, len, &view)) {
    case LH_OK:
    case LH_ERR_BAD_TYPE:
      return LH_BRIDGE_ACCEPT;
    case LH_ERR_BAD_MAGIC:
      return LH_BRIDGE_REJECT_MAGIC;
    case LH_ERR_BAD_CRC:
      return LH_BRIDGE_REJECT_CRC;
    case LH_ERR_TOO_SHORT:
    case LH_ERR_TOO_LONG:
    default:
      return LH_BRIDGE_REJECT_LEN;
  }
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
  ctx->read_health = NULL;
  ctx->read_health_user = NULL;
  lh_slip_init(&ctx->slip_dec, ctx->serial_rx, (uint16_t)sizeof ctx->serial_rx);
}

void lh_bridge_set_duty_cycle_guard(lh_bridge_ctx_t *ctx, lh_bridge_allow_tx_fn allow_tx,
                                    void *user) {
  ctx->allow_tx = allow_tx;
  ctx->allow_tx_user = user;
}

void lh_bridge_set_health_source(lh_bridge_ctx_t *ctx, lh_bridge_health_fn read_health,
                                 void *user) {
  ctx->read_health = read_health;
  ctx->read_health_user = user;
}

void lh_bridge_collect_stat(const lh_bridge_ctx_t *ctx, lh_bridge_stat_t *out) {
  memset(out, 0, sizeof *out);

  /* Platform first, so the counters below cannot be clobbered by a health
   * source that zeroes the struct it is handed. */
  if (ctx->read_health != NULL) ctx->read_health(ctx->read_health_user, out);

  out->serial_frames_in = ctx->stats.serial_frames_in;
  out->serial_frames_out = ctx->stats.serial_frames_out;
  out->radio_frames_in = ctx->stats.radio_frames_in;
  out->radio_frames_out = ctx->stats.radio_frames_out;
  out->rejected_crc = ctx->stats.rejected_crc;
  out->rejected_magic = ctx->stats.rejected_magic;
  out->rejected_len = ctx->stats.rejected_len;
  out->rejected_duty_cycle = ctx->stats.rejected_duty_cycle;
  out->radio_tx_errors = ctx->stats.radio_tx_errors;
  out->serial_tx_errors = ctx->stats.serial_tx_errors;

  out->slip_frames_ok = ctx->slip_dec.stat_frames_ok;
  out->slip_overflow = ctx->slip_dec.stat_overflow;
  out->slip_bad_escape = ctx->slip_dec.stat_bad_escape;
  out->slip_dropped = ctx->slip_dec.stat_dropped;
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

/** SLIP-encodes a frame into the TX buffer and hands it to the Host. */
static void emit_to_host(lh_bridge_ctx_t *ctx, const uint8_t *frame, uint16_t len) {
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

/**
 * Answers a diagnostic request instead of forwarding it.
 *
 * Built into `serial_tx` after the payload is assembled on the stack. 82 B of
 * stack in a task that already has a 3 kB one is cheaper than another static
 * buffer, and this path runs at most once every few seconds.
 */
static void answer_stat_request(lh_bridge_ctx_t *ctx, const lorahome_header_t *request) {
  ctx->stats.local_requests++;

  lh_bridge_stat_t stat;
  lh_bridge_collect_stat(ctx, &stat);

  uint8_t payload[LH_BRIDGE_STAT_SIZE];
  const uint16_t payload_len = lh_bridge_stat_encode(&stat, payload, (uint16_t)sizeof payload);
  if (payload_len == 0) {
    ctx->stats.serial_tx_errors++;
    return;
  }

  const lorahome_header_t header = {
      (uint8_t)LORAHOME_FRAME_BRIDGE_STAT_RSP,
      LORAHOME_BRIDGE_ID,
      /* Back to whoever asked, and echoing the sequence number so a Host with
       * more than one request outstanding can tell the replies apart. */
      request->src_id,
      request->seq,
      LORAHOME_FLAG_NONE,
  };

  uint8_t frame[LORAHOME_HEADER_SIZE + LH_BRIDGE_STAT_SIZE + LORAHOME_CRC_SIZE];
  const int frame_len =
      lorahome_encode_frame(&header, payload, payload_len, frame, sizeof frame);
  if (frame_len <= 0) {
    ctx->stats.serial_tx_errors++;
    return;
  }

  emit_to_host(ctx, frame, (uint16_t)frame_len);
}

/** One completed frame from the Host, already SLIP-decoded. */
static void forward_to_radio(lh_bridge_ctx_t *ctx, const uint8_t *frame, uint16_t len) {
  ctx->stats.serial_frames_in++;

  const lh_bridge_verdict_t verdict = lh_bridge_validate(frame, len);
  if (verdict != LH_BRIDGE_ACCEPT) {
    count_rejection(&ctx->stats, verdict);
    return;
  }

  /* Addressed to the Bridge itself: answered here, never transmitted. Checked
   * after validation so a corrupt frame cannot trigger a reply, and before the
   * duty cycle guard because a local answer spends no airtime. */
  lorahome_header_t header;
  if (lorahome_decode_header(frame, len, &header) &&
      header.dst_id == LORAHOME_BRIDGE_ID &&
      header.type == (uint8_t)LORAHOME_FRAME_BRIDGE_STAT_REQ) {
    answer_stat_request(ctx, &header);
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

  emit_to_host(ctx, frame, len);
}
