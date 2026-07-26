#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Static sensor-driver registry and its dispatch layer. Roadmap T3.1.
 *
 * Etap 3's motto is that adding a sensor is a new file, never a change to the
 * main loop. That only holds if the loop talks to sensors through one shape,
 * and this header is that shape: a function table per driver type, a fixed-size
 * context per instance, and a registry that is an array filled in at compile
 * time. There is no registration call, no factory, and nothing to allocate — a
 * node that has been running for six weeks has exactly the drivers it booted
 * with, in exactly the memory it booted with.
 *
 * The one design decision worth defending is the split between `start_read` and
 * `poll`. A BME680 needs roughly 180 ms to finish a gas measurement, and a
 * blocking read would hold the cooperative loop for that whole time — which on
 * this device does not mean "slow", it means the radio's receive window closes
 * unattended and frames are lost only while a sensor happens to be measuring.
 * That is risk R3.1, and it is the worst kind of fault to diagnose because the
 * symptom appears in the radio and the cause is in the sensor. So the split is
 * mandatory for every driver, including the ones that could answer instantly:
 * a GPIO read gains nothing from it, but a uniform interface means the
 * scheduler has no blocking path available to it at all, rather than one it is
 * trusted not to take.
 *
 * Readings are `int32_t` with the scale factor living in the JSON manifest, not
 * `float`. Rules compare integers, so the comparison is deterministic across
 * host and node; CBOR encodes a small integer in fewer bytes than a float; and
 * nothing in the critical path touches an FPU the ESP32-C3 does not have.
 * Temperature travels as milli-degrees, humidity as milli-percent, pressure as
 * pascals. See risk R3.3.
 */

/** Components one node can hold. Matches CONTRIBUTING.md §1.1's hard limit. */
#define LH_MAX_COMPONENTS 8u

/** Per-instance scratch, inside the context: drivers never own heap. Risk R3.5. */
#define LH_DRIVER_SCRATCH_SIZE 32u

/**
 * Consecutive failures before a component is taken out of the rotation.
 *
 * Three rather than one because a single bus error is normal — a NACK during a
 * measurement, a marginal pull-up on a hot afternoon — and a node that retires
 * a working sensor on the first hiccup is worse than one that tolerates a few.
 * Three rather than ten because the point is to stop *hammering* a broken bus:
 * a shorted SDA line makes every transaction cost its full timeout, and eight
 * components retrying forever is how a node stops answering the radio at all.
 * Risk R3.2 — the node must stay alive and reachable with a dead sensor on it.
 */
#define LH_DRIVER_FAULT_THRESHOLD 3u

/**
 * Deadline for one start_read/poll cycle, when the driver declares no warmup.
 *
 * A driver that never answers is indistinguishable from one that is still
 * working, so somebody has to hold a clock. The alternative — trusting the
 * driver to time itself out — puts the safety property inside the component
 * most likely to be the thing that is broken.
 */
#define LH_DRIVER_OP_TIMEOUT_FLOOR_MS 250u

/**
 * How much longer than its declared warmup a driver may take before it is
 * considered hung. Four rather than two: a BME680's gas heater is specified at
 * ~180 ms but stretches under a cold start, and a timeout that fires during
 * normal operation trains everyone to raise it without looking.
 */
#define LH_DRIVER_OP_TIMEOUT_FACTOR 4u

typedef enum {
  LH_DRV_OK = 0,
  LH_DRV_ERR_BUS = -1,
  LH_DRV_ERR_TIMEOUT = -2,
  /** Warming up or mid-measurement. Not a failure — see lh_driver_is_error(). */
  LH_DRV_ERR_NOT_READY = -3,
  LH_DRV_ERR_CRC = -4,
  LH_DRV_ERR_RANGE = -5,
  /** The call does not apply in the context's current state. */
  LH_DRV_ERR_STATE = -6,
  /** Asked for a channel the driver does not have. */
  LH_DRV_ERR_NO_CHANNEL = -7,
  /** The component has been retired after LH_DRIVER_FAULT_THRESHOLD errors. */
  LH_DRV_ERR_FAULTED = -8,
} lh_drv_err_t;

/**
 * One sample from one channel.
 *
 * `quality` is 0..100 and exists for risk R3.6, the zombie sensor: a device
 * that still acknowledges its address and returns plausible-looking rubbish.
 * A driver that knows its reading is suspect says so here, and the rule engine
 * refuses to act on a zero-quality sample rather than clicking a relay at 3am
 * on a number nobody checked.
 */
typedef struct {
  int32_t value;   /* integer-scaled; the manifest carries the scale factor */
  uint8_t channel; /* 0-based index into the driver's channels             */
  uint8_t quality; /* 0..100; 0 means "do not act on this"                 */
  int64_t ts_us;   /* when the sample was taken, not when it was collected */
} lh_reading_t;

typedef struct lh_driver_ctx_s lh_driver_ctx_t;

/**
 * A driver type. One static instance per supported sensor, in LH_DRIVERS[].
 *
 * Every function pointer may be NULL except `poll`; a driver with nothing to do
 * on init or sleep says so by omission rather than by shipping an empty stub.
 * `poll` is the one call that has no sensible default.
 */
typedef struct {
  /** Matches the manifest's "id" exactly. The coherence test in T3.6 enforces it. */
  const char *name;

  /**
   * Stable wire identity. Never changed, never reused — a retired id stays
   * RESERVED in docs/type-ids.md. Risk R3.4: an id that gets recycled means a
   * host showing BME680 readings that a node produced from a GPIO pin.
   */
  uint16_t type_id;

  uint8_t channel_count;
  uint16_t warmup_ms;       /* after init, before the first reading is trusted */
  uint16_t min_interval_ms; /* the scheduler will not poll faster than this    */

  lh_drv_err_t (*probe)(uint8_t bus_addr);
  lh_drv_err_t (*init)(lh_driver_ctx_t *ctx, uint8_t bus_addr);
  /** Must return in well under a millisecond. Starts work; never waits for it. */
  lh_drv_err_t (*start_read)(lh_driver_ctx_t *ctx);
  /** LH_DRV_ERR_NOT_READY until the measurement completes. Never blocks. */
  lh_drv_err_t (*poll)(lh_driver_ctx_t *ctx, lh_reading_t *out, uint8_t channel);
  lh_drv_err_t (*sleep)(lh_driver_ctx_t *ctx);
} lh_driver_vtable_t;

/**
 * Instance lifecycle.
 *
 * READY is separate from IDLE because a multi-channel sensor takes one
 * measurement and yields several samples from it. Collapsing the two would mean
 * the first successful poll ended the measurement, and channels 1..3 of a
 * BME680 would each need their own 180 ms cycle — four times the airtime-
 * adjacent budget for data the sensor already had in a register.
 */
typedef enum {
  LH_DRV_STATE_EMPTY = 0, /* no driver bound                                  */
  LH_DRV_STATE_WARMUP,    /* bound and initialised; readings not yet trusted   */
  LH_DRV_STATE_IDLE,      /* ready; no measurement in flight                   */
  LH_DRV_STATE_READING,   /* start_read() accepted; waiting on poll()          */
  LH_DRV_STATE_READY,     /* measurement complete; channels collectable        */
  LH_DRV_STATE_SLEEPING,  /* low power; a start_read() wakes it through warmup */
  LH_DRV_STATE_FAULTED,   /* retired from the rotation; the bus is not touched */
} lh_drv_state_t;

/**
 * Per-instance state. Fixed size, and that size is a budget.
 *
 * `scratch` is inside the struct rather than pointed at from it, which is the
 * whole of risk R3.5: eight drivers each holding a small heap block is how a
 * node ends up with plenty of free memory and no contiguous room for a radio
 * buffer, three weeks in.
 */
struct lh_driver_ctx_s {
  const lh_driver_vtable_t *vt;
  uint8_t bus_addr;
  uint8_t component_id; /* from the config; the id telemetry is reported under */
  uint8_t state;        /* lh_drv_state_t */
  /**
   * Consecutive failures, cleared by any success.
   *
   * Consecutive rather than cumulative on purpose: a total error count only
   * ever rises, so a threshold on it retires every component that has been
   * running long enough, which is the opposite of what it is for.
   */
  uint8_t consecutive_errors;
  int64_t op_started_us; /* start of the current warmup or measurement */
  uint8_t scratch[LH_DRIVER_SCRATCH_SIZE];
};

/**
 * The registry itself, defined once per firmware image.
 *
 * An explicit array in driver_registry.c rather than linker-section magic. The
 * roadmap makes the reason plain: section attributes work until a toolchain
 * upgrade, and when they stop working they do it silently — drivers simply are
 * not there. An array of pointers is boring, greppable, and its contents are a
 * declaration of what this image can talk to.
 *
 * Declared here and defined by the firmware (or by a test harness with mock
 * drivers), so firmware/common stays free of any particular sensor.
 */
extern const lh_driver_vtable_t *const LH_DRIVERS[];
extern const uint8_t LH_DRIVER_COUNT;

/** Registry lookup by manifest id. NULL if this image has no such driver. */
const lh_driver_vtable_t *lh_driver_find(const char *name);

/** Registry lookup by wire type id. NULL if this image has no such driver. */
const lh_driver_vtable_t *lh_driver_find_type(uint16_t type_id);

/**
 * True for a result that should count against the fault threshold.
 *
 * LH_DRV_ERR_NOT_READY is the value this exists to exclude. A warming BME680
 * returns it for the better part of a second on every cycle, and counting that
 * as failure would retire the sensor within the first three polls — a bug that
 * looks exactly like broken hardware.
 */
bool lh_driver_is_error(lh_drv_err_t err);

/**
 * Binds a driver to a context and initialises the device.
 *
 * On success the context enters WARMUP (or IDLE, for a driver declaring no
 * warmup). On failure it enters FAULTED immediately rather than after three
 * tries: init failing means the device did not answer at all, and re-running it
 * twice more only delays the same answer while holding the bus.
 */
lh_drv_err_t lh_driver_bind(
    lh_driver_ctx_t *ctx,
    const lh_driver_vtable_t *vt,
    uint8_t bus_addr,
    uint8_t component_id,
    int64_t now_us);

/**
 * Asks the driver to begin a measurement.
 *
 * Returns LH_DRV_ERR_NOT_READY while the declared warmup is still running, and
 * that is the normal path, not an error: the caller retries on its next tick.
 * The driver's own start_read is not invoked until the warmup has elapsed, so a
 * driver never has to implement warmup accounting itself — CONTRIBUTING.md §1.3
 * makes this data correctness rather than an optimisation, and a rule that
 * every driver re-implements is a rule that one driver gets wrong.
 */
lh_drv_err_t lh_driver_start_read(lh_driver_ctx_t *ctx, int64_t now_us);

/**
 * Collects one channel of a measurement.
 *
 * LH_DRV_ERR_NOT_READY means "ask again later" and is the expected answer for
 * most of a BME680's measurement window. The first success moves the context to
 * READY, where the remaining channels are collected without starting the sensor
 * again; the next start_read begins a fresh measurement.
 *
 * Enforces the operation deadline: a driver that never leaves NOT_READY is
 * failed with LH_DRV_ERR_TIMEOUT rather than being polled for ever.
 */
lh_drv_err_t lh_driver_poll(
    lh_driver_ctx_t *ctx,
    lh_reading_t *out,
    uint8_t channel,
    int64_t now_us);

/** Puts the device in its low-power state. A no-op for drivers without one. */
lh_drv_err_t lh_driver_sleep(lh_driver_ctx_t *ctx);

/** True once the component has been retired. It is skipped, never retried. */
bool lh_driver_is_faulted(const lh_driver_ctx_t *ctx);

/**
 * Returns a faulted component to WARMUP and clears its error count.
 *
 * Deliberately manual. A component that recovers on its own is a component that
 * silently re-enters the rotation, so the counter that said something was wrong
 * gets reset by the thing that was wrong — recovery is a decision the host
 * makes, having seen the fault in telemetry.
 */
void lh_driver_clear_fault(lh_driver_ctx_t *ctx, int64_t now_us);

/** The operation deadline this driver's declared warmup implies, in ms. */
uint32_t lh_driver_op_timeout_ms(const lh_driver_vtable_t *vt);

#ifdef __cplusplus
}
#endif
