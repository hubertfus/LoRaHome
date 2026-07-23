#pragma once

#include "driver_registry.h"

namespace lorahome {

bool gpio_digital_init(uint8_t addr);
bool gpio_digital_read(uint8_t addr, SensorReading* out, uint8_t max_readings, uint8_t* out_count);
uint32_t gpio_digital_warmup_ms();

}  // namespace lorahome
