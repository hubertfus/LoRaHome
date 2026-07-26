#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * I2C bus scan with per-address isolation. Roadmap T3.2.
 *
 * The scan itself is trivial: address every device from 0x08 to 0x77 and see
 * who answers. Everything difficult here is about the case where the bus is
 * broken, because that is the case that decides whether a node in a wall stays
 * reachable.
 *
 * Two failures have to be told apart. One device holding the bus is a
 * component fault: the scan records it, skips that address and carries on, and
 * the other seven components keep working. SDA shorted to ground is a bus
 * fault: *every* address times out, and a scanner that dutifully waits out all
 * 112 of them has turned a broken sensor into a node that stops answering the
 * radio for half a second on every scan. The second case is detected by its
 * signature — timeouts with nothing in between — and abandoned early with the
 * bus reported as wedged. That is risk R3.2, and the roadmap's test for it is
 * the one everybody forgets: short SDA to GND and check the system is still
 * alive afterwards.
 *
 * There is no platform in this file. The caller supplies a probe function and a
 * clock, which is what lets the wedged-bus path be tested in microseconds
 * instead of by shorting a real wire, and what lets the same object file link
 * into the node image, the native harness and the three-ABI sweep.
 */

/** The 7-bit addressable range, excluding the reserved blocks at both ends. */
#define LH_I2C_ADDR_FIRST 0x08u
#define LH_I2C_ADDR_LAST 0x77u
#define LH_I2C_ADDR_COUNT (LH_I2C_ADDR_LAST - LH_I2C_ADDR_FIRST + 1u) /* 112 */

/** Devices recorded per scan. A node takes 8 components; 16 is slack, not ambition. */
#define LH_I2C_SCAN_MAX_FOUND 16u

/**
 * Per-address timeout, derived from the full-scan budget rather than guessed.
 *
 * The roadmap budgets a full scan at 500 ms and one address at 5 ms. Those are
 * not independent: a scan that waits the per-address timeout at all 112
 * addresses takes 112 x T, so T is pinned by the scan budget at no more than
 * 4.46 ms. 4 ms leaves the whole sweep at 448 ms with both budgets satisfied,
 * and the _Static_assert in i2c_scan.c is what keeps the two numbers tied
 * together when somebody later raises one of them alone.
 */
#define LH_I2C_PROBE_TIMEOUT_MS 4u

/** The roadmap's full-scan budget, asserted against the per-address timeout. */
#define LH_I2C_SCAN_BUDGET_MS 500u

/**
 * Consecutive timeouts that mean the bus is wedged rather than one device.
 *
 * Eight rather than one: a single device that NAKs slowly is not a reason to
 * abandon a scan, and aborting on the first timeout would make one flaky sensor
 * hide the other seven. Eight rather than fifty: the point is to bound what a
 * shorted line costs, and 8 x 4 ms is 32 ms against the 448 ms it would take to
 * discover the same thing the patient way.
 */
#define LH_I2C_WEDGED_BUS_TIMEOUTS 8u

typedef enum {
  LH_I2C_ACK = 0,       /* a device acknowledged its address        */
  LH_I2C_NACK = 1,      /* nothing there — the normal answer        */
  LH_I2C_TIMEOUT = 2,   /* the bus did not release in time          */
  LH_I2C_BUS_ERROR = 3, /* arbitration lost, or the driver refused  */
} lh_i2c_probe_r;

/**
 * Addresses one device and reports whether it answered.
 *
 * Must return within LH_I2C_PROBE_TIMEOUT_MS. The scanner measures whether it
 * did and counts the overruns rather than trusting the claim: an ESP32 `Wire`
 * built without a timeout configured blocks for as long as the peripheral feels
 * like, and the resulting scan is over budget in a way no code inspection
 * shows.
 */
typedef lh_i2c_probe_r (*lh_i2c_probe_fn)(uint8_t addr, void *user);

/** Monotonic microseconds. Injected for the same reason as everywhere else here. */
typedef int64_t (*lh_i2c_clock_fn)(void *user);

typedef struct {
  lh_i2c_probe_fn probe;
  lh_i2c_clock_fn now_us;
  void *user; /* passed through to both; the scanner never inspects it */
} lh_i2c_bus_t;

/**
 * What a scan found, and what it cost.
 *
 * The counters are as much of the result as the addresses. A scan that returns
 * two devices having timed out forty times has found a bus that is about to
 * fail, and a caller that only reads `found` will report it as healthy.
 */
typedef struct {
  uint8_t found[LH_I2C_SCAN_MAX_FOUND];
  uint8_t found_count;
  uint8_t overflow;    /* devices past MAX_FOUND — addresses lost, not hidden  */
  uint8_t probed;      /* addresses actually attempted (< 112 if aborted)      */
  uint8_t timeouts;    /* saturating; the pattern matters more than the total  */
  uint8_t bus_errors;  /* saturating                                           */
  uint8_t slow_probes; /* probes that overran LH_I2C_PROBE_TIMEOUT_MS          */
  bool wedged;         /* aborted: consecutive timeouts, the bus is not usable */
  bool over_budget;    /* aborted: the whole scan overran LH_I2C_SCAN_BUDGET_MS */
  int64_t started_us;
  int64_t finished_us;
} lh_i2c_scan_result_t;

/**
 * Probes one address, measuring how long the platform actually took.
 *
 * Exposed separately because a driver's own `probe` hook wants exactly this,
 * and because a caller re-checking one address after a fault should not have to
 * run a whole sweep to do it.
 */
lh_i2c_probe_r lh_i2c_probe(const lh_i2c_bus_t *bus, uint8_t addr, int64_t *elapsed_us);

/**
 * Sweeps 0x08..0x77.
 *
 * Always fills `*out`, including when it gives up early: a scan that aborted is
 * a result with `wedged` or `over_budget` set and a `probed` count below 112,
 * not an error code that loses everything learned before the fault. The caller
 * gets the devices found so far either way.
 *
 * Returns true if the sweep completed, false if it was abandoned.
 */
bool lh_i2c_scan(const lh_i2c_bus_t *bus, lh_i2c_scan_result_t *out);

/** True if this address answered in the given result. */
bool lh_i2c_scan_contains(const lh_i2c_scan_result_t *result, uint8_t addr);

/** Total scan duration in milliseconds, for the caller's metric line. */
uint32_t lh_i2c_scan_duration_ms(const lh_i2c_scan_result_t *result);

#ifdef __cplusplus
}
#endif
