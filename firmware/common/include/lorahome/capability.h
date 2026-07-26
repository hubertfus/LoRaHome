#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lorahome/driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Capability discovery: what a node has on it. Roadmap T3.5, ARCHITECTURE.md §8.
 *
 * The Host asks with a 0x40 frame; the node answers with 0x41 carrying this
 * report. It inverts the usual flow — instead of declaring a sensor in a
 * configuration file and hoping it is wired correctly, you plug the hardware in
 * and the system tells you what it found.
 *
 * The report must fit one frame. Fragmenting a discovery response would mean
 * discovery depends on reassembly, which depends on an ARQ, which is a lot of
 * machinery to make "what are you?" work — and all of it is what you would be
 * debugging when a new node fails to appear. 100 bytes of CBOR for eight
 * components is the budget, against a 220 B payload.
 *
 * `free_heap_kb` travels in every report on purpose. It is the cheapest
 * telemetry in the system: the Host learns the memory health of every node it
 * discovers without a separate channel, a separate frame type or a separate
 * schedule. On a device where the failure that matters is a slow leak, a number
 * that arrives for free every time anybody asks anything is worth having.
 */

/** Components in a report. The node's own limit, so a report is always complete. */
#define LH_CAP_MAX LH_MAX_COMPONENTS

/**
 * Wire budget for a realistic full report, in bytes.
 *
 * The roadmap's figure, and it applies to eight components with the field
 * values the protocol actually produces: bus types from a five-value enum,
 * channel counts in single digits, a flags byte with one bit defined. Checked
 * rather than assumed, because the encoded size grows quietly as type ids are
 * allocated — an encoding that passes at 93 B today and 101 B after four more
 * drivers would fragment a discovery response in the field and nowhere else.
 */
#define LH_CAP_WIRE_BUDGET 100u

/**
 * The largest report this codec can produce, in bytes.
 *
 * Every field at the widest value its type allows: 12 B per component (1 array
 * header, 3 for a 16-bit type id, and 2 each for four bytes that spill past the
 * 23-value inline form) plus 13 B of envelope. 8 x 12 + 13 = 109.
 *
 * This number was originally worked out as 93 and asserted as such, on the
 * assumption that bus type, channel count and flags would stay inline. They do
 * in every report the system generates, and they do not in every report it can
 * be *sent* — the decoder accepts a full byte in each of those fields, and the
 * bound that matters for a receive buffer is the one an adversary can reach,
 * not the one the encoder happens to emit. The test now asserts 109 because
 * that is what the arithmetic says.
 *
 * 109 B still fits a single frame with more than half the payload to spare,
 * which is the requirement the roadmap's budget exists to express. The 100 B
 * figure is the tighter target for reports the system produces, and it holds.
 */
#define LH_CAP_WIRE_WORST_CASE 109u

typedef enum {
  LH_BUS_I2C = 0,
  LH_BUS_GPIO = 1,
  LH_BUS_SPI = 2,
  LH_BUS_ONEWIRE = 3,
  LH_BUS_ADC = 4,
} lh_bus_type_t;

/** bit 0: this component already has a configuration on the node. */
#define LH_CAP_FLAG_CONFIGURED 0x01u

typedef struct {
  uint16_t driver_type_id;
  uint8_t bus_addr; /* I2C address, or pin number for a GPIO */
  uint8_t bus_type; /* lh_bus_type_t */
  uint8_t channel_count;
  uint8_t flags;
} lh_capability_t;

typedef struct {
  uint8_t count;
  lh_capability_t caps[LH_CAP_MAX];
  uint32_t fw_version;
  uint16_t free_heap_kb;
} lh_cap_report_t;

/** Map keys. Integers, never strings — CONTRIBUTING.md §3. */
#define LH_CAP_KEY_FW_VERSION 1u
#define LH_CAP_KEY_FREE_HEAP_KB 2u
#define LH_CAP_KEY_COMPONENTS 3u

/** Positions inside one component's array. */
#define LH_CAP_FIELD_TYPE_ID 0u
#define LH_CAP_FIELD_BUS_ADDR 1u
#define LH_CAP_FIELD_BUS_TYPE 2u
#define LH_CAP_FIELD_CHANNELS 3u
#define LH_CAP_FIELD_FLAGS 4u
#define LH_CAP_FIELDS 5u

/**
 * Encodes a report. Returns bytes written, or a negative value if it did not fit.
 *
 * Each component is a positional array rather than a map. A map per component
 * would cost five key bytes each — forty bytes over eight components, against a
 * hundred-byte budget — to describe a structure that is fixed by the protocol
 * and could not be reordered anyway. The outer report is a map because it *will*
 * gain keys, and the components array is exactly where it must not.
 */
int lh_cap_encode(const lh_cap_report_t *report, uint8_t *out, uint16_t cap);

/**
 * Decodes a report.
 *
 * Unknown map keys are skipped rather than refused, so a node running older
 * firmware can still be discovered by a newer host. A component array with more
 * fields than this build knows is truncated the same way, for the same reason.
 * Returns false only for a message that is malformed or does not fit the
 * receiver's limits.
 */
bool lh_cap_decode(const uint8_t *buf, uint16_t len, lh_cap_report_t *out);

/** True if the two reports carry identical contents. For tests and diagnostics. */
bool lh_cap_equal(const lh_cap_report_t *a, const lh_cap_report_t *b);

#ifdef __cplusplus
}
#endif
