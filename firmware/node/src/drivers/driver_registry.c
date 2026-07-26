/*
 * The node image's driver registry. Roadmap T3.1 and T3.6.
 *
 * An explicit array, not linker-section magic. The roadmap is blunt about why:
 * section attributes work until a toolchain upgrade, and when they stop working
 * they do it silently — the drivers are simply not there, and the node reports
 * an empty capability list on hardware that is wired correctly.
 *
 * This file is also the contract the manifests are checked against. Every entry
 * here must have a matching manifest in packages/components with the same
 * `driver_type_id` and channel count, and tools/check-manifest-coherence.mjs
 * fails the build if they drift. That check reads this list by *running* the
 * compiled firmware rather than by parsing this source, so it verifies what the
 * image actually declares rather than what the file appears to say.
 *
 * Adding a sensor is: a driver in firmware/common, a line here, and a manifest.
 * Nothing in the scheduler, nothing in the UI, nothing in the protocol.
 */
#include "lorahome/bme680.h"
#include "lorahome/driver.h"
#include "lorahome/gpio_digital.h"

const lh_driver_vtable_t *const LH_DRIVERS[] = {
    &LH_BME680_DRIVER,        /* type 16, i2c 0x76/0x77 */
    &LH_GPIO_DIGITAL_DRIVER,  /* type 17, any pin       */
};

const uint8_t LH_DRIVER_COUNT = (uint8_t)(sizeof(LH_DRIVERS) / sizeof(LH_DRIVERS[0]));
