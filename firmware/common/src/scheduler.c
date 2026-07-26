/*
 * Cooperative sensor scheduler. Roadmap T3.7. Contract in scheduler.h.
 *
 * The loop is deliberately boring: walk the components from a rotating start,
 * give each one at most one non-blocking step, return. Everything interesting
 * is in what it refuses to do — wait, retry in place, or let one component's
 * turn depend on another's.
 */
#include "lorahome/scheduler.h"

#include <string.h>

/*
 * Static RAM for the whole scheduler, checked on every ABI.
 *
 * Eight contexts, eight due-times, and a reading cache four channels deep. The
 * cache is what dominates: 8 x 4 x sizeof(lh_reading_t). Asserting the total
 * here is what turns "adding a field" into a conversation before the merge
 * rather than a surprise in a size report afterwards.
 */
_Static_assert(sizeof(lh_scheduler_t) <= 1536, "scheduler static RAM budget");
_Static_assert(LH_SCHED_STEPS_PER_TICK == 1u, "one step per component per tick");
_Static_assert(LH_SCHED_MAX_CHANNELS >= 4u, "the BME680 has four channels");

void lh_sched_init(lh_scheduler_t *sched) {
  if (sched == NULL) return;
  memset(sched, 0, sizeof *sched);
}

uint8_t lh_sched_component_count(const lh_scheduler_t *sched) {
  return sched == NULL ? 0u : sched->component_count;
}

int lh_sched_add(
    lh_scheduler_t *sched,
    const lh_driver_vtable_t *vt,
    uint8_t bus_addr,
    uint8_t component_id,
    int64_t now_us) {
  if (sched == NULL || vt == NULL) return LH_DRV_ERR_STATE;
  if (sched->component_count >= LH_MAX_COMPONENTS) return LH_DRV_ERR_STATE;
  if (vt->channel_count > LH_SCHED_MAX_CHANNELS) return LH_DRV_ERR_NO_CHANNEL;

  const uint8_t slot = sched->component_count;
  const lh_drv_err_t err = lh_driver_bind(&sched->components[slot], vt, bus_addr, component_id, now_us);

  /*
   * The slot is taken either way, including when init failed.
   *
   * A component that was configured and does not answer has to be visible: the
   * host asked for it, and "silently absent" is indistinguishable from "never
   * configured" in every report the node produces. lh_driver_bind has already
   * put it in FAULTED, so the tick loop will skip it and the counter will say
   * why.
   */
  sched->next_due_us[slot] = now_us;
  sched->channel_cursor[slot] = 0;
  sched->latest_valid[slot] = 0;
  sched->component_count++;

  if (err != LH_DRV_OK) {
    sched->stat_faulted++;
    return err;
  }
  return (int)slot;
}

/** One non-blocking step for one component. Returns true if anything was done. */
static bool step_component(lh_scheduler_t *sched, uint8_t slot, int64_t now_us) {
  lh_driver_ctx_t *ctx = &sched->components[slot];
  if (ctx->vt == NULL) return false;
  if (lh_driver_is_faulted(ctx)) return false;
  if (now_us < sched->next_due_us[slot]) return false;

  switch ((lh_drv_state_t)ctx->state) {
    case LH_DRV_STATE_WARMUP:
    case LH_DRV_STATE_IDLE:
    case LH_DRV_STATE_SLEEPING: {
      const lh_drv_err_t err = lh_driver_start_read(ctx, now_us);
      if (err == LH_DRV_ERR_NOT_READY) {
        /* Still warming. Not an error, and not a reason to stop asking — but
         * the component is put back in the queue at the tick rate rather than
         * at its interval, so a 200 ms warmup does not cost a 3 s cycle. */
        return true;
      }
      if (lh_driver_is_error(err)) {
        sched->stat_errors++;
        if (lh_driver_is_faulted(ctx)) sched->stat_faulted++;
        /* Wait a full interval before the next attempt. Retrying a failing
         * component at tick rate is how a shorted bus consumes every tick and
         * starves the seven components that are working. */
        sched->next_due_us[slot] = now_us + (int64_t)ctx->vt->min_interval_ms * 1000;
        return true;
      }
      sched->channel_cursor[slot] = 0;
      return true;
    }

    case LH_DRV_STATE_READING:
    case LH_DRV_STATE_READY: {
      const uint8_t channel = sched->channel_cursor[slot];
      lh_reading_t reading;
      memset(&reading, 0, sizeof reading);

      const lh_drv_err_t err = lh_driver_poll(ctx, &reading, channel, now_us);

      if (err == LH_DRV_ERR_NOT_READY) return true; /* ask again next tick */

      if (lh_driver_is_error(err)) {
        sched->stat_errors++;
        if (lh_driver_is_faulted(ctx)) sched->stat_faulted++;
        sched->next_due_us[slot] = now_us + (int64_t)ctx->vt->min_interval_ms * 1000;
        sched->channel_cursor[slot] = 0;
        return true;
      }

      if (channel < LH_SCHED_MAX_CHANNELS) {
        sched->latest[slot][channel] = reading;
        sched->latest_valid[slot] |= (uint8_t)(1u << channel);
      }
      sched->stat_readings++;

      /* One channel per tick, for the same reason as one step per component:
       * collecting four in a row would make the worst tick four sensor
       * operations long instead of one. */
      const uint8_t next = (uint8_t)(channel + 1);
      if (next < ctx->vt->channel_count) {
        sched->channel_cursor[slot] = next;
      } else {
        /* The measurement is complete. The interval is counted from now, so a
         * sensor that took longer than usual does not immediately start again. */
        sched->channel_cursor[slot] = 0;
        sched->next_due_us[slot] = now_us + (int64_t)ctx->vt->min_interval_ms * 1000;
      }
      return true;
    }

    case LH_DRV_STATE_EMPTY:
    case LH_DRV_STATE_FAULTED:
    default:
      return false;
  }
}

uint8_t lh_sched_tick(lh_scheduler_t *sched, int64_t now_us) {
  if (sched == NULL || sched->component_count == 0) return 0;

  sched->stat_ticks++;

  uint8_t stepped = 0;
  const uint8_t count = sched->component_count;

  /*
   * Round-robin from a rotating start.
   *
   * A fixed sweep from zero services component 0 first on every single tick,
   * and component 7 only when nothing earlier had work. Under load that is a
   * sensor which quietly stops reporting while its neighbours are fine — a
   * fault that looks like bad hardware and is actually a for-loop.
   */
  for (uint8_t i = 0; i < count; i++) {
    const uint8_t slot = (uint8_t)((sched->rotation + i) % count);
    if (step_component(sched, slot, now_us)) stepped++;
  }
  sched->rotation = (uint8_t)((sched->rotation + 1u) % count);

  return stepped;
}

void lh_sched_record_tick(lh_scheduler_t *sched, uint32_t elapsed_us) {
  if (sched == NULL) return;
  if (elapsed_us > sched->stat_tick_max_us) sched->stat_tick_max_us = elapsed_us;
  if (elapsed_us > LH_SCHED_TICK_BUDGET_US) sched->stat_budget_overruns++;
}

bool lh_sched_latest(
    const lh_scheduler_t *sched, uint8_t slot, uint8_t channel, lh_reading_t *out) {
  if (sched == NULL || out == NULL) return false;
  if (slot >= sched->component_count || channel >= LH_SCHED_MAX_CHANNELS) return false;
  if ((sched->latest_valid[slot] & (uint8_t)(1u << channel)) == 0) return false;

  *out = sched->latest[slot][channel];
  return true;
}
