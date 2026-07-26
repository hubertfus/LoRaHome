#include "lorahome/protocol.h"

#include <string.h>

#include "lorahome/crc16.h"

/* The MTU is a property of the radio profile, not a preference. If the payload
 * budget and the frame size ever stop agreeing, frames build fine and are
 * rejected on arrival — a failure that looks like a link fault. */
_Static_assert(LORAHOME_MAX_FRAME_SIZE == 230, "frame size drift vs the LoRa MTU");
_Static_assert(LORAHOME_MIN_FRAME_SIZE == 10, "minimum frame size drift");

uint16_t lorahome_frame_crc(const uint8_t* buf, uint16_t body_len) {
  return lorahome_crc16(buf, body_len);
}

bool lh_frame_type_is_known(uint8_t type) {
  switch ((lorahome_frame_type_t)type) {
    case LORAHOME_FRAME_BEACON:
    case LORAHOME_FRAME_JOIN_REQ:
    case LORAHOME_FRAME_JOIN_ACK:
    case LORAHOME_FRAME_CONFIG_BEGIN:
    case LORAHOME_FRAME_CONFIG_FRAG:
    case LORAHOME_FRAME_CONFIG_COMMIT:
    case LORAHOME_FRAME_CONFIG_ACK:
    case LORAHOME_FRAME_TELEMETRY:
    case LORAHOME_FRAME_EVENT:
    case LORAHOME_FRAME_CMD:
    case LORAHOME_FRAME_CMD_ACK:
    case LORAHOME_FRAME_CAPABILITY_REQ:
    case LORAHOME_FRAME_CAPABILITY_RSP:
    case LORAHOME_FRAME_BRIDGE_STAT_REQ:
    case LORAHOME_FRAME_BRIDGE_STAT_RSP:
      return true;
    default:
      return false;
  }
}

lh_err_t lh_frame_parse(const uint8_t* buf, uint16_t len, lh_frame_view_t* out) {
  memset(out, 0, sizeof *out);

  if (len < LORAHOME_MIN_FRAME_SIZE) return LH_ERR_TOO_SHORT;
  if (len > LORAHOME_MAX_FRAME_SIZE) return LH_ERR_TOO_LONG;
  if (buf[0] != LORAHOME_MAGIC_VER) return LH_ERR_BAD_MAGIC;

  const uint16_t body_len = (uint16_t)(len - LORAHOME_CRC_SIZE);
  out->crc_rx = (uint16_t)((buf[body_len] << 8) | buf[body_len + 1]);
  out->crc_calc = lorahome_frame_crc(buf, body_len);
  if (out->crc_rx != out->crc_calc) return LH_ERR_BAD_CRC;

  /* Cannot fail: the length and magic byte were checked above, which is all
   * lorahome_decode_header inspects. The result is still tested rather than
   * discarded, so a future change to that function cannot silently leave a
   * half-filled header behind. */
  if (!lorahome_decode_header(buf, body_len, &out->hdr)) return LH_ERR_BAD_MAGIC;

  out->payload = buf + LORAHOME_HEADER_SIZE;
  out->payload_len = (uint16_t)(body_len - LORAHOME_HEADER_SIZE);

  /* Last, and with the view already complete. An unknown type is a routing
   * decision the caller makes — the Bridge forwards it, a Node ignores it —
   * and neither can decide anything about a frame it has not been given. */
  if (!lh_frame_type_is_known(out->hdr.type)) return LH_ERR_BAD_TYPE;

  return LH_OK;
}

int lh_frame_build(
    const lorahome_header_t* header,
    const uint8_t* payload,
    uint16_t payload_len,
    uint8_t* out,
    uint16_t out_cap) {
  if (payload_len > LORAHOME_MAX_PAYLOAD) return LH_ERR_TOO_LONG;

  const uint16_t total_len =
      (uint16_t)(LORAHOME_HEADER_SIZE + payload_len + LORAHOME_CRC_SIZE);
  if (out_cap < total_len) return LH_ERR_TOO_SHORT;

  lorahome_encode_header(header, out);
  if (payload_len > 0) memcpy(out + LORAHOME_HEADER_SIZE, payload, payload_len);

  const uint16_t body_len = (uint16_t)(LORAHOME_HEADER_SIZE + payload_len);
  const uint16_t crc = lorahome_frame_crc(out, body_len);
  out[body_len] = (uint8_t)(crc >> 8);
  out[body_len + 1] = (uint8_t)(crc & 0xFF);

  return (int)total_len;
}

void lorahome_encode_header(const lorahome_header_t* header, uint8_t out[LORAHOME_HEADER_SIZE]) {
  out[0] = LORAHOME_MAGIC_VER;
  out[1] = header->type;
  out[2] = (uint8_t)(header->src_id >> 8);
  out[3] = (uint8_t)(header->src_id & 0xFF);
  out[4] = (uint8_t)(header->dst_id >> 8);
  out[5] = (uint8_t)(header->dst_id & 0xFF);
  out[6] = header->seq;
  out[7] = header->flags;
}

bool lorahome_decode_header(const uint8_t* buf, size_t len, lorahome_header_t* out) {
  if (len < LORAHOME_HEADER_SIZE) return false;
  if (buf[0] != LORAHOME_MAGIC_VER) return false;

  out->type = buf[1];
  out->src_id = (uint16_t)((buf[2] << 8) | buf[3]);
  out->dst_id = (uint16_t)((buf[4] << 8) | buf[5]);
  out->seq = buf[6];
  out->flags = buf[7];
  return true;
}

/*
 * The two boolean entry points below are now wrappers.
 *
 * They existed before there was an error taxonomy, and half the codebase calls
 * them. Reimplementing them on top of lh_frame_build/lh_frame_parse leaves one
 * definition of what a frame is — one length policy, one CRC span, one magic
 * check — instead of two that agree today and drift in six months. What they
 * lose is the reason for a refusal, which is exactly why new code should call
 * the typed pair.
 */

int lorahome_encode_frame(
    const lorahome_header_t* header,
    const uint8_t* payload,
    size_t payload_len,
    uint8_t* out_buf,
    size_t out_buf_cap) {
  if (payload_len > LORAHOME_MAX_PAYLOAD) return -1;
  if (out_buf_cap > UINT16_MAX) out_buf_cap = UINT16_MAX;

  const int written =
      lh_frame_build(header, payload, (uint16_t)payload_len, out_buf, (uint16_t)out_buf_cap);
  return written < 0 ? -1 : written;
}

bool lorahome_decode_frame(
    const uint8_t* buf,
    size_t len,
    lorahome_header_t* out_header,
    const uint8_t** out_payload,
    size_t* out_payload_len) {
  if (len > UINT16_MAX) return false;

  lh_frame_view_t view;
  const lh_err_t err = lh_frame_parse(buf, (uint16_t)len, &view);

  /* An unknown type still decodes. Whether to act on it is a routing question,
   * and this function's contract has always been "is this frame intact". */
  if (err != LH_OK && err != LH_ERR_BAD_TYPE) return false;

  *out_header = view.hdr;
  *out_payload = view.payload;
  *out_payload_len = view.payload_len;
  return true;
}
