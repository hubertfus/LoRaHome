#include "lorahome/protocol.h"
#include "lorahome/crc16.h"

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

int lorahome_encode_frame(
    const lorahome_header_t* header,
    const uint8_t* payload,
    size_t payload_len,
    uint8_t* out_buf,
    size_t out_buf_cap) {
  const size_t total_len = LORAHOME_HEADER_SIZE + payload_len + LORAHOME_CRC_SIZE;
  if (out_buf_cap < total_len) return -1;

  lorahome_encode_header(header, out_buf);
  if (payload_len > 0) {
    for (size_t i = 0; i < payload_len; i++) {
      out_buf[LORAHOME_HEADER_SIZE + i] = payload[i];
    }
  }

  const size_t body_len = LORAHOME_HEADER_SIZE + payload_len;
  const uint16_t crc = lorahome_crc16(out_buf, body_len);
  out_buf[body_len] = (uint8_t)(crc >> 8);
  out_buf[body_len + 1] = (uint8_t)(crc & 0xFF);

  return (int)total_len;
}

bool lorahome_decode_frame(
    const uint8_t* buf,
    size_t len,
    lorahome_header_t* out_header,
    const uint8_t** out_payload,
    size_t* out_payload_len) {
  if (len < LORAHOME_HEADER_SIZE + LORAHOME_CRC_SIZE) return false;

  const size_t body_len = len - LORAHOME_CRC_SIZE;
  const uint16_t expected_crc = (uint16_t)((buf[body_len] << 8) | buf[body_len + 1]);
  const uint16_t actual_crc = lorahome_crc16(buf, body_len);
  if (expected_crc != actual_crc) return false;

  if (!lorahome_decode_header(buf, body_len, out_header)) return false;

  *out_payload = buf + LORAHOME_HEADER_SIZE;
  *out_payload_len = body_len - LORAHOME_HEADER_SIZE;
  return true;
}
