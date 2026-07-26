/*
 * Minimal CBOR writer and reader. Roadmap T3.5. Contract in cbor.h.
 *
 * RFC 8949 §3: the initial byte carries a 3-bit major type and a 5-bit
 * argument. Values below 24 are the argument itself; 24, 25, 26 and 27 mean the
 * argument follows in 1, 2, 4 or 8 big-endian bytes. That is the whole encoding
 * for everything this project puts on the air, and the reason the wire format
 * costs one byte per small field.
 */
#include "lorahome/cbor.h"

#include <string.h>

/** Nesting the skipper will descend before refusing. */
#define SKIP_MAX_DEPTH 8u

/* ------------------------------------------------------------------------- */
/* Writer                                                                    */
/* ------------------------------------------------------------------------- */

void lh_cbor_writer_init(lh_cbor_writer_t *writer, uint8_t *buf, uint16_t cap) {
  if (writer == NULL) return;
  writer->buf = buf;
  writer->cap = (buf == NULL) ? 0u : cap;
  writer->len = 0;
  writer->overflow = (buf == NULL);
}

int lh_cbor_writer_finish(const lh_cbor_writer_t *writer) {
  if (writer == NULL || writer->overflow) return -1;
  return (int)writer->len;
}

static bool put(lh_cbor_writer_t *writer, uint8_t byte) {
  if (writer->overflow) return false;
  if (writer->len >= writer->cap) {
    writer->overflow = true;
    return false;
  }
  writer->buf[writer->len++] = byte;
  return true;
}

/**
 * Writes a major type and its argument in the shortest form that fits.
 *
 * Shortest form is not an optimisation here, it is the contract. Two encoders
 * that both produce valid CBOR but choose different widths for the same number
 * produce different bytes, and the cross-language check compares bytes — which
 * is the point of having one. Canonical encoding is what makes "C and
 * TypeScript agree" a testable claim rather than a hopeful one.
 */
static bool write_head(lh_cbor_writer_t *writer, uint8_t major, uint64_t argument) {
  const uint8_t prefix = (uint8_t)(major << 5);

  if (argument < 24u) return put(writer, (uint8_t)(prefix | (uint8_t)argument));

  if (argument <= 0xFFu) {
    return put(writer, (uint8_t)(prefix | 24u)) && put(writer, (uint8_t)argument);
  }
  if (argument <= 0xFFFFu) {
    return put(writer, (uint8_t)(prefix | 25u)) && put(writer, (uint8_t)(argument >> 8)) &&
           put(writer, (uint8_t)argument);
  }
  if (argument <= 0xFFFFFFFFu) {
    return put(writer, (uint8_t)(prefix | 26u)) && put(writer, (uint8_t)(argument >> 24)) &&
           put(writer, (uint8_t)(argument >> 16)) && put(writer, (uint8_t)(argument >> 8)) &&
           put(writer, (uint8_t)argument);
  }

  if (!put(writer, (uint8_t)(prefix | 27u))) return false;
  for (int shift = 56; shift >= 0; shift -= 8) {
    if (!put(writer, (uint8_t)(argument >> shift))) return false;
  }
  return true;
}

bool lh_cbor_write_uint(lh_cbor_writer_t *writer, uint64_t value) {
  return writer != NULL && write_head(writer, LH_CBOR_MAJOR_UINT, value);
}

bool lh_cbor_write_int(lh_cbor_writer_t *writer, int64_t value) {
  if (writer == NULL) return false;
  if (value >= 0) return write_head(writer, LH_CBOR_MAJOR_UINT, (uint64_t)value);

  /* Negative integers encode -1 - n, so -1 is argument 0. Computed on the
   * unsigned value to keep INT64_MIN from overflowing on negation — the one
   * input where the obvious `-value - 1` is undefined behaviour. */
  const uint64_t magnitude = ~(uint64_t)value;
  return write_head(writer, LH_CBOR_MAJOR_NINT, magnitude);
}

bool lh_cbor_write_bytes(lh_cbor_writer_t *writer, const uint8_t *bytes, uint16_t len) {
  if (writer == NULL) return false;
  if (!write_head(writer, LH_CBOR_MAJOR_BYTES, len)) return false;
  if (len == 0) return true;
  if (bytes == NULL) {
    writer->overflow = true;
    return false;
  }
  if ((uint32_t)writer->len + len > writer->cap) {
    writer->overflow = true;
    return false;
  }
  memcpy(&writer->buf[writer->len], bytes, len);
  writer->len = (uint16_t)(writer->len + len);
  return true;
}

bool lh_cbor_write_array(lh_cbor_writer_t *writer, uint32_t items) {
  return writer != NULL && write_head(writer, LH_CBOR_MAJOR_ARRAY, items);
}

bool lh_cbor_write_map(lh_cbor_writer_t *writer, uint32_t pairs) {
  return writer != NULL && write_head(writer, LH_CBOR_MAJOR_MAP, pairs);
}

/* ------------------------------------------------------------------------- */
/* Reader                                                                    */
/* ------------------------------------------------------------------------- */

void lh_cbor_reader_init(lh_cbor_reader_t *reader, const uint8_t *buf, uint16_t len) {
  if (reader == NULL) return;
  reader->buf = buf;
  reader->len = (buf == NULL) ? 0u : len;
  reader->pos = 0;
  reader->error = (buf == NULL);
}

bool lh_cbor_reader_done(const lh_cbor_reader_t *reader) {
  return reader != NULL && !reader->error && reader->pos >= reader->len;
}

/** Reads one initial byte and its argument. Sets `error` on truncation. */
static bool read_head(lh_cbor_reader_t *reader, uint8_t *major, uint64_t *argument) {
  if (reader->error || reader->pos >= reader->len) {
    reader->error = true;
    return false;
  }

  const uint8_t initial = reader->buf[reader->pos++];
  *major = (uint8_t)(initial >> 5);
  const uint8_t info = (uint8_t)(initial & 0x1Fu);

  if (info < 24u) {
    *argument = info;
    return true;
  }

  uint8_t extra;
  switch (info) {
    case 24u:
      extra = 1;
      break;
    case 25u:
      extra = 2;
      break;
    case 26u:
      extra = 4;
      break;
    case 27u:
      extra = 8;
      break;
    default:
      /* 28..30 are reserved and 31 is the indefinite-length marker. Both are
       * refused rather than skipped: an indefinite-length item is a stream with
       * no stated end, and accepting one on a device with a fixed receive
       * buffer means trusting a sender to terminate it. */
      reader->error = true;
      return false;
  }

  if ((uint32_t)reader->pos + extra > reader->len) {
    reader->error = true;
    return false;
  }

  uint64_t value = 0;
  for (uint8_t i = 0; i < extra; i++) {
    value = (value << 8) | reader->buf[reader->pos++];
  }
  *argument = value;
  return true;
}

/** Reads a head and insists on the major type the caller expected. */
static bool read_typed(lh_cbor_reader_t *reader, uint8_t expected, uint64_t *argument) {
  if (reader == NULL) return false;
  uint8_t major;
  if (!read_head(reader, &major, argument)) return false;
  if (major != expected) {
    /* Rewinding is pointless: a message whose shape does not match is not going
     * to match on a second attempt, and leaving the reader parked on the
     * offending item is what makes a failure diagnosable. */
    reader->error = true;
    return false;
  }
  return true;
}

bool lh_cbor_read_uint(lh_cbor_reader_t *reader, uint64_t *out) {
  uint64_t argument;
  if (!read_typed(reader, LH_CBOR_MAJOR_UINT, &argument)) return false;
  if (out != NULL) *out = argument;
  return true;
}

bool lh_cbor_read_int(lh_cbor_reader_t *reader, int64_t *out) {
  if (reader == NULL) return false;
  uint8_t major;
  uint64_t argument;
  if (!read_head(reader, &major, &argument)) return false;

  if (major == LH_CBOR_MAJOR_UINT) {
    if (argument > (uint64_t)INT64_MAX) {
      reader->error = true;
      return false;
    }
    if (out != NULL) *out = (int64_t)argument;
    return true;
  }
  if (major == LH_CBOR_MAJOR_NINT) {
    if (argument > (uint64_t)INT64_MAX) {
      reader->error = true;
      return false;
    }
    if (out != NULL) *out = (int64_t)(~argument);
    return true;
  }

  reader->error = true;
  return false;
}

bool lh_cbor_read_array(lh_cbor_reader_t *reader, uint32_t *items) {
  uint64_t argument;
  if (!read_typed(reader, LH_CBOR_MAJOR_ARRAY, &argument)) return false;
  /* A declared count larger than the bytes remaining cannot be honest — every
   * item costs at least one byte — and refusing it here stops a crafted header
   * from driving a caller's loop far past the buffer. */
  if (argument > (uint64_t)(reader->len - reader->pos)) {
    reader->error = true;
    return false;
  }
  if (items != NULL) *items = (uint32_t)argument;
  return true;
}

bool lh_cbor_read_map(lh_cbor_reader_t *reader, uint32_t *pairs) {
  uint64_t argument;
  if (!read_typed(reader, LH_CBOR_MAJOR_MAP, &argument)) return false;
  /* Two items per pair, so the same bound is twice as tight. */
  if (argument > (uint64_t)((reader->len - reader->pos) / 2u) + 1u) {
    reader->error = true;
    return false;
  }
  if (pairs != NULL) *pairs = (uint32_t)argument;
  return true;
}

bool lh_cbor_skip(lh_cbor_reader_t *reader) {
  if (reader == NULL) return false;

  /*
   * Iterative rather than recursive, with an explicit depth cap.
   *
   * The input is a radio frame from a device that may be faulty or hostile, and
   * a recursive skipper meeting eight hundred nested arrays walks the stack off
   * a task that has 3 kB of it. The pending counter is the whole state a
   * depth-first walk needs; the cap bounds how deep it is willing to go.
   */
  uint32_t pending = 1;
  uint32_t depth = 0;

  while (pending > 0) {
    uint8_t major;
    uint64_t argument;
    if (!read_head(reader, &major, &argument)) return false;
    pending--;

    switch (major) {
      case LH_CBOR_MAJOR_UINT:
      case LH_CBOR_MAJOR_NINT:
        break;

      case LH_CBOR_MAJOR_BYTES:
        if (argument > (uint64_t)(reader->len - reader->pos)) {
          reader->error = true;
          return false;
        }
        reader->pos = (uint16_t)(reader->pos + argument);
        break;

      case LH_CBOR_MAJOR_ARRAY:
      case LH_CBOR_MAJOR_MAP: {
        if (++depth > SKIP_MAX_DEPTH) {
          reader->error = true;
          return false;
        }
        const uint64_t items = (major == LH_CBOR_MAJOR_MAP) ? argument * 2u : argument;
        if (items > (uint64_t)(reader->len - reader->pos)) {
          reader->error = true;
          return false;
        }
        pending += (uint32_t)items;
        break;
      }

      default:
        /* Tags, floats and simple values: not in this subset, so a message
         * carrying one is not ours and is refused rather than guessed at. */
        reader->error = true;
        return false;
    }
  }

  return true;
}
