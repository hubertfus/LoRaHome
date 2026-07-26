/*
 * On-target BME680 tests (T3.3).
 *
 * The host harness in firmware/common/test/bme680_selftest.c simulates the chip
 * at register level and sweeps the compensation across its whole domain. This
 * carries the parts that are properties of the target rather than of the code.
 *
 * There are two, and both are about 64-bit arithmetic. The compensation
 * deliberately diverges from Bosch's int32 reference — the reference overflows
 * at the edges of the raw range and produces plausible wrong readings rather
 * than obvious ones — and on a 32-bit target every one of those int64
 * intermediates becomes a call into libgcc. Whether that arithmetic is correct
 * on xtensa and riscv32 is not something the host build can answer, and the two
 * raw counts below are exactly where the reference version failed: 54032, where
 * humidity wrapped from a clamped 100% to bone dry, and the pressure seam near
 * 106 kPa.
 *
 * Run with `pio test -e native -f test_bme680`, or `-e esp32dev` on a board.
 */
#include <string.h>
#include <unity.h>

#include "lorahome/bme680.h"
#include "lorahome/driver.h"

/* This image's registry: the real driver, bound to nothing. */
const lh_driver_vtable_t* const LH_DRIVERS[] = {&LH_BME680_DRIVER};
const uint8_t LH_DRIVER_COUNT = 1;

static lh_bme680_calib_t g_calib;

/** Coefficients of a plausible part — magnitudes and signs that exercise the maths. */
void setUp(void) {
  memset(&g_calib, 0, sizeof g_calib);
  g_calib.par_t1 = 26041;
  g_calib.par_t2 = 26279;
  g_calib.par_t3 = 3;
  g_calib.par_p1 = 35288;
  g_calib.par_p2 = -10486;
  g_calib.par_p3 = 88;
  g_calib.par_p4 = 6420;
  g_calib.par_p5 = -96;
  g_calib.par_p6 = 30;
  g_calib.par_p7 = 44;
  g_calib.par_p8 = -2688;
  g_calib.par_p9 = -2807;
  g_calib.par_p10 = 30;
  g_calib.par_h1 = 678;
  g_calib.par_h2 = 1017;
  g_calib.par_h3 = 0;
  g_calib.par_h4 = 45;
  g_calib.par_h5 = 20;
  g_calib.par_h6 = 120;
  g_calib.par_h7 = -100;
  g_calib.par_g1 = -51;
  g_calib.par_g2 = -8580;
  g_calib.par_g3 = 18;
  g_calib.res_heat_range = 1;
  g_calib.res_heat_val = 45;
  g_calib.range_sw_err = 0;

  /* Every other channel is corrected against t_fine, so it has to exist. */
  lh_bme680_compensate_temperature(&g_calib, 496440);
}
void tearDown(void) {}

/** The vtable's declared shape is a contract with the manifest (T3.6). */
void test_driver_declaration(void) {
  TEST_ASSERT_EQUAL_STRING("bme680", LH_BME680_DRIVER.name);
  TEST_ASSERT_EQUAL_UINT16(16, LH_BME680_DRIVER.type_id);
  TEST_ASSERT_EQUAL_UINT8(4, LH_BME680_DRIVER.channel_count);
  TEST_ASSERT_EQUAL_UINT16(200, LH_BME680_DRIVER.warmup_ms);
  TEST_ASSERT_NOT_NULL(LH_BME680_DRIVER.start_read);
  TEST_ASSERT_NOT_NULL(LH_BME680_DRIVER.poll);
}

/** Temperature comes out in milli-degrees, not the datasheet's centi-degrees. */
void test_temperature_scale(void) {
  const int32_t milli_c = lh_bme680_compensate_temperature(&g_calib, 496440);
  TEST_ASSERT_INT32_WITHIN(100, 25000, milli_c);

  /* A factor of ten wrong here reads as a cold room rather than as a bug. */
  TEST_ASSERT_TRUE(milli_c > 20000);
  TEST_ASSERT_TRUE(milli_c < 30000);
}

/**
 * The humidity overflow, at the exact raw count where the int32 version broke.
 *
 * Bosch's `var5` squares a shifted intermediate, and past a raw count of about
 * 54000 that wraps in int32. The measured symptom was humidity dropping from a
 * clamped 100% to exactly 0% at 54032 and staying there — a corrupted I2C read
 * reporting bone dry, which is a number a rule engine acts on.
 */
void test_humidity_does_not_wrap_at_the_top_of_the_range(void) {
  TEST_ASSERT_EQUAL_INT32(100000, lh_bme680_compensate_humidity(&g_calib, 54031));
  TEST_ASSERT_EQUAL_INT32(100000, lh_bme680_compensate_humidity(&g_calib, 54032));
  TEST_ASSERT_EQUAL_INT32(100000, lh_bme680_compensate_humidity(&g_calib, 60000));
  TEST_ASSERT_EQUAL_INT32(100000, lh_bme680_compensate_humidity(&g_calib, 65535));

  /* And the normal range still behaves. */
  TEST_ASSERT_EQUAL_INT32(0, lh_bme680_compensate_humidity(&g_calib, 0));
  const int32_t mid = lh_bme680_compensate_humidity(&g_calib, 20000);
  TEST_ASSERT_TRUE(mid > 0 && mid < 100000);
}

/**
 * The pressure seam, at the raw count where the int32 version stepped 1954 Pa
 * the wrong way. Pressure falls as the raw count rises; nowhere does it rise.
 */
void test_pressure_has_no_seam(void) {
  const int32_t before = lh_bme680_compensate_pressure(&g_calib, 344500);
  const int32_t at = lh_bme680_compensate_pressure(&g_calib, 345000);
  const int32_t after = lh_bme680_compensate_pressure(&g_calib, 345500);

  TEST_ASSERT_TRUE(before > at);
  TEST_ASSERT_TRUE(at > after);

  /* A sample across the realistic band, in pascals rather than hectopascals. */
  const int32_t sea_level = lh_bme680_compensate_pressure(&g_calib, 380000);
  TEST_ASSERT_TRUE(sea_level > 87000);
  TEST_ASSERT_TRUE(sea_level < 108000);
}

/** Gas resistance never goes negative, and an impossible range code is refused. */
void test_gas_bounds(void) {
  TEST_ASSERT_TRUE(lh_bme680_compensate_gas(&g_calib, 600, 5) >= 0);
  TEST_ASSERT_EQUAL_INT32(0, lh_bme680_compensate_gas(&g_calib, 600, 16));
  TEST_ASSERT_EQUAL_INT32(0, lh_bme680_compensate_gas(&g_calib, 600, 255));
}

/** The heater duration encoding: a 6-bit count and a 2-bit multiplier of four. */
void test_gas_wait_encoding(void) {
  TEST_ASSERT_EQUAL_UINT8(0, lh_bme680_encode_gas_wait(0));
  TEST_ASSERT_EQUAL_UINT8(63, lh_bme680_encode_gas_wait(63));
  TEST_ASSERT_EQUAL_UINT8(16 | (1 << 6), lh_bme680_encode_gas_wait(64));
  TEST_ASSERT_EQUAL_UINT8(37 | (1 << 6), lh_bme680_encode_gas_wait(150));
  /* Saturating rather than wrapping: a wrapped value is a very short heat, and
   * a cold heater reports a gas resistance that means nothing. */
  TEST_ASSERT_EQUAL_UINT8(0xFF, lh_bme680_encode_gas_wait(0xFFFF));
}

/** Without an I/O binding the driver refuses rather than dereferencing nothing. */
void test_refuses_to_work_without_a_bus(void) {
  lh_bme680_reset_instances();
  lh_bme680_set_io(nullptr);

  lh_driver_ctx_t ctx;
  TEST_ASSERT_EQUAL_INT(
      LH_DRV_ERR_BUS, lh_driver_bind(&ctx, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 0, 0));
  TEST_ASSERT_EQUAL_UINT8(0, lh_bme680_instances_in_use());
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_driver_declaration);
  RUN_TEST(test_temperature_scale);
  RUN_TEST(test_humidity_does_not_wrap_at_the_top_of_the_range);
  RUN_TEST(test_pressure_has_no_seam);
  RUN_TEST(test_gas_bounds);
  RUN_TEST(test_gas_wait_encoding);
  RUN_TEST(test_refuses_to_work_without_a_bus);
  return UNITY_END();
}
