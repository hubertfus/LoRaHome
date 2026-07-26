#include <unity.h>
#include <cstring>
#include "lorahome/protocol.h"

void setUp(void) {}
void tearDown(void) {}

void test_encode_decode_frame_round_trip(void) {
  lorahome_header_t header = {LORAHOME_FRAME_TELEMETRY, 42, 1, 7, LORAHOME_FLAG_ACK_REQ};
  const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};

  uint8_t wire[64];
  int wire_len = lorahome_encode_frame(&header, payload, sizeof(payload), wire, sizeof(wire));
  TEST_ASSERT_GREATER_THAN(0, wire_len);

  lorahome_header_t decoded;
  const uint8_t* out_payload;
  size_t out_payload_len;
  TEST_ASSERT_TRUE(lorahome_decode_frame(wire, (size_t)wire_len, &decoded, &out_payload, &out_payload_len));

  TEST_ASSERT_EQUAL_UINT8(LORAHOME_FRAME_TELEMETRY, decoded.type);
  TEST_ASSERT_EQUAL_UINT16(42, decoded.src_id);
  TEST_ASSERT_EQUAL_UINT16(1, decoded.dst_id);
  TEST_ASSERT_EQUAL_UINT8(7, decoded.seq);
  TEST_ASSERT_EQUAL_UINT8(LORAHOME_FLAG_ACK_REQ, decoded.flags);
  TEST_ASSERT_EQUAL_UINT32(sizeof(payload), out_payload_len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, out_payload, sizeof(payload));
}

void test_decode_frame_rejects_corrupted_crc(void) {
  lorahome_header_t header = {LORAHOME_FRAME_BEACON, 1, LORAHOME_BROADCAST_ID, 0, LORAHOME_FLAG_NONE};
  uint8_t wire[16];
  int wire_len = lorahome_encode_frame(&header, nullptr, 0, wire, sizeof(wire));
  TEST_ASSERT_GREATER_THAN(0, wire_len);

  wire[wire_len - 1] ^= 0xFF; // flip a CRC byte

  lorahome_header_t decoded;
  const uint8_t* out_payload;
  size_t out_payload_len;
  TEST_ASSERT_FALSE(lorahome_decode_frame(wire, (size_t)wire_len, &decoded, &out_payload, &out_payload_len));
}

// Cross-language parity check against packages/protocol/test/fixtures/beacon.json —
// same header, same expected wire bytes, computed independently in TypeScript.
void test_matches_shared_beacon_fixture(void) {
  const uint8_t fixture_wire[] = {0x4B, 0x01, 0x00, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x87, 0xB1};

  lorahome_header_t decoded;
  const uint8_t* out_payload;
  size_t out_payload_len;
  TEST_ASSERT_TRUE(
      lorahome_decode_frame(fixture_wire, sizeof(fixture_wire), &decoded, &out_payload, &out_payload_len));

  TEST_ASSERT_EQUAL_UINT8(LORAHOME_FRAME_BEACON, decoded.type);
  TEST_ASSERT_EQUAL_UINT16(1, decoded.src_id);
  TEST_ASSERT_EQUAL_UINT16(LORAHOME_BROADCAST_ID, decoded.dst_id);
  TEST_ASSERT_EQUAL_UINT32(0, out_payload_len);
}

// --- typed frame path (T2.2) -----------------------------------------------

// The payload is a window onto the receive buffer, not a copy of it. On a
// device with a 48 kB static RAM budget that is not an optimisation; it is what
// makes a 230 B frame parseable without a second 230 B buffer.
void test_parse_is_zero_copy(void) {
  lorahome_header_t header = {LORAHOME_FRAME_TELEMETRY, 0x0102, 0x0304, 9, LORAHOME_FLAG_NONE};
  const uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8};

  uint8_t wire[64];
  const int wire_len = lh_frame_build(&header, payload, sizeof(payload), wire, sizeof(wire));
  TEST_ASSERT_EQUAL_INT(LORAHOME_HEADER_SIZE + (int)sizeof(payload) + LORAHOME_CRC_SIZE, wire_len);

  lh_frame_view_t view;
  TEST_ASSERT_EQUAL_INT(LH_OK, lh_frame_parse(wire, (uint16_t)wire_len, &view));
  TEST_ASSERT_EQUAL_PTR(wire + LORAHOME_HEADER_SIZE, view.payload);
  TEST_ASSERT_EQUAL_UINT16(sizeof(payload), view.payload_len);
  TEST_ASSERT_EQUAL_UINT16(view.crc_calc, view.crc_rx);
}

// Each refusal names the cheapest true cause. A short fragment would also fail
// its CRC; calling that a CRC error sends whoever reads the Bridge's counters
// after an RF fault that is not there.
void test_parse_error_taxonomy(void) {
  lorahome_header_t header = {LORAHOME_FRAME_BEACON, 1, LORAHOME_BROADCAST_ID, 0,
                              LORAHOME_FLAG_NONE};
  uint8_t wire[32];
  const int wire_len = lh_frame_build(&header, nullptr, 0, wire, sizeof(wire));
  TEST_ASSERT_GREATER_THAN(0, wire_len);

  lh_frame_view_t view;
  TEST_ASSERT_EQUAL_INT(LH_ERR_TOO_SHORT, lh_frame_parse(wire, 9, &view));
  TEST_ASSERT_EQUAL_INT(LH_ERR_TOO_LONG,
                        lh_frame_parse(wire, LORAHOME_MAX_FRAME_SIZE + 1, &view));

  uint8_t alien[32];
  memcpy(alien, wire, (size_t)wire_len);
  alien[0] = 0x7E;
  TEST_ASSERT_EQUAL_INT(LH_ERR_BAD_MAGIC, lh_frame_parse(alien, (uint16_t)wire_len, &view));

  uint8_t corrupt[32];
  memcpy(corrupt, wire, (size_t)wire_len);
  corrupt[6] ^= 0x01;
  TEST_ASSERT_EQUAL_INT(LH_ERR_BAD_CRC, lh_frame_parse(corrupt, (uint16_t)wire_len, &view));
  // Both CRCs are reported, which is what tells random corruption from a
  // structural misread when a link starts misbehaving in the field.
  TEST_ASSERT_NOT_EQUAL_UINT16(view.crc_calc, view.crc_rx);
}

// An unknown type is reported but fully parsed: routing is the caller's
// decision, and it cannot decide about a frame it was not handed.
void test_unknown_type_is_reported_but_parsed(void) {
  lorahome_header_t header = {0x7F, 0x0505, 0x0606, 4, LORAHOME_FLAG_NONE};
  uint8_t wire[32];
  const int wire_len = lh_frame_build(&header, nullptr, 0, wire, sizeof(wire));

  lh_frame_view_t view;
  TEST_ASSERT_EQUAL_INT(LH_ERR_BAD_TYPE, lh_frame_parse(wire, (uint16_t)wire_len, &view));
  TEST_ASSERT_EQUAL_UINT8(0x7F, view.hdr.type);
  TEST_ASSERT_EQUAL_UINT16(0x0505, view.hdr.src_id);
  TEST_ASSERT_FALSE(lh_frame_type_is_known(0x7F));
  TEST_ASSERT_TRUE(lh_frame_type_is_known(LORAHOME_FRAME_TELEMETRY));
}

// A full payload is exactly the MTU on the wire, and one byte more is refused
// without touching the buffer. The whole payload budget rests on this sum.
void test_build_respects_the_mtu(void) {
  static uint8_t payload[LORAHOME_MAX_PAYLOAD + 1];
  static uint8_t wire[LORAHOME_MAX_FRAME_SIZE + 1];
  lorahome_header_t header = {LORAHOME_FRAME_CONFIG_FRAG, 1, 2, 3, LORAHOME_FLAG_FRAG};

  TEST_ASSERT_EQUAL_INT(
      LORAHOME_MAX_FRAME_SIZE,
      lh_frame_build(&header, payload, LORAHOME_MAX_PAYLOAD, wire, sizeof(wire)));

  wire[0] = 0xEE;
  TEST_ASSERT_EQUAL_INT(
      LH_ERR_TOO_LONG,
      lh_frame_build(&header, payload, LORAHOME_MAX_PAYLOAD + 1, wire, sizeof(wire)));
  TEST_ASSERT_EQUAL_UINT8(0xEE, wire[0]);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_encode_decode_frame_round_trip);
  RUN_TEST(test_decode_frame_rejects_corrupted_crc);
  RUN_TEST(test_matches_shared_beacon_fixture);
  RUN_TEST(test_parse_is_zero_copy);
  RUN_TEST(test_parse_error_taxonomy);
  RUN_TEST(test_unknown_type_is_reported_but_parsed);
  RUN_TEST(test_build_respects_the_mtu);
  return UNITY_END();
}
