# firmware/node

ESP32 + SX1262, end-device. Zero dynamic allocation — see [CONTRIBUTING.md](../../CONTRIBUTING.md) §1.

- `src/drivers/` — static registry of sensor/actuator drivers (`DriverVTable`), max 8 components. `bme680_driver.*` and `gpio_digital_driver.*` match the manifests in `packages/components/manifests`.
- `src/config_store.*` — NVS A/B slot config lifecycle (ARCHITECTURE.md §4).
- `src/main.cpp` — scheduler (respects each driver's `warmup_ms`) + local rule evaluator wiring.
- `test/` — Unity unit tests for `firmware/common`'s pure logic (protocol, CRC16, rule evaluator), run natively with `pio test -e native` — no hardware or ESP32 toolchain needed.

Two PlatformIO environments: `esp32dev` (real hardware, Arduino framework) and `native` (host-native unit tests).
