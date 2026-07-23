#pragma once

#include <cstddef>
#include <cstdint>

namespace lorahome {

struct SensorReading {
  uint8_t output_index; // index into the manifest's `outputs[]`
  float value;
};

using DriverInitFn = bool (*)(uint8_t addr);
using DriverReadFn = bool (*)(uint8_t addr, SensorReading* out, uint8_t max_readings, uint8_t* out_count);
using DriverWarmupFn = uint32_t (*)();

/**
 * Static per-driver function table — see CONTRIBUTING.md §1.2. New sensors
 * get a new entry here (and a matching manifest in packages/components),
 * never a dynamically allocated object.
 */
struct DriverVTable {
  const char* driver_id; // must match the manifest's "id"
  DriverInitFn init;
  DriverReadFn read;
  DriverWarmupFn get_warmup_ms;
};

extern const DriverVTable DRIVER_REGISTRY[];
extern const size_t DRIVER_REGISTRY_COUNT;

/** Returns nullptr if no driver matches `driver_id`. */
const DriverVTable* findDriver(const char* driver_id);

}  // namespace lorahome
