#include "gpio_digital_driver.h"

#include <Arduino.h>

namespace lorahome {

// The manifest's "bus": "gpio" component reuses the generic uint8_t addr
// parameter as the pin number (see packages/components/manifests/gpio_digital.json).

bool gpio_digital_init(uint8_t addr) {
  pinMode(addr, INPUT);
  return true;
}

bool gpio_digital_read(uint8_t addr, SensorReading* out, uint8_t max_readings, uint8_t* out_count) {
  if (max_readings < 1) {
    *out_count = 0;
    return false;
  }
  out[0] = {0, digitalRead(addr) ? 1.0f : 0.0f};
  *out_count = 1;
  return true;
}

uint32_t gpio_digital_warmup_ms() {
  return 0;
}

}  // namespace lorahome
