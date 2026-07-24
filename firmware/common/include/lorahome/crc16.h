#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initial CRC register value for CCITT-FALSE. */
#define LORAHOME_CRC16_INIT 0xFFFFu

/**
 * Catalogue check value: CRC of the ASCII string "123456789".
 *
 * NB for anyone comparing against the roadmap, which quotes 0xE5CC: that is the
 * check value for CRC-16/AUG-CCITT, which differs only in using init=0x1D0F.
 * With the init=0xFFFF that the roadmap itself specifies (and that every
 * committed golden vector uses) the correct value is 0x29B1. The parameters
 * win over the quoted constant; see the T0.5 commit message.
 */
#define LORAHOME_CRC16_CHECK 0x29B1u

/**
 * CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xorout).
 * Must produce byte-identical results to packages/protocol/src/crc16.ts —
 * see packages/protocol/test/fixtures/crc16-vectors.json for the shared
 * cross-language vector set.
 */
uint16_t lorahome_crc16(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif
