#pragma once

#include "driver_registry.h"

namespace lorahome {

bool bme680_init(uint8_t addr);
bool bme680_read(uint8_t addr, SensorReading* out, uint8_t max_readings, uint8_t* out_count);
uint32_t bme680_warmup_ms();

}  // namespace lorahome
