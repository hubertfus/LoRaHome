/*
 * On-target SLIP tests (T1.1).
 *
 * The host harness in firmware/common/test/slip_selftest.c does the heavy
 * lifting — 10k round trips, a million fuzz iterations, sanitizers. What it
 * cannot do is run on the ABI the code actually ships to, and the struct
 * layout, the enum width and the 16-bit arithmetic are all things that can
 * differ between an x86-64 laptop and an ESP32. These tests are the on-target
 * half of that pair: fewer cases, real hardware.
 *
 * Run with `pio test -e esp32dev -f test_slip`.
 */
#include <unity.h>

#include <string.h>

#include "lorahome/slip.h"

static uint8_t g_frame_buf[256];
static lh_slip_decoder_t g_dec;

void setUp(void) { lh_slip_init(&g_dec, g_frame_buf, sizeof(g_frame_buf)); }
void tearDown(void) {}

/** Feeds a buffer, returning the last result that was not NEED_MORE. */
static lh_slip_feed_r feed_all(const uint8_t* data, uint16_t len) {
  lh_slip_feed_r last = LH_SLIP_NEED_MORE;
  for (uint16_t i = 0; i < len; i++) {
    const lh_slip_feed_r r = lh_slip_feed(&g_dec, data[i]);
    if (r != LH_SLIP_NEED_MORE) last = r;
  }
  return last;
}

/**
 * The budget the whole design is built around: 32 B of decoder state on top of
 * a caller-owned buffer. slip.c asserts this at compile time; asserting it here
 * too means the number shows up in the on-target test log, where somebody
 * chasing a RAM budget will actually see it.
 */
void test_decoder_struct_fits_budget(void) {
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(32, (uint32_t)sizeof(lh_slip_decoder_t));
}

void test_encode_decode_round_trip(void) {
  const uint8_t data[] = {0x01, 0x02, 0x03};
  uint8_t encoded[16];

  const uint16_t encoded_len = lh_slip_encode(data, sizeof(data), encoded, sizeof(encoded));
  TEST_ASSERT_EQUAL_UINT16(sizeof(data) + 2, encoded_len);

  TEST_ASSERT_EQUAL_INT(LH_SLIP_FRAME_READY, feed_all(encoded, encoded_len));
  TEST_ASSERT_EQUAL_UINT16(sizeof(data), g_dec.len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data, g_dec.buf, sizeof(data));
  TEST_ASSERT_EQUAL_UINT32(1, g_dec.stat_frames_ok);
}

void test_escapes_end_and_esc_bytes(void) {
  const uint8_t data[] = {LH_SLIP_END, LH_SLIP_ESC, 0x42};
  uint8_t encoded[16];

  // Leading END + 2 escaped bytes (2 each) + 1 plain byte + trailing END = 7.
  const uint16_t encoded_len = lh_slip_encode(data, sizeof(data), encoded, sizeof(encoded));
  TEST_ASSERT_EQUAL_UINT16(7, encoded_len);

  TEST_ASSERT_EQUAL_INT(LH_SLIP_FRAME_READY, feed_all(encoded, encoded_len));
  TEST_ASSERT_EQUAL_UINT16(sizeof(data), g_dec.len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data, g_dec.buf, sizeof(data));
}

/** Risk R1.1: the payload that doubles on the wire. */
void test_worst_case_expansion(void) {
  uint8_t payload[64];
  uint8_t encoded[2 * sizeof(payload) + 2];
  for (size_t i = 0; i < sizeof(payload); i++) {
    payload[i] = (i & 1) ? LH_SLIP_ESC : LH_SLIP_END;
  }

  const uint16_t encoded_len = lh_slip_encode(payload, sizeof(payload), encoded, sizeof(encoded));
  TEST_ASSERT_EQUAL_UINT16(2 * sizeof(payload) + 2, encoded_len);

  TEST_ASSERT_EQUAL_INT(LH_SLIP_FRAME_READY, feed_all(encoded, encoded_len));
  TEST_ASSERT_EQUAL_UINT16(sizeof(payload), g_dec.len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, g_dec.buf, sizeof(payload));
}

/** Capacity is demanded for the worst case, so the failure is deterministic. */
void test_encode_refuses_a_buffer_that_could_overflow(void) {
  const uint8_t data[] = {0x01, 0x02, 0x03};
  uint8_t encoded[7];
  TEST_ASSERT_EQUAL_UINT16(0, lh_slip_encode(data, sizeof(data), encoded, sizeof(encoded)));
  TEST_ASSERT_EQUAL_UINT16(5, lh_slip_encode(data, sizeof(data), encoded, 8));
}

/** One damaged frame must cost one frame, not the rest of the session. */
void test_resync_after_bad_escape(void) {
  const uint8_t corrupt[] = {LH_SLIP_END, 0x11, LH_SLIP_ESC, 0x42, LH_SLIP_END};
  TEST_ASSERT_EQUAL_INT(LH_SLIP_ERROR, feed_all(corrupt, sizeof(corrupt)));
  TEST_ASSERT_EQUAL_UINT32(1, g_dec.stat_bad_escape);
  TEST_ASSERT_EQUAL_UINT32(1, g_dec.stat_dropped);
  TEST_ASSERT_EQUAL_UINT32(0, g_dec.stat_frames_ok);

  const uint8_t good[] = {0xDE, 0xAD};
  uint8_t encoded[8];
  const uint16_t encoded_len = lh_slip_encode(good, sizeof(good), encoded, sizeof(encoded));
  TEST_ASSERT_EQUAL_INT(LH_SLIP_FRAME_READY, feed_all(encoded, encoded_len));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(good, g_dec.buf, sizeof(good));
}

/** An oversized frame is reported, not silently truncated, and stays in bounds. */
void test_overflow_is_reported_and_bounded(void) {
  static uint8_t small[16];
  lh_slip_decoder_t dec;
  lh_slip_init(&dec, small, sizeof(small));

  lh_slip_feed(&dec, LH_SLIP_END);
  lh_slip_feed_r result = LH_SLIP_NEED_MORE;
  for (int i = 0; i < 64; i++) {
    const lh_slip_feed_r r = lh_slip_feed(&dec, (uint8_t)i);
    if (r == LH_SLIP_ERROR) {
      result = r;
      break;
    }
  }

  TEST_ASSERT_EQUAL_INT(LH_SLIP_ERROR, result);
  TEST_ASSERT_EQUAL_UINT32(1, dec.stat_overflow);
  TEST_ASSERT_LESS_OR_EQUAL_UINT16(sizeof(small), dec.len);
}

/** An empty frame (END END) is a delimiter pair, not a zero-length message. */
void test_empty_frame_is_not_delivered(void) {
  const uint8_t empty[] = {LH_SLIP_END, LH_SLIP_END};
  TEST_ASSERT_EQUAL_INT(LH_SLIP_NEED_MORE, feed_all(empty, sizeof(empty)));
  TEST_ASSERT_EQUAL_UINT32(0, g_dec.stat_frames_ok);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_decoder_struct_fits_budget);
  RUN_TEST(test_encode_decode_round_trip);
  RUN_TEST(test_escapes_end_and_esc_bytes);
  RUN_TEST(test_worst_case_expansion);
  RUN_TEST(test_encode_refuses_a_buffer_that_could_overflow);
  RUN_TEST(test_resync_after_bad_escape);
  RUN_TEST(test_overflow_is_reported_and_bounded);
  RUN_TEST(test_empty_frame_is_not_delivered);
  return UNITY_END();
}
