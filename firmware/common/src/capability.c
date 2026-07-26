/*
 * Capability report encoding. Roadmap T3.5. Contract in capability.h.
 *
 * Thin by design: the CBOR subset in cbor.c does the encoding, and this file is
 * only the shape of the message and the bounds on what will be accepted back.
 */
#include "lorahome/capability.h"

#include <string.h>

#include "lorahome/cbor.h"
#include "lorahome/protocol.h"

/*
 * The 6-byte layout is a protocol contract, not an implementation detail — the
 * roadmap states it and the host mirrors it. A uint16_t next to four bytes is
 * exactly the arrangement a target's alignment rules can turn into 8, which is
 * why this is asserted on every ABI the compile sweep reaches rather than on
 * the one that happened to run the tests.
 */
_Static_assert(sizeof(lh_capability_t) == 6, "capability drift");
_Static_assert(LH_CAP_MAX == 8, "a report holds one entry per component");

/*
 * The requirement the wire budget is really about: a discovery response must
 * never fragment. Fragmenting it would make discovery depend on reassembly,
 * which depends on an ARQ, and all of that is what you would be debugging when
 * a new node fails to appear.
 *
 * Asserted against the worst case rather than the typical one. A bound that
 * only holds for the reports the encoder happens to produce is not a bound on
 * what a receiver has to survive.
 */
_Static_assert(
    LH_CAP_WIRE_WORST_CASE <= LORAHOME_MAX_PAYLOAD,
    "a capability report must fit one frame without fragmentation");
_Static_assert(LH_CAP_WIRE_BUDGET <= LH_CAP_WIRE_WORST_CASE, "the tighter target is the tighter one");

int lh_cap_encode(const lh_cap_report_t *report, uint8_t *out, uint16_t cap) {
  if (report == NULL || report->count > LH_CAP_MAX) return -1;

  lh_cbor_writer_t writer;
  lh_cbor_writer_init(&writer, out, cap);

  lh_cbor_write_map(&writer, 3);

  lh_cbor_write_uint(&writer, LH_CAP_KEY_FW_VERSION);
  lh_cbor_write_uint(&writer, report->fw_version);

  lh_cbor_write_uint(&writer, LH_CAP_KEY_FREE_HEAP_KB);
  lh_cbor_write_uint(&writer, report->free_heap_kb);

  lh_cbor_write_uint(&writer, LH_CAP_KEY_COMPONENTS);
  lh_cbor_write_array(&writer, report->count);

  for (uint8_t i = 0; i < report->count; i++) {
    const lh_capability_t *entry = &report->caps[i];
    lh_cbor_write_array(&writer, LH_CAP_FIELDS);
    lh_cbor_write_uint(&writer, entry->driver_type_id);
    lh_cbor_write_uint(&writer, entry->bus_addr);
    lh_cbor_write_uint(&writer, entry->bus_type);
    lh_cbor_write_uint(&writer, entry->channel_count);
    lh_cbor_write_uint(&writer, entry->flags);
  }

  /* One check at the end. The writer stops emitting once it has overflowed, so
   * a partial buffer can never be mistaken for a short message. */
  return lh_cbor_writer_finish(&writer);
}

/** Reads one component array, tolerating extra trailing fields. */
static bool decode_entry(lh_cbor_reader_t *reader, lh_capability_t *out) {
  uint32_t fields = 0;
  if (!lh_cbor_read_array(reader, &fields)) return false;
  if (fields < LH_CAP_FIELDS) return false;

  uint64_t values[LH_CAP_FIELDS];
  for (uint8_t i = 0; i < LH_CAP_FIELDS; i++) {
    if (!lh_cbor_read_uint(reader, &values[i])) return false;
  }
  /* A newer node may describe a component with more fields than this build
   * knows about. Skipping them is what lets an old host still discover it,
   * rather than reporting a device it can see as unreadable. */
  for (uint32_t i = LH_CAP_FIELDS; i < fields; i++) {
    if (!lh_cbor_skip(reader)) return false;
  }

  if (values[LH_CAP_FIELD_TYPE_ID] > UINT16_MAX) return false;
  if (values[LH_CAP_FIELD_BUS_ADDR] > UINT8_MAX) return false;
  if (values[LH_CAP_FIELD_BUS_TYPE] > UINT8_MAX) return false;
  if (values[LH_CAP_FIELD_CHANNELS] > UINT8_MAX) return false;
  if (values[LH_CAP_FIELD_FLAGS] > UINT8_MAX) return false;

  out->driver_type_id = (uint16_t)values[LH_CAP_FIELD_TYPE_ID];
  out->bus_addr = (uint8_t)values[LH_CAP_FIELD_BUS_ADDR];
  out->bus_type = (uint8_t)values[LH_CAP_FIELD_BUS_TYPE];
  out->channel_count = (uint8_t)values[LH_CAP_FIELD_CHANNELS];
  out->flags = (uint8_t)values[LH_CAP_FIELD_FLAGS];
  return true;
}

bool lh_cap_decode(const uint8_t *buf, uint16_t len, lh_cap_report_t *out) {
  if (out == NULL) return false;
  memset(out, 0, sizeof *out);

  lh_cbor_reader_t reader;
  lh_cbor_reader_init(&reader, buf, len);

  uint32_t pairs = 0;
  if (!lh_cbor_read_map(&reader, &pairs)) return false;

  for (uint32_t pair = 0; pair < pairs; pair++) {
    uint64_t key = 0;
    if (!lh_cbor_read_uint(&reader, &key)) return false;

    switch (key) {
      case LH_CAP_KEY_FW_VERSION: {
        uint64_t value = 0;
        if (!lh_cbor_read_uint(&reader, &value)) return false;
        if (value > UINT32_MAX) return false;
        out->fw_version = (uint32_t)value;
        break;
      }
      case LH_CAP_KEY_FREE_HEAP_KB: {
        uint64_t value = 0;
        if (!lh_cbor_read_uint(&reader, &value)) return false;
        if (value > UINT16_MAX) return false;
        out->free_heap_kb = (uint16_t)value;
        break;
      }
      case LH_CAP_KEY_COMPONENTS: {
        uint32_t count = 0;
        if (!lh_cbor_read_array(&reader, &count)) return false;
        /* A node that reports more components than this build can hold is a
         * protocol mismatch, not something to silently truncate: a host acting
         * on the first eight of twelve would show a device with parts missing
         * and no indication that anything was dropped. */
        if (count > LH_CAP_MAX) return false;
        for (uint32_t i = 0; i < count; i++) {
          if (!decode_entry(&reader, &out->caps[i])) return false;
        }
        out->count = (uint8_t)count;
        break;
      }
      default:
        /* Forward compatibility: a key from a newer protocol is stepped over,
         * so adding one does not require flashing every node in a building on
         * the same day. */
        if (!lh_cbor_skip(&reader)) return false;
        break;
    }
  }

  return !reader.error;
}

bool lh_cap_equal(const lh_cap_report_t *a, const lh_cap_report_t *b) {
  if (a == NULL || b == NULL) return false;
  if (a->count != b->count) return false;
  if (a->fw_version != b->fw_version) return false;
  if (a->free_heap_kb != b->free_heap_kb) return false;
  /* Field by field rather than memcmp over the struct: padding bytes are
   * whatever the last write left there, and comparing them would make the
   * answer depend on how the report was built. */
  for (uint8_t i = 0; i < a->count; i++) {
    if (a->caps[i].driver_type_id != b->caps[i].driver_type_id) return false;
    if (a->caps[i].bus_addr != b->caps[i].bus_addr) return false;
    if (a->caps[i].bus_type != b->caps[i].bus_type) return false;
    if (a->caps[i].channel_count != b->caps[i].channel_count) return false;
    if (a->caps[i].flags != b->caps[i].flags) return false;
  }
  return true;
}
