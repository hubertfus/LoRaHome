#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lorahome/driver.h"
#include "lorahome/i2c_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BME680 driver: forced mode, non-blocking, warmup-aware. Roadmap T3.3.
 *
 * The first real sensor, and the one the whole start_read/poll model in
 * driver.h was designed around. A gas measurement takes roughly 180 ms with the
 * heater on, and this driver never waits for it: start_read triggers a forced
 * conversion and returns, poll answers NOT_READY until the chip clears its
 * measuring bit. Risk R3.1 in one sentence — a blocking read here closes the
 * radio's receive window for a fifth of a second at a time, and the frames that
 * go missing go missing *only while a sensor is measuring*, which is a fault
 * that looks like a radio problem for as long as anybody is willing to look.
 *
 * Forced mode rather than continuous is a battery decision. In forced mode the
 * chip takes one measurement, returns to sleep, and draws under a microamp
 * between readings; in continuous mode it would cycle for ever whether or not
 * anybody wanted a sample, at a cost that dominates the node's power budget.
 *
 * Compensation is Bosch's integer algorithm from the datasheet, in int32/int64
 * throughout — no float anywhere, per R3.3 and CONTRIBUTING.md. The outputs are
 * already in the project's canonical scaled integers, which is a convenient
 * accident of the algorithm rather than a conversion: humidity falls out in
 * milli-percent and pressure in pascals on its own; only temperature needs a
 * factor of ten, from the datasheet's centi-degrees to our milli-degrees.
 */

/**
 * Instances a node can hold.
 *
 * Two, and that is not a budget compromise — it is the chip. The BME680's
 * address is selected by one pin, so a single I2C bus can physically carry
 * exactly two of them, at 0x76 and 0x77. A third would need a second bus, which
 * the node does not have.
 */
#define LH_BME680_MAX_INSTANCES 2u

#define LH_BME680_ADDR_LOW 0x76u
#define LH_BME680_ADDR_HIGH 0x77u

/** Who the chip says it is. A different answer means it is not a BME680. */
#define LH_BME680_CHIP_ID 0x61u

/**
 * Declared warmup, in milliseconds.
 *
 * The datasheet's figure for the gas sensor to settle after power-on. The
 * manifest declares 200 ms and so does this; T3.6's coherence test is what
 * keeps the two from drifting apart.
 */
#define LH_BME680_WARMUP_MS 200u

/** The chip is not asked for a new sample faster than this. */
#define LH_BME680_MIN_INTERVAL_MS 3000u

/** Target heater temperature, °C, and how long to hold it. Datasheet defaults. */
#define LH_BME680_HEATER_TEMP_C 320
#define LH_BME680_HEATER_DURATION_MS 150

/**
 * Ambient temperature assumed when computing the heater setpoint, °C.
 *
 * The heater resistance calculation needs an ambient figure, and the first
 * measurement has none — there is no reading yet. 25 °C is the datasheet's own
 * reference. After the first successful conversion the driver uses the measured
 * temperature instead, so the assumption costs at most one slightly mis-set
 * heater on the very first sample, which the warmup covers anyway.
 */
#define LH_BME680_AMBIENT_DEFAULT_C 25

typedef enum {
  LH_BME680_CH_TEMPERATURE = 0, /* milli-degrees Celsius */
  LH_BME680_CH_HUMIDITY = 1,    /* milli-percent RH      */
  LH_BME680_CH_PRESSURE = 2,    /* pascals               */
  LH_BME680_CH_GAS = 3,         /* ohms                  */
  LH_BME680_CHANNELS = 4,
} lh_bme680_channel_t;

/**
 * Installs the I2C access this driver uses.
 *
 * Global rather than per-instance because the bus is: every BME680 on a node
 * hangs off the same two wires, and giving each instance its own copy of the
 * same three function pointers would only create the possibility of them
 * differing. Must be called before any component is bound.
 */
void lh_bme680_set_io(const lh_i2c_io_t *io);

/** The registry entry. Bind it through lh_driver_bind() like any other. */
extern const lh_driver_vtable_t LH_BME680_DRIVER;

/**
 * Releases every instance slot.
 *
 * For tests and for a re-scan after a configuration change. Not called during
 * normal operation: a node binds its components once at boot and keeps them.
 */
void lh_bme680_reset_instances(void);

/** Instance slots currently in use. Diagnostics, and the slot-leak test. */
uint8_t lh_bme680_instances_in_use(void);

/*
 * The compensation arithmetic, exposed for testing.
 *
 * Not part of the driver's interface — nothing in the firmware calls these
 * directly. They are here because they are the part of this file most likely to
 * be wrong in a way no amount of running it will reveal: a sign error in the
 * pressure polynomial produces plausible numbers that are simply not the
 * pressure, and the only way to catch that is to feed known inputs to the
 * arithmetic in isolation.
 */

/** Calibration coefficients, as read from the chip's two register blocks. */
typedef struct {
  uint16_t par_t1;
  int16_t par_t2;
  int8_t par_t3;

  uint16_t par_p1;
  int16_t par_p2;
  int8_t par_p3;
  int16_t par_p4;
  int16_t par_p5;
  int8_t par_p6;
  int8_t par_p7;
  int16_t par_p8;
  int16_t par_p9;
  uint8_t par_p10;

  uint16_t par_h1;
  uint16_t par_h2;
  int8_t par_h3;
  int8_t par_h4;
  int8_t par_h5;
  uint8_t par_h6;
  int8_t par_h7;

  int8_t par_g1;
  int16_t par_g2;
  int8_t par_g3;

  uint8_t res_heat_range;
  int8_t res_heat_val;
  int8_t range_sw_err;

  /* Carried between the four compensations: temperature has to be computed
   * first because the other three are corrected against it. */
  int32_t t_fine;
} lh_bme680_calib_t;

/** Bytes read from 0x89 and from 0xE1; the two blocks the coefficients live in. */
#define LH_BME680_CALIB_BLOCK1 25u
#define LH_BME680_CALIB_BLOCK2 16u

/**
 * Parses the calibration blocks into coefficients.
 *
 * `extra` carries registers 0x00, 0x02 and 0x04 in that order — the heater
 * trim values, which Bosch keeps outside both blocks.
 */
void lh_bme680_parse_calibration(
    const uint8_t block1[LH_BME680_CALIB_BLOCK1],
    const uint8_t block2[LH_BME680_CALIB_BLOCK2],
    const uint8_t extra[3],
    lh_bme680_calib_t *out);

/** Temperature in milli-degrees Celsius. Sets calib->t_fine for the others. */
int32_t lh_bme680_compensate_temperature(lh_bme680_calib_t *calib, uint32_t temp_adc);

/** Pressure in pascals. Requires t_fine from a preceding temperature call. */
int32_t lh_bme680_compensate_pressure(const lh_bme680_calib_t *calib, uint32_t press_adc);

/** Relative humidity in milli-percent, clamped to 0..100000. */
int32_t lh_bme680_compensate_humidity(const lh_bme680_calib_t *calib, uint16_t hum_adc);

/** Gas resistance in ohms. */
int32_t lh_bme680_compensate_gas(
    const lh_bme680_calib_t *calib, uint16_t gas_adc, uint8_t gas_range);

/** Heater register value for a target temperature at a given ambient. */
uint8_t lh_bme680_heater_resistance(
    const lh_bme680_calib_t *calib, int16_t target_c, int16_t ambient_c);

/** Encodes a heater duration in milliseconds into the gas_wait register form. */
uint8_t lh_bme680_encode_gas_wait(uint16_t duration_ms);

#ifdef __cplusplus
}
#endif
