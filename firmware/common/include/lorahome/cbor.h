#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The CBOR subset this project puts on the air. Roadmap T3.5, reused by Etap 4.
 *
 * Deliberately not a CBOR library. It handles unsigned integers, negative
 * integers, byte strings, arrays and maps in definite-length form, and nothing
 * else — no tags, no floats, no indefinite lengths, no text strings. Every one
 * of those absences is a decision:
 *
 *   - Floats, because readings are integer-scaled (R3.3) and a float on the
 *     wire is four to eight bytes buying nothing.
 *   - Text strings, because CONTRIBUTING.md §3 forbids string keys outright.
 *     There is no encoder for them here, so a PR cannot quietly add one.
 *   - Indefinite lengths, because a decoder that accepts them has to handle a
 *     stream that never terminates, on a device with a 230 B MTU where the
 *     length is always known before the write.
 *
 * The roadmap chose CBOR with integer keys over protobuf for one reason: 1 byte
 * of overhead per field against protobuf's 2 to 4. At a 230 B MTU that is
 * roughly 15% of the payload back, which is two or three more rules per
 * fragment. This file is where that saving is actually taken, and keeping it
 * this small is what makes the choice affordable in flash.
 *
 * Nothing here allocates. The writer fills a caller-owned buffer and records
 * overflow rather than truncating silently; the reader never walks past the
 * length it was given.
 */

/** Major types, in the top three bits of the initial byte. */
#define LH_CBOR_MAJOR_UINT 0u
#define LH_CBOR_MAJOR_NINT 1u
#define LH_CBOR_MAJOR_BYTES 2u
#define LH_CBOR_MAJOR_ARRAY 4u
#define LH_CBOR_MAJOR_MAP 5u

typedef struct {
  uint8_t *buf;
  uint16_t cap;
  uint16_t len;
  /**
   * Set once anything did not fit, and never cleared.
   *
   * Checked at the end rather than at every call: a caller writing twelve
   * fields would otherwise need twelve error branches, and the twelfth is the
   * one nobody writes. A sticky flag means one check at the end is exactly as
   * safe as twelve in the middle, and the writer stops emitting once it is set
   * so the partial buffer is never mistaken for a short message.
   */
  bool overflow;
} lh_cbor_writer_t;

typedef struct {
  const uint8_t *buf;
  uint16_t len;
  uint16_t pos;
  /** Set on the first malformed or truncated item. Same reasoning as above. */
  bool error;
} lh_cbor_reader_t;

void lh_cbor_writer_init(lh_cbor_writer_t *writer, uint8_t *buf, uint16_t cap);

/** Bytes written, or a negative value if anything overflowed. */
int lh_cbor_writer_finish(const lh_cbor_writer_t *writer);

bool lh_cbor_write_uint(lh_cbor_writer_t *writer, uint64_t value);
bool lh_cbor_write_int(lh_cbor_writer_t *writer, int64_t value);
bool lh_cbor_write_bytes(lh_cbor_writer_t *writer, const uint8_t *bytes, uint16_t len);
bool lh_cbor_write_array(lh_cbor_writer_t *writer, uint32_t items);
bool lh_cbor_write_map(lh_cbor_writer_t *writer, uint32_t pairs);

void lh_cbor_reader_init(lh_cbor_reader_t *reader, const uint8_t *buf, uint16_t len);

bool lh_cbor_read_uint(lh_cbor_reader_t *reader, uint64_t *out);
bool lh_cbor_read_int(lh_cbor_reader_t *reader, int64_t *out);
bool lh_cbor_read_array(lh_cbor_reader_t *reader, uint32_t *items);
bool lh_cbor_read_map(lh_cbor_reader_t *reader, uint32_t *pairs);

/**
 * Skips one item, whatever it is, including a nested array or map.
 *
 * The forward-compatibility hook. A node running older firmware will meet keys
 * it does not know, and the alternative to skipping them is refusing the whole
 * message — which would make every protocol addition a flag day across every
 * device in a building. Bounded by nesting depth so a crafted message cannot
 * recurse the stack away.
 */
bool lh_cbor_skip(lh_cbor_reader_t *reader);

/** True once the reader has consumed everything it was given. */
bool lh_cbor_reader_done(const lh_cbor_reader_t *reader);

#ifdef __cplusplus
}
#endif
