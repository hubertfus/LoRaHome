/*
 * BME680 driver: forced mode, non-blocking. Roadmap T3.3. Contract in bme680.h.
 *
 * Two halves that have nothing to do with each other. The state machine is
 * small and is about never waiting: trigger a conversion, come back later. The
 * compensation arithmetic is Bosch's, transcribed from the datasheet, and is
 * about getting a number right that no amount of running the code will tell you
 * is wrong — a sign error in the pressure polynomial yields a plausible figure
 * that is simply not the pressure.
 *
 * Nothing here allocates. Instance state lives in a static array of two, which
 * is not a budget compromise but the chip: one address pin, two possible
 * addresses, so a single bus physically carries exactly two of them.
 */
#include "lorahome/bme680.h"

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Register map                                                              */
/* ------------------------------------------------------------------------- */

#define REG_RES_HEAT_VAL 0x00u
#define REG_RES_HEAT_RANGE 0x02u
#define REG_RANGE_SW_ERR 0x04u
#define REG_MEAS_STATUS_0 0x1Du
#define REG_PRESS_MSB 0x1Fu
#define REG_GAS_R_MSB 0x2Au
#define REG_GAS_WAIT_0 0x64u
#define REG_RES_HEAT_0 0x5Au
#define REG_CTRL_GAS_0 0x70u
#define REG_CTRL_GAS_1 0x71u
#define REG_CTRL_HUM 0x72u
#define REG_CTRL_MEAS 0x74u
#define REG_CONFIG 0x75u
#define REG_CHIP_ID 0xD0u
#define REG_SOFT_RESET 0xE0u
#define REG_CALIB_BLOCK1 0x89u
#define REG_CALIB_BLOCK2 0xE1u

#define CMD_SOFT_RESET 0xB6u

#define MODE_SLEEP 0x00u
#define MODE_FORCED 0x01u

/* Oversampling: x2 temperature, x16 pressure, x1 humidity — Bosch's own
 * recommendation for indoor environmental monitoring, and the reason pressure
 * is the only channel worth filtering. */
#define OSRS_T 2u /* x2  */
#define OSRS_P 5u /* x16 */
#define OSRS_H 1u /* x1  */
#define FILTER_COEFF 3u

#define STATUS_NEW_DATA 0x80u
#define STATUS_GAS_MEASURING 0x40u
#define STATUS_MEASURING 0x20u

#define GAS_R_LSB_GAS_VALID 0x20u
#define GAS_R_LSB_HEAT_STAB 0x10u

/* ------------------------------------------------------------------------- */
/* Instance state                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
  bool in_use;
  uint8_t addr;
  lh_bme680_calib_t calib;

  /**
   * Ambient temperature used for the heater setpoint.
   *
   * Seeded from the datasheet's 25 °C reference and replaced by the measured
   * temperature after the first conversion. The heater resistance depends on
   * it, and the first measurement has no reading to depend on — the warmup
   * covers the one slightly mis-set heater this costs.
   */
  int16_t ambient_c;

  /* The last completed measurement, all four channels at once. The chip
   * produces them from a single conversion, so serving later channels from
   * here rather than re-reading is both faster and more correct: it is the
   * same sample, and it carries the same timestamp. */
  bool have_sample;
  int32_t values[LH_BME680_CHANNELS];
  uint8_t quality[LH_BME680_CHANNELS];
  int64_t sample_ts_us;
} instance_t;

static instance_t g_instances[LH_BME680_MAX_INSTANCES];
static lh_i2c_io_t g_io;
static bool g_io_set = false;

void lh_bme680_set_io(const lh_i2c_io_t *io) {
  if (io == NULL) {
    memset(&g_io, 0, sizeof g_io);
    g_io_set = false;
    return;
  }
  g_io = *io;
  g_io_set = io->read != NULL && io->write != NULL;
}

void lh_bme680_reset_instances(void) { memset(g_instances, 0, sizeof g_instances); }

uint8_t lh_bme680_instances_in_use(void) {
  uint8_t count = 0;
  for (uint8_t i = 0; i < LH_BME680_MAX_INSTANCES; i++) {
    if (g_instances[i].in_use) count++;
  }
  return count;
}

static bool read_regs(uint8_t addr, uint8_t reg, uint8_t *out, uint8_t len) {
  return g_io_set && g_io.read(addr, reg, out, len, g_io.user);
}

static bool write_reg(uint8_t addr, uint8_t reg, uint8_t value) {
  return g_io_set && g_io.write(addr, reg, value, g_io.user);
}

static int64_t now_us(void) { return g_io.now_us != NULL ? g_io.now_us(g_io.user) : 0; }

/** The instance a bound context owns. The scratch holds nothing but its index. */
static instance_t *instance_of(lh_driver_ctx_t *ctx) {
  const uint8_t slot = ctx->scratch[0];
  if (slot >= LH_BME680_MAX_INSTANCES) return NULL;
  instance_t *inst = &g_instances[slot];
  return inst->in_use ? inst : NULL;
}

/* ------------------------------------------------------------------------- */
/* Compensation — Bosch's integer algorithm, datasheet 5.3.2                 */
/* ------------------------------------------------------------------------- */

static uint16_t u16_of(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[1] << 8) | (uint16_t)p[0]);
}

static int16_t s16_of(const uint8_t *p) { return (int16_t)u16_of(p); }

void lh_bme680_parse_calibration(
    const uint8_t block1[LH_BME680_CALIB_BLOCK1],
    const uint8_t block2[LH_BME680_CALIB_BLOCK2],
    const uint8_t extra[3],
    lh_bme680_calib_t *out) {
  if (block1 == NULL || block2 == NULL || extra == NULL || out == NULL) return;
  memset(out, 0, sizeof *out);

  /* block1 starts at 0x89, so index n is register 0x89 + n. */
  out->par_t2 = s16_of(&block1[1]);  /* 0x8A/0x8B */
  out->par_t3 = (int8_t)block1[3];   /* 0x8C      */
  out->par_p1 = u16_of(&block1[5]);  /* 0x8E/0x8F */
  out->par_p2 = s16_of(&block1[7]);  /* 0x90/0x91 */
  out->par_p3 = (int8_t)block1[9];   /* 0x92      */
  out->par_p4 = s16_of(&block1[11]); /* 0x94/0x95 */
  out->par_p5 = s16_of(&block1[13]); /* 0x96/0x97 */
  out->par_p7 = (int8_t)block1[15];  /* 0x98      */
  out->par_p6 = (int8_t)block1[16];  /* 0x99      */
  out->par_p8 = s16_of(&block1[19]); /* 0x9C/0x9D */
  out->par_p9 = s16_of(&block1[21]); /* 0x9E/0x9F */
  out->par_p10 = block1[23];         /* 0xA0      */

  /* block2 starts at 0xE1. par_h1 and par_h2 share the nibbles of 0xE2 — the
   * one place in this map where a byte belongs to two coefficients, and the
   * usual place to get it backwards. */
  out->par_h2 = (uint16_t)(((uint16_t)block2[0] << 4) | ((uint16_t)block2[1] >> 4));
  out->par_h1 = (uint16_t)(((uint16_t)block2[2] << 4) | ((uint16_t)block2[1] & 0x0Fu));
  out->par_h3 = (int8_t)block2[3];   /* 0xE4      */
  out->par_h4 = (int8_t)block2[4];   /* 0xE5      */
  out->par_h5 = (int8_t)block2[5];   /* 0xE6      */
  out->par_h6 = block2[6];           /* 0xE7      */
  out->par_h7 = (int8_t)block2[7];   /* 0xE8      */
  out->par_t1 = u16_of(&block2[8]);  /* 0xE9/0xEA */
  out->par_g2 = s16_of(&block2[10]); /* 0xEB/0xEC */
  out->par_g1 = (int8_t)block2[12];  /* 0xED      */
  out->par_g3 = (int8_t)block2[13];  /* 0xEE      */

  out->res_heat_val = (int8_t)extra[0];
  out->res_heat_range = (uint8_t)((extra[1] & 0x30u) >> 4);
  out->range_sw_err = (int8_t)((int8_t)(extra[2] & 0xF0u) / 16);
}

int32_t lh_bme680_compensate_temperature(lh_bme680_calib_t *calib, uint32_t temp_adc) {
  if (calib == NULL) return 0;

  const int32_t var1 = ((int32_t)(temp_adc >> 3)) - ((int32_t)calib->par_t1 << 1);
  const int32_t var2 = (var1 * (int32_t)calib->par_t2) >> 11;
  int32_t var3 = ((var1 >> 1) * (var1 >> 1)) >> 12;
  var3 = (var3 * ((int32_t)calib->par_t3 << 4)) >> 14;

  calib->t_fine = var2 + var3;

  /* The datasheet yields centi-degrees; the project's canonical unit for
   * temperature is milli-degrees, so this is the one channel that needs a
   * conversion at all. */
  const int32_t centi = ((calib->t_fine * 5) + 128) >> 8;
  return centi * 10;
}

int32_t lh_bme680_compensate_pressure(const lh_bme680_calib_t *calib, uint32_t press_adc) {
  if (calib == NULL) return 0;

  int32_t var1 = (((int32_t)calib->t_fine) >> 1) - 64000;
  int32_t var2 = ((((var1 >> 2) * (var1 >> 2)) >> 11) * (int32_t)calib->par_p6) >> 2;
  var2 = var2 + ((var1 * (int32_t)calib->par_p5) << 1);
  var2 = (var2 >> 2) + ((int32_t)calib->par_p4 << 16);
  var1 = (((((var1 >> 2) * (var1 >> 2)) >> 13) * ((int32_t)calib->par_p3 << 5)) >> 3) +
         (((int32_t)calib->par_p2 * var1) >> 1);
  var1 = var1 >> 18;
  var1 = ((32768 + var1) * (int32_t)calib->par_p1) >> 15;

  /* A zero here would be a division by zero two lines down. It means par_p1 is
   * zero or the coefficients were never read — an uncalibrated chip, not a
   * pressure of nothing, so 0 Pa is returned and the caller's range check in
   * the manifest rejects it. */
  if (var1 == 0) return 0;

  /*
   * A deliberate divergence from the reference, in 64-bit rather than 32.
   *
   * Bosch computes this product in uint32_t and then selects between two
   * scalings depending on whether it passed 2^30 — a branch that exists purely
   * to keep a 32-bit intermediate from overflowing. It works while the product
   * stays under 2^31 and stops working above it, and the measured symptom was a
   * 1954 Pa step *upwards* in an otherwise falling curve, at a raw count
   * corresponding to about 106 kPa. Not a crash and not an obviously bad
   * number: twenty hectopascals appearing out of nothing at one end of the
   * scale, on a channel a rule engine compares against a threshold.
   *
   * In int64 the branch is unnecessary and the result is exact. It agrees with
   * the reference to within a pascal everywhere the reference is valid, and
   * keeps working where the reference wraps. The cost is one 64-bit divide
   * every three seconds.
   */
  const int64_t scaled = (int64_t)(1048576 - (int32_t)press_adc - (var2 >> 12)) * 3125;
  int32_t press = (int32_t)((scaled * 2) / var1);

  /*
   * The correction terms, also widened. Same reason, different overflow.
   *
   * `cube` is `(press >> 8)` raised to the third power and multiplied by
   * par_p10. At the top of the raw range that is around 6 x 10^9 in an int32,
   * and the wrap showed up as a second 2 kPa step in the sweep — the first fix
   * moved the failure rather than removing it, which is what a sweep over the
   * whole domain is for.
   */
  const int64_t p = press;
  const int64_t corr1 = ((int64_t)calib->par_p9 * (((p >> 3) * (p >> 3)) >> 13)) >> 12;
  const int64_t corr2 = ((p >> 2) * (int64_t)calib->par_p8) >> 13;
  const int64_t corr3 = ((p >> 8) * (p >> 8) * (p >> 8) * (int64_t)calib->par_p10) >> 17;

  const int64_t result = p + ((corr1 + corr2 + corr3 + ((int64_t)calib->par_p7 << 7)) >> 4);
  if (result > INT32_MAX) return INT32_MAX;
  if (result < 0) return 0;
  return (int32_t)result;
}

int32_t lh_bme680_compensate_humidity(const lh_bme680_calib_t *calib, uint16_t hum_adc) {
  if (calib == NULL) return 0;

  const int32_t temp_scaled = ((calib->t_fine * 5) + 128) >> 8;

  const int32_t var1 = (int32_t)hum_adc - ((int32_t)calib->par_h1 * 16) -
                       (((temp_scaled * (int32_t)calib->par_h3) / 100) >> 1);
  const int32_t var2 =
      ((int32_t)calib->par_h2 *
       (((temp_scaled * (int32_t)calib->par_h4) / 100) +
        (((temp_scaled * ((temp_scaled * (int32_t)calib->par_h5) / 100)) >> 6) / 100) +
        (int32_t)(1 << 14))) >>
      10;
  /*
   * The second divergence, and the one that matters more.
   *
   * `var5` squares `var3 >> 14`, and in int32 that overflows once the raw count
   * passes roughly 54000. What came out was not a large number or a small one:
   * humidity went from a clamped 100% to exactly 0% at a single raw count and
   * stayed there — a sensor reading bone dry because a 16-bit field was two
   * thousand counts higher than the polynomial's domain.
   *
   * A working BME680 saturates at 100% RH somewhere around 27500, so nothing
   * healthy reaches this. A corrupted I2C read does, and that is precisely the
   * case: 0% is a plausible number, it is what a rule engine would act on, and
   * nothing anywhere would record that it came from arithmetic rather than from
   * the air. Widening the intermediates makes the function monotonic over the
   * entire 16-bit domain, so an impossible raw count saturates at 100% instead
   * of wrapping to a lie.
   */
  const int64_t var3 = (int64_t)var1 * var2;
  int32_t var4 = (int32_t)calib->par_h6 << 7;
  var4 = (var4 + ((temp_scaled * (int32_t)calib->par_h7) / 100)) >> 4;
  const int64_t var5 = ((var3 >> 14) * (var3 >> 14)) >> 10;
  const int64_t var6 = ((int64_t)var4 * var5) >> 1;

  /* Already milli-percent: the algorithm's own scale happens to be the
   * project's. Clamped rather than trusted, because the polynomial is not
   * bounded and a sensor reporting 104% would sail into a rule as a number. */
  const int64_t humidity = (((var3 + var6) >> 10) * 1000) >> 12;
  if (humidity > 100000) return 100000;
  if (humidity < 0) return 0;
  return (int32_t)humidity;
}

/*
 * The two range-correction tables from the datasheet.
 *
 * Transcribed, not derived. They exist because the gas ADC's scale changes with
 * the range the chip selected, and there is no closed form for the correction.
 */
static const uint32_t GAS_LOOKUP_K1[16] = {
    2147483647u, 2147483647u, 2147483647u, 2147483647u, 2147483647u, 2126008810u,
    2147483647u, 2130303777u, 2147483647u, 2147483647u, 2143188679u, 2136746228u,
    2147483647u, 2126008810u, 2147483647u, 2147483647u};

static const uint32_t GAS_LOOKUP_K2[16] = {
    4096000000u, 2048000000u, 1024000000u, 512000000u, 255744255u, 127110228u,
    64000000u,   32258064u,   16016016u,   8000000u,   4000000u,   2000000u,
    1000000u,    500000u,     250000u,     125000u};

int32_t lh_bme680_compensate_gas(
    const lh_bme680_calib_t *calib, uint16_t gas_adc, uint8_t gas_range) {
  if (calib == NULL || gas_range > 15u) return 0;

  const int64_t var1 =
      ((int64_t)(1340 + (5 * (int64_t)calib->range_sw_err)) * (int64_t)GAS_LOOKUP_K1[gas_range]) >>
      16;
  const int64_t var2 = (((int64_t)gas_adc << 15) - (int64_t)16777216) + var1;
  const int64_t var3 = ((int64_t)GAS_LOOKUP_K2[gas_range] * var1) >> 9;

  /* var2 is a denominator and the datasheet does not say it cannot be zero. It
   * can, for an ADC reading of exactly the wrong value, and a division by zero
   * on an ESP32 is a reset — a sensor fault turning into a reboot loop. */
  if (var2 == 0) return 0;

  const int64_t ohms = (var3 + (var2 >> 1)) / var2;
  return (ohms > INT32_MAX) ? INT32_MAX : (int32_t)ohms;
}

uint8_t lh_bme680_heater_resistance(
    const lh_bme680_calib_t *calib, int16_t target_c, int16_t ambient_c) {
  if (calib == NULL) return 0;

  /* The chip's heater is specified only to 400 °C; asking for more produces a
   * register value that means something else entirely. */
  int32_t target = target_c;
  if (target > 400) target = 400;
  if (target < 200) target = 200;

  const int32_t var1 = (((int32_t)ambient_c * (int32_t)calib->par_g3) / 1000) * 256;
  const int32_t var2 =
      ((int32_t)calib->par_g1 + 784) *
      (((((int32_t)calib->par_g2 + 154009) * target * 5) / 100 + 3276800) / 10);
  const int32_t var3 = var1 + (var2 / 2);
  const int32_t var4 = var3 / ((int32_t)calib->res_heat_range + 4);
  const int32_t var5 = (131 * (int32_t)calib->res_heat_val) + 65536;
  if (var5 == 0) return 0;

  const int32_t res_x100 = ((var4 / var5) - 250) * 34;
  const int32_t res = (res_x100 + 50) / 100;
  if (res < 0) return 0;
  if (res > 255) return 255;
  return (uint8_t)res;
}

uint8_t lh_bme680_encode_gas_wait(uint16_t duration_ms) {
  /* The register holds a 6-bit count and a 2-bit multiplier of 4. 0xFF is the
   * longest expressible wait; anything above it saturates rather than wrapping
   * to a very short heat, which would silently produce cold-sensor readings. */
  if (duration_ms >= 0xFC0u) return 0xFFu;

  uint16_t value = duration_ms;
  uint8_t factor = 0;
  while (value > 0x3Fu) {
    value /= 4u;
    factor++;
  }
  return (uint8_t)(value | (uint8_t)(factor << 6));
}

/* ------------------------------------------------------------------------- */
/* Driver                                                                    */
/* ------------------------------------------------------------------------- */

static lh_drv_err_t bme680_probe(uint8_t bus_addr) {
  uint8_t chip_id = 0;
  if (!read_regs(bus_addr, REG_CHIP_ID, &chip_id, 1)) return LH_DRV_ERR_BUS;
  /* A device that answers but is not a BME680 is a wiring or manifest mistake,
   * not a bus fault, and reporting it as one sends somebody to check the
   * pull-ups on a bus that is working perfectly. */
  return (chip_id == LH_BME680_CHIP_ID) ? LH_DRV_OK : LH_DRV_ERR_RANGE;
}

/** Reads calibration and writes the measurement configuration. */
static lh_drv_err_t configure(instance_t *inst) {
  uint8_t block1[LH_BME680_CALIB_BLOCK1];
  uint8_t block2[LH_BME680_CALIB_BLOCK2];
  uint8_t extra[3];

  if (!read_regs(inst->addr, REG_CALIB_BLOCK1, block1, LH_BME680_CALIB_BLOCK1)) {
    return LH_DRV_ERR_BUS;
  }
  if (!read_regs(inst->addr, REG_CALIB_BLOCK2, block2, LH_BME680_CALIB_BLOCK2)) {
    return LH_DRV_ERR_BUS;
  }
  if (!read_regs(inst->addr, REG_RES_HEAT_VAL, &extra[0], 1)) return LH_DRV_ERR_BUS;
  if (!read_regs(inst->addr, REG_RES_HEAT_RANGE, &extra[1], 1)) return LH_DRV_ERR_BUS;
  if (!read_regs(inst->addr, REG_RANGE_SW_ERR, &extra[2], 1)) return LH_DRV_ERR_BUS;

  lh_bme680_parse_calibration(block1, block2, extra, &inst->calib);

  /* Humidity oversampling must be written before ctrl_meas: the chip latches
   * ctrl_hum when ctrl_meas is written, so the other order configures the
   * humidity channel one measurement late. */
  if (!write_reg(inst->addr, REG_CTRL_HUM, OSRS_H)) return LH_DRV_ERR_BUS;
  if (!write_reg(inst->addr, REG_CONFIG, (uint8_t)(FILTER_COEFF << 2))) return LH_DRV_ERR_BUS;
  if (!write_reg(inst->addr, REG_CTRL_MEAS, (uint8_t)((OSRS_T << 5) | (OSRS_P << 2) | MODE_SLEEP))) {
    return LH_DRV_ERR_BUS;
  }
  return LH_DRV_OK;
}

static lh_drv_err_t bme680_init(lh_driver_ctx_t *ctx, uint8_t bus_addr) {
  if (!g_io_set) return LH_DRV_ERR_BUS;

  const lh_drv_err_t probed = bme680_probe(bus_addr);
  if (probed != LH_DRV_OK) return probed;

  uint8_t slot = 0xFFu;
  for (uint8_t i = 0; i < LH_BME680_MAX_INSTANCES; i++) {
    if (!g_instances[i].in_use) {
      slot = i;
      break;
    }
  }
  /* Out of slots is a configuration error caught at bind time rather than a
   * silent third instance sharing another's calibration. */
  if (slot == 0xFFu) return LH_DRV_ERR_STATE;

  instance_t *inst = &g_instances[slot];
  memset(inst, 0, sizeof *inst);
  inst->in_use = true;
  inst->addr = bus_addr;
  inst->ambient_c = LH_BME680_AMBIENT_DEFAULT_C;

  if (!write_reg(bus_addr, REG_SOFT_RESET, CMD_SOFT_RESET)) {
    inst->in_use = false;
    return LH_DRV_ERR_BUS;
  }

  const lh_drv_err_t err = configure(inst);
  if (err != LH_DRV_OK) {
    /* Releasing the slot matters: a node that retries a failing component would
     * otherwise exhaust two slots on one broken sensor and refuse the good one. */
    inst->in_use = false;
    return err;
  }

  ctx->scratch[0] = slot;
  return LH_DRV_OK;
}

static lh_drv_err_t bme680_start_read(lh_driver_ctx_t *ctx) {
  instance_t *inst = instance_of(ctx);
  if (inst == NULL) return LH_DRV_ERR_STATE;

  inst->have_sample = false;

  const uint8_t heat = lh_bme680_heater_resistance(
      &inst->calib, LH_BME680_HEATER_TEMP_C, inst->ambient_c);
  if (!write_reg(inst->addr, REG_RES_HEAT_0, heat)) return LH_DRV_ERR_BUS;
  if (!write_reg(inst->addr, REG_GAS_WAIT_0, lh_bme680_encode_gas_wait(LH_BME680_HEATER_DURATION_MS))) {
    return LH_DRV_ERR_BUS;
  }
  /* run_gas, heater profile 0. */
  if (!write_reg(inst->addr, REG_CTRL_GAS_1, 0x10u)) return LH_DRV_ERR_BUS;

  /* The last write is the one that starts the conversion, and it is the only
   * thing in this function that takes any time on the chip's side. Everything
   * above it is register setup; nothing here waits for a result. */
  const uint8_t ctrl_meas = (uint8_t)((OSRS_T << 5) | (OSRS_P << 2) | MODE_FORCED);
  if (!write_reg(inst->addr, REG_CTRL_MEAS, ctrl_meas)) return LH_DRV_ERR_BUS;

  return LH_DRV_OK;
}

/** Reads the completed conversion out of the chip and compensates all four channels. */
static lh_drv_err_t collect(instance_t *inst) {
  uint8_t raw[8]; /* 0x1F..0x26: pressure, temperature, humidity */
  if (!read_regs(inst->addr, REG_PRESS_MSB, raw, 8)) return LH_DRV_ERR_BUS;

  uint8_t gas[2]; /* 0x2A..0x2B */
  if (!read_regs(inst->addr, REG_GAS_R_MSB, gas, 2)) return LH_DRV_ERR_BUS;

  const uint32_t press_adc =
      ((uint32_t)raw[0] << 12) | ((uint32_t)raw[1] << 4) | ((uint32_t)raw[2] >> 4);
  const uint32_t temp_adc =
      ((uint32_t)raw[3] << 12) | ((uint32_t)raw[4] << 4) | ((uint32_t)raw[5] >> 4);
  const uint16_t hum_adc = (uint16_t)(((uint16_t)raw[6] << 8) | (uint16_t)raw[7]);

  const uint16_t gas_adc = (uint16_t)(((uint16_t)gas[0] << 2) | ((uint16_t)gas[1] >> 6));
  const uint8_t gas_range = (uint8_t)(gas[1] & 0x0Fu);
  const bool gas_valid = (gas[1] & GAS_R_LSB_GAS_VALID) != 0;
  const bool heat_stable = (gas[1] & GAS_R_LSB_HEAT_STAB) != 0;

  /* Temperature first, always: the other three are corrected against t_fine,
   * and computing them from a stale one silently reports last cycle's
   * correction applied to this cycle's raw counts. */
  inst->values[LH_BME680_CH_TEMPERATURE] =
      lh_bme680_compensate_temperature(&inst->calib, temp_adc);
  inst->values[LH_BME680_CH_PRESSURE] = lh_bme680_compensate_pressure(&inst->calib, press_adc);
  inst->values[LH_BME680_CH_HUMIDITY] = lh_bme680_compensate_humidity(&inst->calib, hum_adc);
  inst->values[LH_BME680_CH_GAS] = lh_bme680_compensate_gas(&inst->calib, gas_adc, gas_range);

  inst->quality[LH_BME680_CH_TEMPERATURE] = 100;
  inst->quality[LH_BME680_CH_PRESSURE] = 100;
  inst->quality[LH_BME680_CH_HUMIDITY] = 100;
  /*
   * The gas channel is the one that lies.
   *
   * The chip reports whether the reading is valid and whether the heater
   * reached its setpoint, and a gas resistance taken from a heater that never
   * got hot is a number with no meaning attached. Reporting it at quality 0
   * rather than dropping it keeps the sample in telemetry — an operator can see
   * the heater is failing — while the rule engine refuses to act on it. That is
   * R3.6: the sensor that answers and is wrong.
   */
  inst->quality[LH_BME680_CH_GAS] = (gas_valid && heat_stable) ? 100 : 0;

  /* Feed the measured temperature back into the next heater calculation. */
  inst->ambient_c = (int16_t)(inst->values[LH_BME680_CH_TEMPERATURE] / 1000);

  inst->sample_ts_us = now_us();
  inst->have_sample = true;
  return LH_DRV_OK;
}

static lh_drv_err_t bme680_poll(lh_driver_ctx_t *ctx, lh_reading_t *out, uint8_t channel) {
  instance_t *inst = instance_of(ctx);
  if (inst == NULL) return LH_DRV_ERR_STATE;
  if (channel >= LH_BME680_CHANNELS) return LH_DRV_ERR_NO_CHANNEL;

  if (!inst->have_sample) {
    uint8_t status = 0;
    if (!read_regs(inst->addr, REG_MEAS_STATUS_0, &status, 1)) return LH_DRV_ERR_BUS;

    /* Both bits, not just `measuring`: the gas conversion runs after the
     * environmental one, and reading the registers between the two yields a
     * complete pressure and a gas resistance from the previous cycle. */
    if ((status & (STATUS_MEASURING | STATUS_GAS_MEASURING)) != 0) return LH_DRV_ERR_NOT_READY;
    if ((status & STATUS_NEW_DATA) == 0) return LH_DRV_ERR_NOT_READY;

    const lh_drv_err_t err = collect(inst);
    if (err != LH_DRV_OK) return err;
  }

  out->value = inst->values[channel];
  out->quality = inst->quality[channel];
  out->ts_us = inst->sample_ts_us;
  out->channel = channel;
  return LH_DRV_OK;
}

static lh_drv_err_t bme680_sleep(lh_driver_ctx_t *ctx) {
  instance_t *inst = instance_of(ctx);
  if (inst == NULL) return LH_DRV_ERR_STATE;

  /* Forced mode returns to sleep on its own after each conversion, so the only
   * thing that needs saying is that the heater must stay off — it is the part
   * that would otherwise keep drawing 12 mA from a battery. */
  if (!write_reg(inst->addr, REG_CTRL_GAS_0, 0x08u)) return LH_DRV_ERR_BUS;
  if (!write_reg(inst->addr, REG_CTRL_MEAS, (uint8_t)((OSRS_T << 5) | (OSRS_P << 2) | MODE_SLEEP))) {
    return LH_DRV_ERR_BUS;
  }
  inst->have_sample = false;
  return LH_DRV_OK;
}

const lh_driver_vtable_t LH_BME680_DRIVER = {
    .name = "bme680",
    /* 16, matching packages/components/manifests/bme680.json. Never changed,
     * never reused — see docs/type-ids.md and risk R3.4. */
    .type_id = 16,
    .channel_count = LH_BME680_CHANNELS,
    .warmup_ms = LH_BME680_WARMUP_MS,
    .min_interval_ms = LH_BME680_MIN_INTERVAL_MS,
    .probe = bme680_probe,
    .init = bme680_init,
    .start_read = bme680_start_read,
    .poll = bme680_poll,
    .sleep = bme680_sleep,
};
