#include <unity.h>
#include "lorahome/slip.h"

void setUp(void) {}
void tearDown(void) {}

void test_encode_decode_round_trip(void) {
  const uint8_t data[] = {0x01, 0x02, 0x03};
  uint8_t encoded[16];
  int encoded_len = lorahome_slip_encode(data, sizeof(data), encoded, sizeof(encoded));
  TEST_ASSERT_GREATER_THAN(0, encoded_len);

  uint8_t decode_buf[16];
  lorahome_slip_decoder_t dec;
  lorahome_slip_decoder_init(&dec, decode_buf, sizeof(decode_buf));

  bool got_frame = false;
  for (int i = 0; i < encoded_len; i++) {
    if (lorahome_slip_decoder_feed(&dec, encoded[i])) {
      got_frame = true;
      break;
    }
  }

  TEST_ASSERT_TRUE(got_frame);
  TEST_ASSERT_EQUAL_UINT32(sizeof(data), dec.len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data, dec.buf, sizeof(data));
}

void test_escapes_end_and_esc_bytes(void) {
  const uint8_t data[] = {LORAHOME_SLIP_END, LORAHOME_SLIP_ESC, 0x42};
  uint8_t encoded[16];
  int encoded_len = lorahome_slip_encode(data, sizeof(data), encoded, sizeof(encoded));
  TEST_ASSERT_GREATER_THAN(0, encoded_len);

  // Leading END + 2 escaped bytes (2 bytes each) + 1 plain byte + trailing END = 1+4+1+1 = 7
  TEST_ASSERT_EQUAL_INT(7, encoded_len);

  uint8_t decode_buf[16];
  lorahome_slip_decoder_t dec;
  lorahome_slip_decoder_init(&dec, decode_buf, sizeof(decode_buf));

  bool got_frame = false;
  for (int i = 0; i < encoded_len; i++) {
    if (lorahome_slip_decoder_feed(&dec, encoded[i])) {
      got_frame = true;
      break;
    }
  }

  TEST_ASSERT_TRUE(got_frame);
  TEST_ASSERT_EQUAL_UINT32(sizeof(data), dec.len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data, dec.buf, sizeof(data));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_encode_decode_round_trip);
  RUN_TEST(test_escapes_end_and_esc_bytes);
  return UNITY_END();
}
