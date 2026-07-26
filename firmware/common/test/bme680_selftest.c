/*
 * Native correctness and benchmark harness for the BME680 driver (T3.3).
 *
 * A simulated chip at register level: the calibration blocks, the status bits,
 * the forced-mode trigger and a virtual clock that makes the 180 ms conversion
 * take 180 ms of modelled time and no real time at all. What that buys is the
 * ability to test the things a board cannot conveniently be made to do — the
 * sensor unplugged halfway through a read, the heater that never reaches
 * temperature, a third instance where the chip allows two — on every commit
 * rather than on the day somebody has a pair of tweezers.
 *
 * What it cannot do is tell you whether the numbers are right. Compensation is
 * checked here for self-consistency, sign, scale and monotonicity, which
 * catches the errors that make a reading meaningless; it cannot catch a
 * transcription slip that is consistently wrong, because the only reference for
 * that is silicon. The roadmap's sanity ranges are an on-hardware test and are
 * reported SKIPPED until there is hardware.
 *
 * Driven by tools/run-native.mjs. Output is LH_METRIC lines per roadmap §0.4.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/bme680.h"
#include "lorahome/driver.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

/* ------------------------------------------------------------------------- */
/* Harness plumbing                                                          */
/* ------------------------------------------------------------------------- */

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                          \
  do {                                            \
    g_checks++;                                   \
    if (!(cond)) {                                \
      g_failures++;                               \
      printf("FAIL %s:%d  ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
    }                                             \
  } while (0)

static double now_seconds(void) {
#if defined(_WIN32)
  static LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static int compare_double(const void *a, const void *b) {
  const double lhs = *(const double *)a;
  const double rhs = *(const double *)b;
  return (lhs > rhs) - (lhs < rhs);
}

/* The registry this image ships with: one real driver, no mocks needed. */
const lh_driver_vtable_t *const LH_DRIVERS[] = {&LH_BME680_DRIVER};
const uint8_t LH_DRIVER_COUNT = 1;

/* ------------------------------------------------------------------------- */
/* Simulated BME680                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Calibration coefficients of a plausible part.
 *
 * Representative magnitudes rather than any particular chip's: what they have
 * to be is the right size and sign, so the polynomials are exercised in the
 * region real ones operate in. A set of zeroes would pass every test here and
 * prove nothing.
 */
#define PAR_T1 26041u
#define PAR_T2 26279
#define PAR_T3 3
#define PAR_P1 35288u
#define PAR_P2 (-10486)
#define PAR_P3 88
#define PAR_P4 6420
#define PAR_P5 (-96)
#define PAR_P6 30
#define PAR_P7 44
#define PAR_P8 (-2688)
#define PAR_P9 (-2807)
#define PAR_P10 30u
#define PAR_H1 678u
#define PAR_H2 1017u
#define PAR_H3 0
#define PAR_H4 45
#define PAR_H5 20
#define PAR_H6 120u
#define PAR_H7 (-100)
#define PAR_G1 (-51)
#define PAR_G2 (-8580)
#define PAR_G3 18
#define RES_HEAT_RANGE 1u
#define RES_HEAT_VAL 45
#define RANGE_SW_ERR 0

/** How long the modelled chip takes for one forced conversion, in microseconds. */
#define CONVERSION_US 180000

typedef struct {
  bool present; /* false simulates the sensor being unplugged */
  uint8_t chip_id;
  int64_t clock_us;

  uint8_t calib1[LH_BME680_CALIB_BLOCK1];
  uint8_t calib2[LH_BME680_CALIB_BLOCK2];
  uint8_t extra[3];

  /* Raw conversion results the chip will report. */
  uint32_t temp_adc;
  uint32_t press_adc;
  uint16_t hum_adc;
  uint16_t gas_adc;
  uint8_t gas_range;
  bool gas_valid;
  bool heat_stable;

  /* Forced-mode state. */
  bool converting;
  int64_t conversion_done_us;
  bool new_data;

  /* What the driver wrote, so ordering can be asserted. */
  uint32_t writes;
  uint32_t reads;
  int ctrl_hum_write_index;
  int ctrl_meas_write_index;
  uint8_t last_res_heat;
  uint8_t last_gas_wait;
  bool heater_off;
  uint8_t mode;
} sim_t;

static sim_t g_sim;

/** Builds the two calibration register blocks from the coefficients above. */
static void sim_build_calibration(sim_t *sim) {
  memset(sim->calib1, 0, sizeof sim->calib1);
  memset(sim->calib2, 0, sizeof sim->calib2);

  sim->calib1[1] = (uint8_t)(PAR_T2 & 0xFF);
  sim->calib1[2] = (uint8_t)((PAR_T2 >> 8) & 0xFF);
  sim->calib1[3] = (uint8_t)PAR_T3;
  sim->calib1[5] = (uint8_t)(PAR_P1 & 0xFF);
  sim->calib1[6] = (uint8_t)((PAR_P1 >> 8) & 0xFF);
  sim->calib1[7] = (uint8_t)(PAR_P2 & 0xFF);
  sim->calib1[8] = (uint8_t)(((uint16_t)PAR_P2 >> 8) & 0xFF);
  sim->calib1[9] = (uint8_t)PAR_P3;
  sim->calib1[11] = (uint8_t)(PAR_P4 & 0xFF);
  sim->calib1[12] = (uint8_t)((PAR_P4 >> 8) & 0xFF);
  sim->calib1[13] = (uint8_t)(PAR_P5 & 0xFF);
  sim->calib1[14] = (uint8_t)(((uint16_t)PAR_P5 >> 8) & 0xFF);
  sim->calib1[15] = (uint8_t)PAR_P7;
  sim->calib1[16] = (uint8_t)PAR_P6;
  sim->calib1[19] = (uint8_t)(PAR_P8 & 0xFF);
  sim->calib1[20] = (uint8_t)(((uint16_t)PAR_P8 >> 8) & 0xFF);
  sim->calib1[21] = (uint8_t)(PAR_P9 & 0xFF);
  sim->calib1[22] = (uint8_t)(((uint16_t)PAR_P9 >> 8) & 0xFF);
  sim->calib1[23] = (uint8_t)PAR_P10;

  /* par_h1 and par_h2 share the nibbles of 0xE2 — block2[1]. */
  sim->calib2[0] = (uint8_t)(PAR_H2 >> 4);
  sim->calib2[1] = (uint8_t)(((PAR_H2 & 0x0Fu) << 4) | (PAR_H1 & 0x0Fu));
  sim->calib2[2] = (uint8_t)(PAR_H1 >> 4);
  sim->calib2[3] = (uint8_t)PAR_H3;
  sim->calib2[4] = (uint8_t)PAR_H4;
  sim->calib2[5] = (uint8_t)PAR_H5;
  sim->calib2[6] = (uint8_t)PAR_H6;
  sim->calib2[7] = (uint8_t)PAR_H7;
  sim->calib2[8] = (uint8_t)(PAR_T1 & 0xFF);
  sim->calib2[9] = (uint8_t)((PAR_T1 >> 8) & 0xFF);
  sim->calib2[10] = (uint8_t)(PAR_G2 & 0xFF);
  sim->calib2[11] = (uint8_t)(((uint16_t)PAR_G2 >> 8) & 0xFF);
  sim->calib2[12] = (uint8_t)PAR_G1;
  sim->calib2[13] = (uint8_t)PAR_G3;

  sim->extra[0] = (uint8_t)RES_HEAT_VAL;
  sim->extra[1] = (uint8_t)(RES_HEAT_RANGE << 4);
  sim->extra[2] = (uint8_t)(RANGE_SW_ERR << 4);
}

static void sim_reset(void) {
  memset(&g_sim, 0, sizeof g_sim);
  g_sim.present = true;
  g_sim.chip_id = LH_BME680_CHIP_ID;
  g_sim.clock_us = 1000000;
  g_sim.temp_adc = 510000;
  g_sim.press_adc = 400000;
  g_sim.hum_adc = 25000;
  g_sim.gas_adc = 600;
  g_sim.gas_range = 5;
  g_sim.gas_valid = true;
  g_sim.heat_stable = true;
  g_sim.ctrl_hum_write_index = -1;
  g_sim.ctrl_meas_write_index = -1;
  sim_build_calibration(&g_sim);
  lh_bme680_reset_instances();
}

static int64_t sim_now(void *user) { return ((sim_t *)user)->clock_us; }

static bool sim_read(uint8_t addr, uint8_t reg, uint8_t *out, uint8_t len, void *user) {
  sim_t *sim = (sim_t *)user;
  if (!sim->present) return false;
  if (addr != LH_BME680_ADDR_LOW && addr != LH_BME680_ADDR_HIGH) return false;
  sim->reads++;

  /* Each transaction costs a little modelled time, so a driver that polls in a
   * tight loop shows up as one that spends time rather than one that is free. */
  sim->clock_us += 200;

  if (reg == 0xD0u && len == 1) {
    out[0] = sim->chip_id;
    return true;
  }
  if (reg == 0x89u && len == LH_BME680_CALIB_BLOCK1) {
    memcpy(out, sim->calib1, LH_BME680_CALIB_BLOCK1);
    return true;
  }
  if (reg == 0xE1u && len == LH_BME680_CALIB_BLOCK2) {
    memcpy(out, sim->calib2, LH_BME680_CALIB_BLOCK2);
    return true;
  }
  if (reg == 0x00u && len == 1) {
    out[0] = sim->extra[0];
    return true;
  }
  if (reg == 0x02u && len == 1) {
    out[0] = sim->extra[1];
    return true;
  }
  if (reg == 0x04u && len == 1) {
    out[0] = sim->extra[2];
    return true;
  }

  if (reg == 0x1Du && len == 1) {
    if (sim->converting && sim->clock_us >= sim->conversion_done_us) {
      sim->converting = false;
      sim->new_data = true;
    }
    out[0] = (uint8_t)((sim->converting ? 0x60u : 0x00u) | (sim->new_data ? 0x80u : 0x00u));
    return true;
  }

  if (reg == 0x1Fu && len == 8) {
    out[0] = (uint8_t)(sim->press_adc >> 12);
    out[1] = (uint8_t)(sim->press_adc >> 4);
    out[2] = (uint8_t)((sim->press_adc & 0x0Fu) << 4);
    out[3] = (uint8_t)(sim->temp_adc >> 12);
    out[4] = (uint8_t)(sim->temp_adc >> 4);
    out[5] = (uint8_t)((sim->temp_adc & 0x0Fu) << 4);
    out[6] = (uint8_t)(sim->hum_adc >> 8);
    out[7] = (uint8_t)(sim->hum_adc & 0xFFu);
    return true;
  }

  if (reg == 0x2Au && len == 2) {
    out[0] = (uint8_t)(sim->gas_adc >> 2);
    out[1] = (uint8_t)(((sim->gas_adc & 0x03u) << 6) | (sim->gas_valid ? 0x20u : 0u) |
                       (sim->heat_stable ? 0x10u : 0u) | (sim->gas_range & 0x0Fu));
    return true;
  }

  return false;
}

static bool sim_write(uint8_t addr, uint8_t reg, uint8_t value, void *user) {
  sim_t *sim = (sim_t *)user;
  if (!sim->present) return false;
  if (addr != LH_BME680_ADDR_LOW && addr != LH_BME680_ADDR_HIGH) return false;

  const int index = (int)sim->writes++;
  sim->clock_us += 200;

  switch (reg) {
    case 0x72u:
      sim->ctrl_hum_write_index = index;
      break;
    case 0x74u:
      sim->ctrl_meas_write_index = index;
      sim->mode = (uint8_t)(value & 0x03u);
      if (sim->mode == 0x01u) {
        /* Forced: the conversion starts now and finishes later. Nothing about
         * this write blocks, which is the property under test. */
        sim->converting = true;
        sim->new_data = false;
        sim->conversion_done_us = sim->clock_us + CONVERSION_US;
      }
      break;
    case 0x5Au:
      sim->last_gas_wait = value;
      break;
    case 0x64u:
      sim->last_res_heat = value;
      break;
    case 0x70u:
      sim->heater_off = (value & 0x08u) != 0;
      break;
    default:
      break;
  }
  return true;
}

static lh_i2c_io_t sim_io(void) {
  lh_i2c_io_t io;
  io.read = sim_read;
  io.write = sim_write;
  io.now_us = sim_now;
  io.user = &g_sim;
  return io;
}

static void install_sim(void) {
  sim_reset();
  const lh_i2c_io_t io = sim_io();
  lh_bme680_set_io(&io);
}

/* ------------------------------------------------------------------------- */
/* Calibration parsing                                                       */
/* ------------------------------------------------------------------------- */

/**
 * Every coefficient survives the round trip through the register blocks.
 *
 * The one that matters is the pair sharing register 0xE2: par_h1 takes its low
 * nibble and par_h2 its high one, and getting that backwards produces a
 * humidity reading that is wrong by a plausible-looking amount rather than
 * obviously broken.
 */
static void test_calibration_parsing(void) {
  sim_reset();
  lh_bme680_calib_t calib;
  lh_bme680_parse_calibration(g_sim.calib1, g_sim.calib2, g_sim.extra, &calib);

  CHECK(calib.par_t1 == PAR_T1, "par_t1 %u != %u", (unsigned)calib.par_t1, (unsigned)PAR_T1);
  CHECK(calib.par_t2 == PAR_T2, "par_t2 %d != %d", (int)calib.par_t2, (int)PAR_T2);
  CHECK(calib.par_t3 == PAR_T3, "par_t3 %d", (int)calib.par_t3);
  CHECK(calib.par_p1 == PAR_P1, "par_p1 %u", (unsigned)calib.par_p1);
  CHECK(calib.par_p2 == PAR_P2, "par_p2 %d", (int)calib.par_p2);
  CHECK(calib.par_p3 == PAR_P3, "par_p3 %d", (int)calib.par_p3);
  CHECK(calib.par_p4 == PAR_P4, "par_p4 %d", (int)calib.par_p4);
  CHECK(calib.par_p5 == PAR_P5, "par_p5 %d", (int)calib.par_p5);
  CHECK(calib.par_p6 == PAR_P6, "par_p6 %d", (int)calib.par_p6);
  CHECK(calib.par_p7 == PAR_P7, "par_p7 %d", (int)calib.par_p7);
  CHECK(calib.par_p8 == PAR_P8, "par_p8 %d", (int)calib.par_p8);
  CHECK(calib.par_p9 == PAR_P9, "par_p9 %d", (int)calib.par_p9);
  CHECK(calib.par_p10 == PAR_P10, "par_p10 %u", (unsigned)calib.par_p10);

  CHECK(calib.par_h1 == PAR_H1, "par_h1 %u != %u — the 0xE2 nibbles are crossed",
        (unsigned)calib.par_h1, (unsigned)PAR_H1);
  CHECK(calib.par_h2 == PAR_H2, "par_h2 %u != %u — the 0xE2 nibbles are crossed",
        (unsigned)calib.par_h2, (unsigned)PAR_H2);
  CHECK(calib.par_h3 == PAR_H3, "par_h3 %d", (int)calib.par_h3);
  CHECK(calib.par_h4 == PAR_H4, "par_h4 %d", (int)calib.par_h4);
  CHECK(calib.par_h5 == PAR_H5, "par_h5 %d", (int)calib.par_h5);
  CHECK(calib.par_h6 == PAR_H6, "par_h6 %u", (unsigned)calib.par_h6);
  CHECK(calib.par_h7 == PAR_H7, "par_h7 %d", (int)calib.par_h7);

  CHECK(calib.par_g1 == PAR_G1, "par_g1 %d", (int)calib.par_g1);
  CHECK(calib.par_g2 == PAR_G2, "par_g2 %d", (int)calib.par_g2);
  CHECK(calib.par_g3 == PAR_G3, "par_g3 %d", (int)calib.par_g3);
  CHECK(calib.res_heat_range == RES_HEAT_RANGE, "res_heat_range %u",
        (unsigned)calib.res_heat_range);
  CHECK(calib.res_heat_val == RES_HEAT_VAL, "res_heat_val %d", (int)calib.res_heat_val);
}

/* ------------------------------------------------------------------------- */
/* Compensation                                                              */
/* ------------------------------------------------------------------------- */

static lh_bme680_calib_t calibration(void) {
  sim_reset();
  lh_bme680_calib_t calib;
  lh_bme680_parse_calibration(g_sim.calib1, g_sim.calib2, g_sim.extra, &calib);
  return calib;
}

/** Smallest temp_adc whose compensated temperature reaches `target_milli_c`. */
static uint32_t adc_for_temperature(lh_bme680_calib_t *calib, int32_t target_milli_c) {
  uint32_t low = 0;
  uint32_t high = 1u << 20;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    if (lh_bme680_compensate_temperature(calib, mid) < target_milli_c) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return low;
}

/**
 * Temperature: monotonic, invertible, and on the right scale.
 *
 * Searching for the raw count that yields 25.000 °C and checking the result
 * lands there proves three things at once — that the function increases with
 * its input, that it has no discontinuities in the operating range, and that
 * the output really is milli-degrees rather than the datasheet's centi-degrees.
 * The last is the mistake worth guarding: a missing factor of ten produces
 * readings of 2.5 °C that look like a cold room rather than like a bug.
 */
static void test_temperature_compensation(void) {
  lh_bme680_calib_t calib = calibration();

  const uint32_t adc_25 = adc_for_temperature(&calib, 25000);
  const int32_t at_25 = lh_bme680_compensate_temperature(&calib, adc_25);
  CHECK(at_25 >= 25000 && at_25 < 25050, "25 C should round-trip, got %ld milli-C",
        (long)at_25);

  const uint32_t adc_0 = adc_for_temperature(&calib, 0);
  const uint32_t adc_40 = adc_for_temperature(&calib, 40000);
  CHECK(adc_0 < adc_25 && adc_25 < adc_40, "temperature must increase with the raw count");

  const int32_t at_0 = lh_bme680_compensate_temperature(&calib, adc_0);
  CHECK(at_0 >= 0 && at_0 < 50, "0 C should round-trip, got %ld milli-C", (long)at_0);

  /* Monotonic across the whole operating range, not just at three points. */
  int32_t previous = INT32_MIN;
  int breaks = 0;
  for (uint32_t adc = 400000; adc < 700000; adc += 1000) {
    const int32_t value = lh_bme680_compensate_temperature(&calib, adc);
    if (value < previous) breaks++;
    previous = value;
  }
  CHECK(breaks == 0, "temperature must be monotonic; %d inversions", breaks);
}

/**
 * Pressure: right units, right direction, and no division by zero.
 *
 * The units check is the valuable one. Pascals and hectopascals differ by a
 * factor of 100, and a pressure channel reporting 1013 instead of 101325 is a
 * plausible number in the wrong unit — exactly the kind of thing a manifest's
 * `scale` then silently compensates for in the UI while the rule engine
 * compares against a threshold in the other one.
 */
static void test_pressure_compensation(void) {
  lh_bme680_calib_t calib = calibration();
  lh_bme680_compensate_temperature(&calib, adc_for_temperature(&calib, 25000));

  /*
   * Strictly monotonic across the entire raw range, with no seam anywhere.
   *
   * This is the assertion that found the overflow. Bosch's int32 version steps
   * 1954 Pa *upwards* at a raw count near 106 kPa, where its intermediate
   * passes 2^31; the 64-bit version in bme680.c has no such point. Sweeping the
   * whole 20-bit domain rather than a plausible window is deliberate — the
   * failure was outside the window somebody would have chosen.
   */
  int32_t previous = INT32_MAX;
  int breaks = 0;
  int32_t worst = 0;
  int32_t plausible = 0;
  for (uint32_t adc = 0; adc < 1048576; adc += 97) {
    const int32_t pa = lh_bme680_compensate_pressure(&calib, adc);
    /* Pressure falls as the raw count rises — the polynomial's sign, and the
     * single easiest thing to transcribe backwards. */
    if (pa > previous) {
      breaks++;
      if (pa - previous > worst) worst = pa - previous;
    }
    previous = pa;
    if (pa > 87000 && pa < 108000) plausible++;
  }
  CHECK(breaks == 0, "pressure must fall monotonically across the whole raw range; %d inversions, "
                     "worst +%ld Pa",
        breaks, (long)worst);
  CHECK(plausible > 0, "some raw count in range should give a sea-level-plausible pressure");

  /* An uncalibrated part must not divide by zero. On an ESP32 that is a reset,
   * which turns a sensor fault into a reboot loop. */
  lh_bme680_calib_t zeroed;
  memset(&zeroed, 0, sizeof zeroed);
  CHECK(lh_bme680_compensate_pressure(&zeroed, 400000) == 0,
        "zero calibration should give 0 Pa, not a fault");
}

/** Humidity: milli-percent, clamped at both ends, monotonic in between. */
static void test_humidity_compensation(void) {
  lh_bme680_calib_t calib = calibration();
  lh_bme680_compensate_temperature(&calib, adc_for_temperature(&calib, 25000));

  /*
   * Non-decreasing across every raw count a 16-bit field can hold.
   *
   * The domain is swept in full on purpose. A healthy sensor saturates at 100%
   * around a raw count of 27500 and never goes near the top of the range — but
   * a corrupted I2C read can land anywhere, and the int32 version of this
   * arithmetic dropped from a clamped 100% to exactly 0% at 54032 and stayed
   * there. That is the shape of bug this sweep exists to find: not a crash, a
   * plausible reading that a rule would act on.
   */
  int32_t previous = INT32_MIN;
  int breaks = 0;
  int32_t worst = 0;
  int out_of_range = 0;
  uint32_t saturated_at = 0;

  for (uint32_t adc = 0; adc <= 65535; adc++) {
    const int32_t rh = lh_bme680_compensate_humidity(&calib, (uint16_t)adc);
    if (rh < 0 || rh > 100000) out_of_range++;
    if (rh >= 100000 && saturated_at == 0) saturated_at = adc;
    if (rh < previous) {
      breaks++;
      if (previous - rh > worst) worst = previous - rh;
    }
    previous = rh;
  }

  CHECK(saturated_at > 0, "humidity should reach 100 pct somewhere in the raw range");
  CHECK(breaks == 0, "humidity must never decrease as the raw count rises; %d inversions, "
                     "worst -%ld milli-pct",
        breaks, (long)worst);
  CHECK(out_of_range == 0, "humidity must stay in 0..100000 milli-pct; %d escaped", out_of_range);
  CHECK(lh_bme680_compensate_humidity(&calib, 65535) == 100000,
        "the top of the raw range must saturate, not wrap to bone dry");

  /* Both clamps reachable: the polynomial is unbounded and a sensor reporting
   * 104% would otherwise reach a rule as a number. */
  CHECK(lh_bme680_compensate_humidity(&calib, 0) == 0, "the low clamp should hold");
  CHECK(lh_bme680_compensate_humidity(&calib, 65535) == 100000, "the high clamp should hold");
}

/** Gas resistance: positive, finite, and safe at every range code. */
static void test_gas_compensation(void) {
  lh_bme680_calib_t calib = calibration();

  int nonzero = 0;
  int negative = 0;
  for (uint8_t range = 0; range < 16; range++) {
    for (uint16_t adc = 100; adc < 1000; adc = (uint16_t)(adc + 100)) {
      const int32_t ohms = lh_bme680_compensate_gas(&calib, adc, range);
      if (ohms < 0) negative++;
      if (ohms > 0) nonzero++;
    }
  }
  CHECK(nonzero > 0, "some combination should give a resistance");
  CHECK(negative == 0, "a resistance is never negative; %d were", negative);

  /* An out-of-range code is rejected rather than indexing past the lookup
   * tables — the range field is four bits from a chip that may be faulty. */
  CHECK(lh_bme680_compensate_gas(&calib, 600, 16) == 0, "range 16 does not exist");
  CHECK(lh_bme680_compensate_gas(&calib, 600, 255) == 0, "nor does range 255");
}

/** The heater setpoint saturates rather than wrapping, at both ends. */
static void test_heater_and_gas_wait(void) {
  lh_bme680_calib_t calib = calibration();

  const uint8_t at_320 = lh_bme680_heater_resistance(&calib, 320, 25);
  CHECK(at_320 > 0, "a 320 C setpoint should give a heater value, got %u", (unsigned)at_320);

  /* Above the chip's 400 C limit the request is clamped, not wrapped: a
   * register value computed from 900 C means something else entirely. */
  CHECK(lh_bme680_heater_resistance(&calib, 900, 25) ==
            lh_bme680_heater_resistance(&calib, 400, 25),
        "an over-range setpoint should clamp to 400 C");
  CHECK(lh_bme680_heater_resistance(&calib, 100, 25) ==
            lh_bme680_heater_resistance(&calib, 200, 25),
        "an under-range setpoint should clamp to 200 C");

  /*
   * Ambient temperature does not move the heater register, and that is Bosch's
   * algorithm rather than a bug here.
   *
   * The ambient term is `((ambient * par_g3) / 1000) * 256`, and it is added to
   * `var2 / 2`, which for realistic coefficients is around 2 x 10^8. For the
   * term to shift the final register value by one step it would have to change
   * `var4` by at least `var5` — about 71000 — which needs `ambient * par_g3` to
   * exceed 1.4 million. With par_g3 an int8, no temperature a sensor will ever
   * see gets close.
   *
   * So the driver's feeding the last measured temperature back in is faithful
   * to the datasheet and costs nothing, but it is not doing any work. Asserting
   * that it does — which the first version of this test did, at 0 °C against
   * 40 °C — was asserting something false about the algorithm and dressing it
   * up as a property of the driver. This asserts what is actually true, so that
   * a future part whose coefficients *do* make it matter shows up as a failure
   * here rather than as a surprise in the field.
   */
  CHECK(lh_bme680_heater_resistance(&calib, 320, 0) ==
            lh_bme680_heater_resistance(&calib, 320, 40),
        "ambient is below the resolution of Bosch's integer term at 0 vs 40 C");
  CHECK(lh_bme680_heater_resistance(&calib, 320, 0) ==
            lh_bme680_heater_resistance(&calib, 320, 127),
        "and still is at the extreme of the int8 range");
  CHECK(lh_bme680_heater_resistance(&calib, 320, 25) !=
            lh_bme680_heater_resistance(&calib, 200, 25),
        "the target temperature, by contrast, does move it");

  /* gas_wait: a 6-bit count and a 2-bit multiplier of four. */
  CHECK(lh_bme680_encode_gas_wait(0) == 0, "zero encodes as zero");
  CHECK(lh_bme680_encode_gas_wait(63) == 63, "63 fits the count field alone");
  CHECK(lh_bme680_encode_gas_wait(64) == (16u | (1u << 6)), "64 needs the first multiplier");
  CHECK(lh_bme680_encode_gas_wait(150) == (37u | (1u << 6)), "150 ms encodes as 37 x 4");
  CHECK(lh_bme680_encode_gas_wait(0xFFFF) == 0xFF,
        "an over-long wait saturates rather than wrapping to a cold heater");
}

/* ------------------------------------------------------------------------- */
/* Driver behaviour                                                          */
/* ------------------------------------------------------------------------- */

static void test_probe(void) {
  install_sim();
  CHECK(LH_BME680_DRIVER.probe(LH_BME680_ADDR_LOW) == LH_DRV_OK, "a BME680 should be recognised");

  g_sim.chip_id = 0x58; /* a BMP280 answers here too */
  CHECK(LH_BME680_DRIVER.probe(LH_BME680_ADDR_LOW) == LH_DRV_ERR_RANGE,
        "a different chip is a wiring mistake, not a bus fault");

  g_sim.present = false;
  CHECK(LH_BME680_DRIVER.probe(LH_BME680_ADDR_LOW) == LH_DRV_ERR_BUS, "no device is a bus error");
}

/**
 * The ordering constraint that produces one wrong reading if reversed.
 *
 * The chip latches ctrl_hum when ctrl_meas is written. Configure them the other
 * way round and the humidity oversampling takes effect one measurement late —
 * a single wrong sample at boot, which is exactly the kind of thing that is
 * never noticed and never reproduced.
 */
static void test_init_configures_in_the_right_order(void) {
  install_sim();
  lh_driver_ctx_t ctx;

  CHECK(lh_driver_bind(&ctx, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 0, g_sim.clock_us) ==
            LH_DRV_OK,
        "bind should succeed against a healthy chip");
  CHECK(ctx.state == LH_DRV_STATE_WARMUP, "the declared warmup should be respected");
  CHECK(g_sim.ctrl_hum_write_index >= 0, "ctrl_hum should have been written");
  CHECK(g_sim.ctrl_meas_write_index > g_sim.ctrl_hum_write_index,
        "ctrl_hum must be written before ctrl_meas (%d vs %d)", g_sim.ctrl_hum_write_index,
        g_sim.ctrl_meas_write_index);
  CHECK(lh_bme680_instances_in_use() == 1, "one slot should be taken");
}

/**
 * start_read must not wait for the conversion. This is the whole of R3.1.
 *
 * Measured against the modelled clock rather than the wall clock: what matters
 * is that the driver did not sit through the chip's 180 ms, and a host CPU
 * would finish the function in microseconds either way. If start_read ever
 * waits, this is the assertion that says so.
 */
static void test_start_read_does_not_block(void) {
  install_sim();
  lh_driver_ctx_t ctx;
  lh_driver_bind(&ctx, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 0, g_sim.clock_us);
  g_sim.clock_us += (int64_t)LH_BME680_WARMUP_MS * 1000;

  const int64_t before = g_sim.clock_us;
  CHECK(lh_driver_start_read(&ctx, g_sim.clock_us) == LH_DRV_OK, "start_read should be accepted");
  const int64_t spent_us = g_sim.clock_us - before;

  CHECK(spent_us < 5000, "start_read must not wait out the conversion; it spent %ld us",
        (long)spent_us);
  CHECK(g_sim.converting, "and the chip should now be converting");
  CHECK(g_sim.mode == 1, "in forced mode, not continuous — this is the battery decision");
  CHECK(g_sim.last_res_heat > 0, "with the heater set");
  CHECK(g_sim.last_gas_wait > 0, "and a gas wait configured");
}

/** poll answers NOT_READY for the length of the conversion, then delivers. */
static void test_poll_waits_without_blocking(void) {
  install_sim();
  lh_driver_ctx_t ctx;
  lh_reading_t reading;

  lh_driver_bind(&ctx, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 0, g_sim.clock_us);
  g_sim.clock_us += (int64_t)LH_BME680_WARMUP_MS * 1000;
  lh_driver_start_read(&ctx, g_sim.clock_us);

  int not_ready = 0;
  for (int i = 0; i < 40; i++) {
    const lh_drv_err_t err = lh_driver_poll(&ctx, &reading, 0, g_sim.clock_us);
    if (err == LH_DRV_ERR_NOT_READY) {
      not_ready++;
      g_sim.clock_us += 10000; /* the scheduler comes back in 10 ms */
      continue;
    }
    CHECK(err == LH_DRV_OK, "poll should eventually succeed, got %d", (int)err);
    break;
  }

  CHECK(not_ready >= 15, "a 180 ms conversion polled every 10 ms should not be ready ~18 times, "
                         "was %d",
        not_ready);
  CHECK(ctx.consecutive_errors == 0, "and none of that counts as a failure");
  CHECK(!lh_driver_is_faulted(&ctx), "nor retires the component");
}

/** One conversion, four channels, one timestamp. */
static void test_all_channels_share_one_measurement(void) {
  install_sim();
  lh_driver_ctx_t ctx;
  lh_reading_t reading;

  lh_driver_bind(&ctx, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 0, g_sim.clock_us);
  g_sim.clock_us += (int64_t)LH_BME680_WARMUP_MS * 1000;
  lh_driver_start_read(&ctx, g_sim.clock_us);
  g_sim.clock_us += CONVERSION_US + 1000;

  CHECK(lh_driver_poll(&ctx, &reading, LH_BME680_CH_TEMPERATURE, g_sim.clock_us) == LH_DRV_OK,
        "temperature should be available");
  const int64_t ts = reading.ts_us;
  const uint32_t reads_after_first = g_sim.reads;

  for (uint8_t channel = 1; channel < LH_BME680_CHANNELS; channel++) {
    CHECK(lh_driver_poll(&ctx, &reading, channel, g_sim.clock_us) == LH_DRV_OK,
          "channel %u should be available", (unsigned)channel);
    CHECK(reading.ts_us == ts, "every channel carries the measurement's timestamp");
    CHECK(reading.channel == channel, "and its own channel index");
  }

  CHECK(g_sim.reads == reads_after_first,
        "the later channels must come from the cached sample, not the bus (%u extra reads)",
        (unsigned)(g_sim.reads - reads_after_first));
}

/** The gas channel is reported at quality 0 when the heater did not settle. */
static void test_unstable_heater_lowers_quality(void) {
  install_sim();
  g_sim.heat_stable = false;

  lh_driver_ctx_t ctx;
  lh_reading_t reading;
  lh_driver_bind(&ctx, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 0, g_sim.clock_us);
  g_sim.clock_us += (int64_t)LH_BME680_WARMUP_MS * 1000;
  lh_driver_start_read(&ctx, g_sim.clock_us);
  g_sim.clock_us += CONVERSION_US + 1000;

  CHECK(lh_driver_poll(&ctx, &reading, LH_BME680_CH_GAS, g_sim.clock_us) == LH_DRV_OK,
        "the sample is still delivered");
  CHECK(reading.quality == 0, "but at quality 0 — the heater never reached temperature");

  CHECK(lh_driver_poll(&ctx, &reading, LH_BME680_CH_TEMPERATURE, g_sim.clock_us) == LH_DRV_OK,
        "temperature is unaffected");
  CHECK(reading.quality == 100, "and is still trustworthy");
}

/** The sensor is unplugged mid-measurement: an error, not a hang and not a crash. */
static void test_disconnection_mid_read(void) {
  install_sim();
  lh_driver_ctx_t ctx;
  lh_reading_t reading;

  lh_driver_bind(&ctx, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 0, g_sim.clock_us);
  g_sim.clock_us += (int64_t)LH_BME680_WARMUP_MS * 1000;
  lh_driver_start_read(&ctx, g_sim.clock_us);

  g_sim.present = false; /* somebody pulled the connector */
  g_sim.clock_us += CONVERSION_US + 1000;

  CHECK(lh_driver_poll(&ctx, &reading, 0, g_sim.clock_us) == LH_DRV_ERR_BUS,
        "a vanished sensor is a bus error");
  CHECK(ctx.consecutive_errors == 1, "and it counts");

  /* Three in a row and the component is out of the rotation, with the node
   * still running — the whole point of the fault threshold. */
  for (int i = 0; i < 3; i++) {
    lh_driver_start_read(&ctx, g_sim.clock_us);
    lh_driver_poll(&ctx, &reading, 0, g_sim.clock_us);
  }
  CHECK(lh_driver_is_faulted(&ctx), "a permanently absent sensor is retired");
}

/**
 * Two instances, and a third refused.
 *
 * Two is the chip's limit, not a budget: one address pin means 0x76 and 0x77
 * and nothing else on a single bus. A third bind has to fail cleanly rather
 * than quietly share another instance's calibration, which would report one
 * sensor's readings under the other's component id.
 */
static void test_instance_slots(void) {
  install_sim();
  lh_driver_ctx_t low;
  lh_driver_ctx_t high;
  lh_driver_ctx_t third;

  CHECK(lh_driver_bind(&low, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 0, g_sim.clock_us) ==
            LH_DRV_OK,
        "0x76 should bind");
  CHECK(lh_driver_bind(&high, &LH_BME680_DRIVER, LH_BME680_ADDR_HIGH, 1, g_sim.clock_us) ==
            LH_DRV_OK,
        "0x77 should bind");
  CHECK(lh_bme680_instances_in_use() == 2, "both slots taken");

  CHECK(lh_driver_bind(&third, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 2, g_sim.clock_us) !=
            LH_DRV_OK,
        "a third instance must be refused");
  CHECK(lh_bme680_instances_in_use() == 2, "and must not have taken a slot");

  /* The two are independent: different addresses, different cached samples. */
  g_sim.clock_us += (int64_t)LH_BME680_WARMUP_MS * 1000;
  lh_driver_start_read(&low, g_sim.clock_us);
  lh_driver_start_read(&high, g_sim.clock_us);
  g_sim.clock_us += CONVERSION_US + 1000;

  lh_reading_t a;
  lh_reading_t b;
  CHECK(lh_driver_poll(&low, &a, 0, g_sim.clock_us) == LH_DRV_OK, "0x76 reads");
  CHECK(lh_driver_poll(&high, &b, 0, g_sim.clock_us) == LH_DRV_OK, "0x77 reads");
}

/**
 * A failed init must give its slot back.
 *
 * Otherwise a node that retries a broken sensor exhausts both slots on it and
 * refuses the working one — a single loose wire taking out the sensor next to
 * it, which is a fault nobody would think to look for.
 */
static void test_failed_init_releases_the_slot(void) {
  install_sim();
  lh_driver_ctx_t ctx;

  /* The chip answers its id and then stops responding, so init gets past probe
   * and fails during calibration. */
  g_sim.calib1[0] = 0;
  g_sim.chip_id = LH_BME680_CHIP_ID;
  g_sim.present = true;

  /* Make the calibration read fail by removing the device right after probe. */
  CHECK(LH_BME680_DRIVER.probe(LH_BME680_ADDR_LOW) == LH_DRV_OK, "probe succeeds first");
  g_sim.present = false;

  CHECK(lh_driver_bind(&ctx, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 0, g_sim.clock_us) !=
            LH_DRV_OK,
        "bind against a vanished device should fail");
  CHECK(lh_bme680_instances_in_use() == 0, "and must not leak a slot, %u in use",
        (unsigned)lh_bme680_instances_in_use());

  /* The device comes back; the slot is available. */
  g_sim.present = true;
  CHECK(lh_driver_bind(&ctx, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 0, g_sim.clock_us) ==
            LH_DRV_OK,
        "and a later bind should succeed");
}

/** With no I/O installed the driver refuses rather than dereferencing nothing. */
static void test_missing_io(void) {
  lh_bme680_reset_instances();
  lh_bme680_set_io(NULL);

  lh_driver_ctx_t ctx;
  CHECK(lh_driver_bind(&ctx, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 0, 0) == LH_DRV_ERR_BUS,
        "no bus means no driver");
  CHECK(LH_BME680_DRIVER.probe(LH_BME680_ADDR_LOW) == LH_DRV_ERR_BUS, "and no probe");
}

/** Sleeping turns the heater off — the part that costs 12 mA if forgotten. */
static void test_sleep_turns_the_heater_off(void) {
  install_sim();
  lh_driver_ctx_t ctx;
  lh_driver_bind(&ctx, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 0, g_sim.clock_us);

  CHECK(lh_driver_sleep(&ctx) == LH_DRV_OK, "sleep should succeed");
  CHECK(g_sim.heater_off, "the heater must be switched off");
  CHECK(g_sim.mode == 0, "and the chip put back to sleep mode");
}

/* ------------------------------------------------------------------------- */
/* Benchmarks                                                                */
/* ------------------------------------------------------------------------- */

#define ROUNDS 60
#define BATCH 2000

static volatile int32_t g_sink = 0;

static double bench_median(double *samples) {
  qsort(samples, ROUNDS, sizeof(double), compare_double);
  return samples[ROUNDS / 2];
}

/** Full compensation of all four channels from raw counts. */
static double bench_compensation_us(void) {
  double samples[ROUNDS];
  lh_bme680_calib_t calib = calibration();

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      g_sink += lh_bme680_compensate_temperature(&calib, 510000u + (uint32_t)(i & 0xFF));
      g_sink += lh_bme680_compensate_pressure(&calib, 400000u);
      g_sink += lh_bme680_compensate_humidity(&calib, 25000u);
      g_sink += lh_bme680_compensate_gas(&calib, 600u, 5u);
    }
    samples[round] = (now_seconds() - started) * 1e6 / BATCH;
  }
  return bench_median(samples);
}

/** start_read, as the scheduler calls it: the non-blocking claim, timed. */
static double bench_start_read_us(void) {
  double samples[ROUNDS];
  install_sim();
  lh_driver_ctx_t ctx;
  lh_driver_bind(&ctx, &LH_BME680_DRIVER, LH_BME680_ADDR_LOW, 0, g_sim.clock_us);
  g_sim.clock_us += (int64_t)LH_BME680_WARMUP_MS * 1000;

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      /* Back to IDLE each time so the full path is measured rather than the
       * "already measuring" early return. */
      ctx.state = LH_DRV_STATE_IDLE;
      g_sink += (int32_t)lh_driver_start_read(&ctx, g_sim.clock_us);
    }
    samples[round] = (now_seconds() - started) * 1e6 / BATCH;
  }
  return bench_median(samples);
}

/* ------------------------------------------------------------------------- */

int main(void) {
#if defined(LH_SANITIZED)
  printf("LH_ENV bme680.selftest.build=sanitized\n");
#else
  printf("LH_ENV bme680.selftest.build=plain\n");
#endif

  test_calibration_parsing();
  test_temperature_compensation();
  test_pressure_compensation();
  test_humidity_compensation();
  test_gas_compensation();
  test_heater_and_gas_wait();

  test_probe();
  test_init_configures_in_the_right_order();
  test_start_read_does_not_block();
  test_poll_waits_without_blocking();
  test_all_channels_share_one_measurement();
  test_unstable_heater_lowers_quality();
  test_disconnection_mid_read();
  test_instance_slots();
  test_failed_init_releases_the_slot();
  test_missing_io();
  test_sleep_turns_the_heater_off();

  printf("LH_METRIC test.bme680.checks value=%d unit=count\n", g_checks);
  printf("LH_METRIC test.bme680.failures value=%d unit=count budget=0\n", g_failures);

  printf("LH_METRIC bench.bme680.compensation.native.p50 value=%.4f unit=us\n",
         bench_compensation_us());
  printf("LH_METRIC bench.bme680.start_read.native.p50 value=%.4f unit=us\n",
         bench_start_read_us());

  printf("LH_METRIC bme680.instances_max value=%u unit=count\n",
         (unsigned)LH_BME680_MAX_INSTANCES);
  printf("LH_METRIC bme680.warmup_declared_ms value=%u unit=ms\n", (unsigned)LH_BME680_WARMUP_MS);
  printf("LH_METRIC mem.bme680.calib value=%u unit=B\n", (unsigned)sizeof(lh_bme680_calib_t));

  /* Everything below needs the part. The budgets travel with the SKIPPED value
   * so they are attached to the measurement they were written for rather than
   * quietly dropped. */
  printf("LH_METRIC bench.bme680.start_read.esp32 value=SKIPPED unit=us budget=1000"
         " (no BME680 attached)\n");
  printf("LH_METRIC bench.bme680.poll.esp32 value=SKIPPED unit=us budget=2000"
         " (no BME680 attached)\n");
  printf("LH_METRIC bench.bme680.warmup_actual_ms value=SKIPPED unit=ms budget=200"
         " (no BME680 attached)\n");
  printf("LH_METRIC metric.bme680.i_measure_ma value=SKIPPED unit=mA"
         " (needs a lab supply; USB power cannot measure it)\n");
  printf("LH_METRIC metric.bme680.i_sleep_ua value=SKIPPED unit=uA"
         " (needs a lab supply; USB power cannot measure it)\n");
  printf("LH_METRIC test.bme680.sanity_ranges value=SKIPPED unit=count"
         " (15-30 C, 20-80 pct, 95-105 kPa on real silicon)\n");

  if (g_sink == 0x7FFFFFFF) printf("unreachable %ld\n", (long)g_sink);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
