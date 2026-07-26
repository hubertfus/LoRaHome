#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register-level I2C access, as a driver sees it. Roadmap T3.3.
 *
 * Separate from the scan interface in i2c_scan.h because the two want different
 * things: a scan asks "did anyone answer at this address", a driver asks "give
 * me eight bytes starting at register 0x8A". Merging them would give every
 * scanner a read/write pair it never calls and every driver a probe hook it
 * does not want.
 *
 * The functions are supplied by the image, so firmware/common stays free of
 * `Wire`, of ESP-IDF and of any particular bus implementation. That is what
 * lets a sensor driver be tested against a simulated chip: the compensation
 * arithmetic, the state machine and the failure paths are all exercised without
 * a device, and only the eight lines that talk to the peripheral are not.
 *
 * Every call must return within the platform's own transaction timeout. A read
 * that blocks for ever defeats the non-blocking model above it — the driver
 * cannot be quicker than the bus underneath it.
 */
typedef struct {
  /** Reads `len` bytes starting at `reg`. False on NACK, timeout or bus error. */
  bool (*read)(uint8_t addr, uint8_t reg, uint8_t *out, uint8_t len, void *user);

  /** Writes one register. False on NACK, timeout or bus error. */
  bool (*write)(uint8_t addr, uint8_t reg, uint8_t value, void *user);

  /** Monotonic microseconds, injected for the same reason as everywhere else. */
  int64_t (*now_us)(void *user);

  void *user; /* passed through untouched */
} lh_i2c_io_t;

#ifdef __cplusplus
}
#endif
