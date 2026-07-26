#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lorahome/driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Cooperative sensor scheduler. Roadmap T3.7.
 *
 * The component that decides whether this node is a radio that also reads
 * sensors, or a sensor reader that occasionally misses frames.
 *
 * One number governs the whole design: a tick must never take longer than 5 ms.
 * Not on average — ever. The radio's receive window is not something the
 * scheduler can see, and a tick that overruns closes it unattended; the frames
 * lost that way go missing *only while a sensor happens to be busy*, which
 * presents as an intermittent radio fault and sends whoever is debugging it to
 * read the wrong code for a day. That is risk R3.1, and the budget is the
 * mechanism that keeps it from happening.
 *
 * So the scheduler does at most one thing per component per tick, and the
 * drivers underneath it have no blocking calls to offer. It walks its
 * components, gives each one a single non-blocking step, and returns. There is
 * no loop inside it that waits for anything.
 *
 * Fairness is round-robin from a rotating start rather than a fixed sweep from
 * zero. With a fixed start, component 0 is serviced first on every tick and
 * component 7 only gets attention on ticks where nothing earlier had work —
 * which under load is a sensor that quietly stops reporting while its
 * neighbours are fine.
 */

/**
 * The hard budget for one tick, in microseconds.
 *
 * The roadmap's number, and the one the Etap 3 DoD gates a release on. It is a
 * ceiling on the *maximum*, not the average: an average holds while a p99
 * ruins a receive window every few seconds, and the frames that go missing are
 * exactly the ones a scheduler-shaped fault would drop.
 */
#define LH_SCHED_TICK_BUDGET_US 5000u

/**
 * Steps taken per component per tick.
 *
 * One. A component that could be started and polled in the same tick would
 * still only be doing what a blocking read does, spread over two calls — and
 * with eight of them the tick becomes eight sensor operations long. One step
 * each keeps the worst tick predictable from the slowest single operation
 * rather than from their sum.
 */
#define LH_SCHED_STEPS_PER_TICK 1u

/**
 * Channels held per component, matching the widest driver in the image.
 *
 * Four, because the BME680 has four. A fixed array rather than a per-driver
 * allocation: eight components each holding a small block is how a node ends
 * up with free memory and no contiguous room for a radio buffer (R3.5).
 */
#define LH_SCHED_MAX_CHANNELS 4u

typedef struct {
  /** Bound components. Slots with a NULL vtable are skipped. */
  lh_driver_ctx_t components[LH_MAX_COMPONENTS];

  /**
   * When each component is next due, in microseconds.
   *
   * Absolute rather than a countdown, so a tick that arrives late does not
   * shift every future deadline by however late it was. A node whose loop
   * stalls for a second should catch up, not permanently drift.
   */
  int64_t next_due_us[LH_MAX_COMPONENTS];

  /** The most recent reading collected from each component, per channel. */
  lh_reading_t latest[LH_MAX_COMPONENTS][LH_SCHED_MAX_CHANNELS];
  uint8_t latest_valid[LH_MAX_COMPONENTS];

  /** Which channel of the current measurement is being collected. */
  uint8_t channel_cursor[LH_MAX_COMPONENTS];

  uint8_t component_count;
  /** Round-robin cursor, so component 0 is not always serviced first. */
  uint8_t rotation;

  uint32_t stat_ticks;
  uint32_t stat_readings;
  uint32_t stat_errors;
  uint32_t stat_faulted;
  /** Ticks that ran over LH_SCHED_TICK_BUDGET_US. Must stay 0. */
  uint32_t stat_budget_overruns;
  /** Longest tick observed, in microseconds. The metric the DoD gates on. */
  uint32_t stat_tick_max_us;
} lh_scheduler_t;

void lh_sched_init(lh_scheduler_t *sched);

/**
 * Binds a component into the next free slot.
 *
 * Returns the slot index, or a negative lh_drv_err_t. A component whose driver
 * fails to initialise is still given a slot, in the FAULTED state: the node
 * needs to be able to report that a configured sensor is not answering, and a
 * component that was silently dropped is indistinguishable from one that was
 * never configured.
 */
int lh_sched_add(
    lh_scheduler_t *sched,
    const lh_driver_vtable_t *vt,
    uint8_t bus_addr,
    uint8_t component_id,
    int64_t now_us);

/**
 * Runs one tick. Never blocks, never allocates, never exceeds the budget.
 *
 * `elapsed_us` is what the caller measured for this tick, fed back in so the
 * scheduler can keep its own maximum. Passing it rather than reading a clock
 * twice inside keeps this file free of a platform, and means the number
 * recorded is the one the caller's own instrumentation saw.
 *
 * Returns the number of components stepped.
 */
uint8_t lh_sched_tick(lh_scheduler_t *sched, int64_t now_us);

/** Records how long the caller measured the last tick to take. */
void lh_sched_record_tick(lh_scheduler_t *sched, uint32_t elapsed_us);

/**
 * The most recent reading for a channel, if there is one.
 *
 * Returns false when the component has produced nothing yet — which is not an
 * error, just a node that has been up for less than one interval.
 */
bool lh_sched_latest(
    const lh_scheduler_t *sched, uint8_t slot, uint8_t channel, lh_reading_t *out);

/** Fills a capability report from the scheduler's components. */
uint8_t lh_sched_component_count(const lh_scheduler_t *sched);

#ifdef __cplusplus
}
#endif
