#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xorout).
 * Must produce byte-identical results to packages/protocol/src/frame.ts's
 * crc16() — see test/fixtures/beacon.json in packages/protocol for a shared
 * cross-language test vector.
 */
uint16_t lorahome_crc16(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif
