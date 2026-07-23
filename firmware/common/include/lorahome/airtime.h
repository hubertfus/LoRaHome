#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * LoRa time-on-air estimate (Semtech AN1200.22), used by the Bridge's duty
 * cycle tracker. Mirrors packages/host/src/duty-cycle-guard/airtime.ts —
 * the Host and the Bridge each independently enforce the ETSI 1% limit
 * (ARCHITECTURE.md §7), so both must agree on what a frame actually costs.
 */
typedef struct {
  uint16_t bytes;
  uint8_t spreading_factor; /* 7-12 */
  uint32_t bandwidth_hz;    /* e.g. 125000 */
  uint8_t coding_rate;      /* 1 = 4/5 ... 4 = 4/8 */
  uint8_t preamble_symbols; /* typically 8 */
} lorahome_airtime_params_t;

/** Returns estimated time-on-air in milliseconds. */
float lorahome_compute_airtime_ms(const lorahome_airtime_params_t* params);

#ifdef __cplusplus
}
#endif
