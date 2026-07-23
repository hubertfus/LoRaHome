#include "driver_registry.h"

#include <cstring>
#include "bme680_driver.h"
#include "gpio_digital_driver.h"

namespace lorahome {

const DriverVTable DRIVER_REGISTRY[] = {
    {"bme680", bme680_init, bme680_read, bme680_warmup_ms},
    {"gpio_digital", gpio_digital_init, gpio_digital_read, gpio_digital_warmup_ms},
};

const size_t DRIVER_REGISTRY_COUNT = sizeof(DRIVER_REGISTRY) / sizeof(DRIVER_REGISTRY[0]);

const DriverVTable* findDriver(const char* driver_id) {
  for (size_t i = 0; i < DRIVER_REGISTRY_COUNT; i++) {
    if (std::strcmp(DRIVER_REGISTRY[i].driver_id, driver_id) == 0) {
      return &DRIVER_REGISTRY[i];
    }
  }
  return nullptr;
}

}  // namespace lorahome
