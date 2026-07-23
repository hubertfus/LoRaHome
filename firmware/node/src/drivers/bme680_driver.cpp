#include "bme680_driver.h"

#include <Adafruit_BME680.h>

namespace lorahome {

namespace {
// One static instance — no dynamic allocation (CONTRIBUTING.md §1.1).
// Only one BME680 per node is supported in V1; LORAHOME_MAX_COMPONENTS still
// bounds the overall component count across all driver types.
Adafruit_BME680 g_bme680;
bool g_initialized = false;
}  // namespace

bool bme680_init(uint8_t addr) {
  g_initialized = g_bme680.begin(addr);
  if (g_initialized) {
    g_bme680.setTemperatureOversampling(BME680_OS_2X);
    g_bme680.setHumidityOversampling(BME680_OS_1X);
    g_bme680.setPressureOversampling(BME680_OS_16X);
    g_bme680.setIIRFilterSize(BME680_FILTER_SIZE_3);
    g_bme680.setGasHeater(320, 150); // 320°C for 150ms, per Bosch's recommended profile
  }
  return g_initialized;
}

bool bme680_read(uint8_t addr, SensorReading* out, uint8_t max_readings, uint8_t* out_count) {
  (void)addr;
  if (!g_initialized || max_readings < 4) {
    *out_count = 0;
    return false;
  }
  if (!g_bme680.performReading()) {
    *out_count = 0;
    return false;
  }

  // Output indices must match packages/components/manifests/bme680.json's outputs[] order.
  out[0] = {0, g_bme680.temperature};
  out[1] = {1, g_bme680.humidity};
  out[2] = {2, g_bme680.pressure / 100.0f}; // Pa -> hPa
  out[3] = {3, static_cast<float>(g_bme680.gas_resistance)};
  *out_count = 4;
  return true;
}

uint32_t bme680_warmup_ms() {
  return 300; // matches manifest default for "warmup_ms"
}

}  // namespace lorahome
