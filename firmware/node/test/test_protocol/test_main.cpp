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

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_encode_decode_frame_round_trip);
  RUN_TEST(test_decode_frame_rejects_corrupted_crc);
  RUN_TEST(test_matches_shared_beacon_fixture);
  return UNITY_END();
}
