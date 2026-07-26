/*
 * Driver registry and dispatch. Roadmap T3.1. Contract in lorahome/driver.h.
 *
 * Everything here is bookkeeping around someone else's function pointers: the
 * state machine, the warmup clock, the operation deadline and the fault count.
 * None of it touches a bus. That separation is what makes the whole layer
 * testable without a sensor on the desk — the mock driver in the self-test
 * exercises the same code path a BME680 will.
 *
 * `now_us` is a parameter rather than a call into a clock, for the same reason
 * as in dedup.c: firmware/common has no platform. The same object file links
 * into the ESP32 image, the native harness and the three-ABI compile sweep, and
 * a timeout test that has to wait a real second is a timeout test that gets
 * commented out.
 */
#include "lorahome/driver.h"

#include <string.h>

/*
 * The RAM budget for the whole component table, checked on every ABI the
 * compile sweep can reach. 8 * 64 B is the roadmap's `mem.registry.static`
 * limit, and a struct's size is exactly the kind of thing that is fine on the
 * host and different on xtensa: `op_started_us` is an int64_t sitting next to
 * bytes, so its alignment decides the answer.
 *
 * Asserting the product rather than the element keeps the budget stated in the
 * terms the roadmap states it in — somebody adding a field sees the number they
 * are spending, not a per-instance figure they have to multiply.
 */
_Static_assert(sizeof(lh_driver_ctx_t) <= 64, "driver ctx budget: 64 B per instance");
_Static_assert(
    LH_MAX_COMPONENTS * sizeof(lh_driver_ctx_t) <= 512,
    "mem.registry.static budget: 512 B for the whole component table");
_Static_assert(LH_DRIVER_SCRATCH_SIZE == 32u, "scratch size is part of the ctx budget");

/* A driver with no channels could never produce a reading; catch it at build. */
#define CHANNELS_OK(vt) ((vt)->channel_count > 0u)

uint32_t lh_driver_op_timeout_ms(const lh_driver_vtable_t *vt) {
  if (vt == NULL) return LH_DRIVER_OP_TIMEOUT_FLOOR_MS;
  const uint32_t scaled = (uint32_t)vt->warmup_ms * LH_DRIVER_OP_TIMEOUT_FACTOR;
  return scaled > LH_DRIVER_OP_TIMEOUT_FLOOR_MS ? scaled : LH_DRIVER_OP_TIMEOUT_FLOOR_MS;
}

bool lh_driver_is_error(lh_drv_err_t err) {
  return err != LH_DRV_OK && err != LH_DRV_ERR_NOT_READY;
}

const lh_driver_vtable_t *lh_driver_find(const char *name) {
  if (name == NULL) return NULL;
  for (uint8_t i = 0; i < LH_DRIVER_COUNT; i++) {
    const lh_driver_vtable_t *vt = LH_DRIVERS[i];
    if (vt != NULL && vt->name != NULL && strcmp(vt->name, name) == 0) return vt;
  }
  return NULL;
}

const lh_driver_vtable_t *lh_driver_find_type(uint16_t type_id) {
  for (uint8_t i = 0; i < LH_DRIVER_COUNT; i++) {
    const lh_driver_vtable_t *vt = LH_DRIVERS[i];
    if (vt != NULL && vt->type_id == type_id) return vt;
  }
  return NULL;
}

bool lh_driver_is_faulted(const lh_driver_ctx_t *ctx) {
  return ctx != NULL && ctx->state == (uint8_t)LH_DRV_STATE_FAULTED;
}

/**
 * Counts a failed driver call and retires the component once it is time.
 *
 * Clearing the count is deliberately *not* the inverse of this function, and
 * happens in exactly one place: when a poll returns an actual reading. The
 * symmetric version was written first and was wrong. A successful start_read
 * means an I2C write was accepted, which is not evidence that the component
 * works — so clearing on it let a permanently stuck sensor reset its own error
 * count on every cycle, one call before timing out again, and it stayed in the
 * rotation for ever. A component is healthy when it produces data, not when it
 * accepts a request.
 *
 * Passing a non-error through unchanged keeps the call sites readable: they can
 * wrap every return without asking first whether it counts.
 */
static lh_drv_err_t fail(lh_driver_ctx_t *ctx, lh_drv_err_t err) {
  if (!lh_driver_is_error(err)) return err;

  if (ctx->consecutive_errors < 0xFFu) ctx->consecutive_errors++;
  if (ctx->consecutive_errors >= LH_DRIVER_FAULT_THRESHOLD) {
    ctx->state = (uint8_t)LH_DRV_STATE_FAULTED;
  }
  return err;
}

/** Milliseconds elapsed since the context's current operation began. */
static int64_t elapsed_ms(const lh_driver_ctx_t *ctx, int64_t now_us) {
  const int64_t delta_us = now_us - ctx->op_started_us;
  return delta_us < 0 ? 0 : delta_us / 1000;
}

lh_drv_err_t lh_driver_bind(
    lh_driver_ctx_t *ctx,
    const lh_driver_vtable_t *vt,
    uint8_t bus_addr,
    uint8_t component_id,
    int64_t now_us) {
  if (ctx == NULL || vt == NULL) return LH_DRV_ERR_STATE;
  if (!CHANNELS_OK(vt) || vt->poll == NULL) return LH_DRV_ERR_STATE;

  memset(ctx, 0, sizeof *ctx);
  ctx->vt = vt;
  ctx->bus_addr = bus_addr;
  ctx->component_id = component_id;
  ctx->op_started_us = now_us;

  if (vt->init != NULL) {
    const lh_drv_err_t err = vt->init(ctx, bus_addr);
    if (err != LH_DRV_OK) {
      /* Straight to FAULTED, not to a retry count. A device that did not answer
       * its initialisation is not going to answer the next two attempts either,
       * and the bus is shared with seven other components. */
      ctx->state = (uint8_t)LH_DRV_STATE_FAULTED;
      ctx->consecutive_errors = LH_DRIVER_FAULT_THRESHOLD;
      return err;
    }
  }

  ctx->state =
      (uint8_t)(vt->warmup_ms > 0u ? LH_DRV_STATE_WARMUP : LH_DRV_STATE_IDLE);
  return LH_DRV_OK;
}

lh_drv_err_t lh_driver_start_read(lh_driver_ctx_t *ctx, int64_t now_us) {
  if (ctx == NULL || ctx->vt == NULL) return LH_DRV_ERR_STATE;
  if (ctx->state == (uint8_t)LH_DRV_STATE_FAULTED) return LH_DRV_ERR_FAULTED;

  switch ((lh_drv_state_t)ctx->state) {
    case LH_DRV_STATE_WARMUP:
      if (elapsed_ms(ctx, now_us) < (int64_t)ctx->vt->warmup_ms) {
        /* Deliberately not counted as an error and deliberately not dispatched:
         * the driver never learns that anybody asked early. Warmup accounting
         * implemented once here cannot be got wrong per driver. */
        return LH_DRV_ERR_NOT_READY;
      }
      ctx->state = (uint8_t)LH_DRV_STATE_IDLE;
      break;

    case LH_DRV_STATE_SLEEPING:
      /* Waking is a warmup, by definition: the datasheet's figure is measured
       * from power-on, and a sensor that was asleep is in that state again. */
      ctx->op_started_us = now_us;
      if (ctx->vt->warmup_ms > 0u) {
        ctx->state = (uint8_t)LH_DRV_STATE_WARMUP;
        return LH_DRV_ERR_NOT_READY;
      }
      ctx->state = (uint8_t)LH_DRV_STATE_IDLE;
      break;

    case LH_DRV_STATE_IDLE:
    case LH_DRV_STATE_READY:
      break;

    case LH_DRV_STATE_READING:
      /* Already measuring. Restarting would discard a measurement that is
       * partly paid for and, on a forced-mode sensor, corrupt the one in the
       * chip's registers. */
      return LH_DRV_ERR_NOT_READY;

    case LH_DRV_STATE_EMPTY:
    case LH_DRV_STATE_FAULTED:
    default:
      return LH_DRV_ERR_STATE;
  }

  ctx->op_started_us = now_us;

  if (ctx->vt->start_read == NULL) {
    /* A driver with nothing to start is ready immediately — a GPIO pin has no
     * conversion to wait for. It still goes through READING so that every
     * component takes the same path through the scheduler. */
    ctx->state = (uint8_t)LH_DRV_STATE_READING;
    return LH_DRV_OK;
  }

  const lh_drv_err_t err = ctx->vt->start_read(ctx);
  if (err == LH_DRV_OK) ctx->state = (uint8_t)LH_DRV_STATE_READING;
  return fail(ctx, err);
}

lh_drv_err_t lh_driver_poll(
    lh_driver_ctx_t *ctx,
    lh_reading_t *out,
    uint8_t channel,
    int64_t now_us) {
  if (ctx == NULL || out == NULL || ctx->vt == NULL) return LH_DRV_ERR_STATE;
  if (ctx->state == (uint8_t)LH_DRV_STATE_FAULTED) return LH_DRV_ERR_FAULTED;
  if (channel >= ctx->vt->channel_count) return LH_DRV_ERR_NO_CHANNEL;

  if (ctx->state != (uint8_t)LH_DRV_STATE_READING &&
      ctx->state != (uint8_t)LH_DRV_STATE_READY) {
    return LH_DRV_ERR_STATE;
  }

  lh_drv_err_t err = ctx->vt->poll(ctx, out, channel);

  if (err == LH_DRV_ERR_NOT_READY) {
    if (elapsed_ms(ctx, now_us) <= (int64_t)lh_driver_op_timeout_ms(ctx->vt)) {
      return LH_DRV_ERR_NOT_READY;
    }
    /* The deadline is the whole reason poll() takes a clock. A driver stuck on
     * a bus that never releases returns NOT_READY for ever, and NOT_READY is
     * not counted as an error — so without this the component would be polled
     * at full rate until the device was power-cycled, which is risk R3.2
     * arriving through the front door. */
    err = LH_DRV_ERR_TIMEOUT;
  }

  if (err == LH_DRV_OK) {
    out->channel = channel;
    ctx->state = (uint8_t)LH_DRV_STATE_READY;
    /* The one place the error count is cleared: a reading came out. */
    ctx->consecutive_errors = 0;
    return LH_DRV_OK;
  }

  /*
   * A hard error ends the measurement.
   *
   * Leaving the context in READING would make the next start_read refuse with
   * "already measuring", so a component that hit one bus error would never be
   * asked for another reading and would sit healthy-looking and silent — worse
   * than faulting, because no counter would ever say so. Going back to IDLE is
   * what makes the fault threshold reachable at all: three errors in a row is
   * only observable if a second attempt is possible.
   */
  const lh_drv_err_t counted = fail(ctx, err);
  if (ctx->state != (uint8_t)LH_DRV_STATE_FAULTED) ctx->state = (uint8_t)LH_DRV_STATE_IDLE;
  return counted;
}

lh_drv_err_t lh_driver_sleep(lh_driver_ctx_t *ctx) {
  if (ctx == NULL || ctx->vt == NULL) return LH_DRV_ERR_STATE;
  if (ctx->state == (uint8_t)LH_DRV_STATE_FAULTED) return LH_DRV_ERR_FAULTED;
  if (ctx->state == (uint8_t)LH_DRV_STATE_EMPTY) return LH_DRV_ERR_STATE;

  if (ctx->vt->sleep != NULL) {
    const lh_drv_err_t err = ctx->vt->sleep(ctx);
    if (err != LH_DRV_OK) return fail(ctx, err);
  }
  ctx->state = (uint8_t)LH_DRV_STATE_SLEEPING;
  return LH_DRV_OK;
}

void lh_driver_clear_fault(lh_driver_ctx_t *ctx, int64_t now_us) {
  if (ctx == NULL || ctx->vt == NULL) return;
  if (ctx->state != (uint8_t)LH_DRV_STATE_FAULTED) return;

  ctx->consecutive_errors = 0;
  ctx->op_started_us = now_us;
  ctx->state = (uint8_t)(ctx->vt->warmup_ms > 0u ? LH_DRV_STATE_WARMUP : LH_DRV_STATE_IDLE);
}
