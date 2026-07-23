#include "lorahome/slip.h"

int lorahome_slip_encode(const uint8_t* data, size_t len, uint8_t* out_buf, size_t out_buf_cap) {
  size_t out = 0;

  if (out >= out_buf_cap) return -1;
  out_buf[out++] = LORAHOME_SLIP_END;

  for (size_t i = 0; i < len; i++) {
    const uint8_t byte = data[i];
    if (byte == LORAHOME_SLIP_END) {
      if (out + 2 > out_buf_cap) return -1;
      out_buf[out++] = LORAHOME_SLIP_ESC;
      out_buf[out++] = LORAHOME_SLIP_ESC_END;
    } else if (byte == LORAHOME_SLIP_ESC) {
      if (out + 2 > out_buf_cap) return -1;
      out_buf[out++] = LORAHOME_SLIP_ESC;
      out_buf[out++] = LORAHOME_SLIP_ESC_ESC;
    } else {
      if (out + 1 > out_buf_cap) return -1;
      out_buf[out++] = byte;
    }
  }

  if (out + 1 > out_buf_cap) return -1;
  out_buf[out++] = LORAHOME_SLIP_END;

  return (int)out;
}

void lorahome_slip_decoder_init(lorahome_slip_decoder_t* dec, uint8_t* buf, size_t cap) {
  dec->buf = buf;
  dec->cap = cap;
  dec->len = 0;
  dec->in_escape = false;
}

void lorahome_slip_decoder_reset(lorahome_slip_decoder_t* dec) {
  dec->len = 0;
  dec->in_escape = false;
}

bool lorahome_slip_decoder_feed(lorahome_slip_decoder_t* dec, uint8_t byte) {
  if (byte == LORAHOME_SLIP_END) {
    if (dec->len > 0) {
      /* Frame complete. Left intact for the caller — see lorahome_slip_decoder_reset(). */
      return true;
    }
    /* Leading/duplicate END byte: nothing accumulated yet, nothing to do. */
    return false;
  }

  if (dec->in_escape) {
    dec->in_escape = false;
    if (byte == LORAHOME_SLIP_ESC_END) {
      byte = LORAHOME_SLIP_END;
    } else if (byte == LORAHOME_SLIP_ESC_ESC) {
      byte = LORAHOME_SLIP_ESC;
    }
    /* else: malformed escape sequence — pass the byte through as-is. */
  } else if (byte == LORAHOME_SLIP_ESC) {
    dec->in_escape = true;
    return false;
  }

  if (dec->len < dec->cap) {
    dec->buf[dec->len++] = byte;
  }
  /* else: frame too large for the static buffer — drop the byte rather than overflow. */

  return false;
}
