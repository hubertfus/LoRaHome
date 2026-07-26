/*
 * I2C bus scan with per-address isolation. Roadmap T3.2. Contract in i2c_scan.h.
 *
 * The loop is ten lines. The rest is the two ways a scan can go wrong and the
 * difference between them, which is the whole reason this is a file rather than
 * a for-loop at the call site.
 */
#include "lorahome/i2c_scan.h"

#include <string.h>

/*
 * The per-address timeout and the full-scan budget are one decision, not two.
 *
 * A sweep that waits out every address costs 112 x the timeout, so choosing the
 * timeout freely and the budget freely is choosing the same number twice. This
 * assertion is what makes raising one of them alone a build failure instead of
 * a scan that quietly runs over budget on a bus with nothing on it.
 */
_Static_assert(
    LH_I2C_ADDR_COUNT * LH_I2C_PROBE_TIMEOUT_MS <= LH_I2C_SCAN_BUDGET_MS,
    "a full sweep of timeouts must fit the scan budget");
_Static_assert(LH_I2C_ADDR_COUNT == 112u, "the addressable range is 0x08..0x77");
_Static_assert(sizeof(lh_i2c_scan_result_t) <= 64u, "scan result budget");
_Static_assert(
    LH_I2C_WEDGED_BUS_TIMEOUTS < LH_I2C_ADDR_COUNT,
    "a wedged bus must be detectable before the sweep ends on its own");

/** Saturating increment: a counter that wraps to zero reports a healthy bus. */
static void bump(uint8_t *counter) {
  if (*counter < 0xFFu) (*counter)++;
}

lh_i2c_probe_r lh_i2c_probe(const lh_i2c_bus_t *bus, uint8_t addr, int64_t *elapsed_us) {
  if (bus == NULL || bus->probe == NULL) return LH_I2C_BUS_ERROR;

  const int64_t started = (bus->now_us != NULL) ? bus->now_us(bus->user) : 0;
  const lh_i2c_probe_r result = bus->probe(addr, bus->user);
  const int64_t finished = (bus->now_us != NULL) ? bus->now_us(bus->user) : 0;

  if (elapsed_us != NULL) *elapsed_us = finished - started;
  return result;
}

bool lh_i2c_scan(const lh_i2c_bus_t *bus, lh_i2c_scan_result_t *out) {
  if (out == NULL) return false;
  memset(out, 0, sizeof *out);
  if (bus == NULL || bus->probe == NULL) {
    out->bus_errors = 1;
    return false;
  }

  out->started_us = (bus->now_us != NULL) ? bus->now_us(bus->user) : 0;
  out->finished_us = out->started_us;

  uint8_t consecutive_timeouts = 0;

  for (uint16_t addr = LH_I2C_ADDR_FIRST; addr <= LH_I2C_ADDR_LAST; addr++) {
    int64_t elapsed_us = 0;
    const lh_i2c_probe_r result = lh_i2c_probe(bus, (uint8_t)addr, &elapsed_us);

    out->probed++;
    out->finished_us = (bus->now_us != NULL) ? bus->now_us(bus->user) : out->finished_us;

    if (elapsed_us > (int64_t)LH_I2C_PROBE_TIMEOUT_MS * 1000) bump(&out->slow_probes);

    switch (result) {
      case LH_I2C_ACK:
        consecutive_timeouts = 0;
        if (out->found_count < LH_I2C_SCAN_MAX_FOUND) {
          out->found[out->found_count++] = (uint8_t)addr;
        } else {
          /* Counted rather than dropped silently. Seventeen devices on a node
           * that supports eight components is a wiring mistake, and a scan that
           * simply stopped listing them would report it as a working bus. */
          bump(&out->overflow);
        }
        break;

      case LH_I2C_NACK:
        consecutive_timeouts = 0;
        break;

      case LH_I2C_TIMEOUT:
        bump(&out->timeouts);
        consecutive_timeouts++;
        break;

      case LH_I2C_BUS_ERROR:
      default:
        bump(&out->bus_errors);
        /* Not a timeout, so it does not count towards "wedged" — an
         * arbitration loss means somebody else is driving the bus, which is a
         * different fault from a line stuck low, and merging them would send
         * whoever reads the counters to check the wrong wire. */
        consecutive_timeouts = 0;
        break;
    }

    if (consecutive_timeouts >= LH_I2C_WEDGED_BUS_TIMEOUTS) {
      /* The signature of a line held low: nothing answers, and nothing NAKs
       * either. Continuing would spend the remaining addresses discovering the
       * same thing at full price, during which this node is not servicing its
       * radio. The scan ends; the node does not. */
      out->wedged = true;
      return false;
    }

    if ((out->finished_us - out->started_us) > (int64_t)LH_I2C_SCAN_BUDGET_MS * 1000) {
      /* A backstop against a platform whose probe does not honour its own
       * timeout. Without it, the budget in the header is a comment: every
       * guarantee this file makes about duration rests on a function it does
       * not own. */
      out->over_budget = true;
      return false;
    }
  }

  return true;
}

bool lh_i2c_scan_contains(const lh_i2c_scan_result_t *result, uint8_t addr) {
  if (result == NULL) return false;
  for (uint8_t i = 0; i < result->found_count; i++) {
    if (result->found[i] == addr) return true;
  }
  return false;
}

uint32_t lh_i2c_scan_duration_ms(const lh_i2c_scan_result_t *result) {
  if (result == NULL) return 0;
  const int64_t delta_us = result->finished_us - result->started_us;
  return delta_us <= 0 ? 0u : (uint32_t)(delta_us / 1000);
}
