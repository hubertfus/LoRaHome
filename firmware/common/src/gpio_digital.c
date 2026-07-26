/*
 * Digital input with debouncing. Roadmap T3.4. Contract in gpio_digital.h.
 *
 * The whole component is one state machine with two states, and the only thing
 * worth being careful about is which clock the window is measured against.
 * It is measured against observations, not against wall time: a level counts as
 * stable when every sample since the change has agreed, and the window elapses
 * between samples. That distinction is what makes the behaviour identical
 * whether the scheduler polls every 10 ms or every 2 ms — and it is why a poll
 * that never happens cannot silently accept a transition.
 */
#include "lorahome/gpio_digital.h"

#include <stddef.h>
#include <string.h>

/* The instance table is per-component RAM and it is a budget like any other. */
_Static_assert(LH_GPIO_MAX_INSTANCES == LH_MAX_COMPONENTS, "one pin per component at most");

typedef struct {
  bool in_use;
  uint8_t pin;
  uint16_t debounce_ms;
  bool active_low;

  /** The level the driver reports: the last one that survived the window. */
  bool stable_level;

  /**
   * The level currently under consideration, and since when.
   *
   * Equal to `stable_level` when nothing is pending. Keeping the candidate
   * rather than a simple "changed at" timestamp is what makes bounce cheap:
   * every sample that disagrees with the candidate restarts the window, so
   * twenty transitions in five milliseconds cost twenty comparisons and
   * produce nothing.
   */
  bool candidate_level;
  int64_t candidate_since_us;

  uint32_t transitions;
  int64_t last_sample_us;
} instance_t;

typedef struct {
  bool set;
  uint8_t pin;
  uint16_t debounce_ms;
  bool active_low;
} pin_config_t;

static instance_t g_instances[LH_GPIO_MAX_INSTANCES];
static pin_config_t g_configs[LH_GPIO_MAX_INSTANCES];
static lh_gpio_io_t g_io;
static bool g_io_set = false;

void lh_gpio_set_io(const lh_gpio_io_t *io) {
  if (io == NULL) {
    memset(&g_io, 0, sizeof g_io);
    g_io_set = false;
    return;
  }
  g_io = *io;
  g_io_set = io->read_pin != NULL && io->now_us != NULL;
}

void lh_gpio_reset(void) {
  memset(g_instances, 0, sizeof g_instances);
  memset(g_configs, 0, sizeof g_configs);
}

uint8_t lh_gpio_instances_in_use(void) {
  uint8_t count = 0;
  for (uint8_t i = 0; i < LH_GPIO_MAX_INSTANCES; i++) {
    if (g_instances[i].in_use) count++;
  }
  return count;
}

bool lh_gpio_configure(uint8_t pin, uint16_t debounce_ms, bool active_low) {
  if (debounce_ms > LH_GPIO_MAX_DEBOUNCE_MS) return false;

  /* Reconfiguring a pin replaces its entry rather than adding a second one:
   * two rows for one pin would make the effective setting depend on scan
   * order, which is the kind of thing that works until the table fills up. */
  for (uint8_t i = 0; i < LH_GPIO_MAX_INSTANCES; i++) {
    if (g_configs[i].set && g_configs[i].pin == pin) {
      g_configs[i].debounce_ms = debounce_ms;
      g_configs[i].active_low = active_low;
      return true;
    }
  }
  for (uint8_t i = 0; i < LH_GPIO_MAX_INSTANCES; i++) {
    if (!g_configs[i].set) {
      g_configs[i].set = true;
      g_configs[i].pin = pin;
      g_configs[i].debounce_ms = debounce_ms;
      g_configs[i].active_low = active_low;
      return true;
    }
  }
  return false;
}

static const pin_config_t *config_for(uint8_t pin) {
  for (uint8_t i = 0; i < LH_GPIO_MAX_INSTANCES; i++) {
    if (g_configs[i].set && g_configs[i].pin == pin) return &g_configs[i];
  }
  return NULL;
}

static instance_t *instance_of(lh_driver_ctx_t *ctx) {
  const uint8_t slot = ctx->scratch[0];
  if (slot >= LH_GPIO_MAX_INSTANCES) return NULL;
  instance_t *inst = &g_instances[slot];
  return inst->in_use ? inst : NULL;
}

/** The pin as the rest of the system sees it, with polarity applied. */
static bool logical_level(const instance_t *inst) {
  const bool raw = g_io.read_pin(inst->pin, g_io.user);
  return inst->active_low ? !raw : raw;
}

static lh_drv_err_t gpio_probe(uint8_t bus_addr) {
  /*
   * Every pin "exists" — there is nothing to interrogate, unlike an I2C
   * address. The only failure a probe can honestly report is that no pin access
   * has been installed, so this exists to answer that rather than to pretend it
   * checked something.
   */
  (void)bus_addr;
  return g_io_set ? LH_DRV_OK : LH_DRV_ERR_BUS;
}

static lh_drv_err_t gpio_init(lh_driver_ctx_t *ctx, uint8_t bus_addr) {
  if (!g_io_set) return LH_DRV_ERR_BUS;

  uint8_t slot = 0xFFu;
  for (uint8_t i = 0; i < LH_GPIO_MAX_INSTANCES; i++) {
    if (!g_instances[i].in_use) {
      slot = i;
      break;
    }
  }
  if (slot == 0xFFu) return LH_DRV_ERR_STATE;

  instance_t *inst = &g_instances[slot];
  memset(inst, 0, sizeof *inst);
  inst->in_use = true;
  inst->pin = bus_addr;

  const pin_config_t *config = config_for(bus_addr);
  inst->debounce_ms = (config != NULL) ? config->debounce_ms : LH_GPIO_DEFAULT_DEBOUNCE_MS;
  inst->active_low = (config != NULL) ? config->active_low : false;

  /*
   * The starting level is whatever the pin reads now, adopted without a window.
   *
   * The alternative is to start at 0 and let the first real read look like a
   * transition, which would report a door as having just opened every time the
   * node reboots — and reboots are exactly when nobody is watching.
   */
  inst->stable_level = logical_level(inst);
  inst->candidate_level = inst->stable_level;
  inst->candidate_since_us = g_io.now_us(g_io.user);
  inst->last_sample_us = inst->candidate_since_us;

  ctx->scratch[0] = slot;
  return LH_DRV_OK;
}

/** Samples the pin and advances the debounce state machine. */
static void sample(instance_t *inst, int64_t now_us) {
  const bool level = logical_level(inst);
  inst->last_sample_us = now_us;

  if (level == inst->stable_level) {
    /* Whatever was pending is over; the line came back. */
    inst->candidate_level = inst->stable_level;
    inst->candidate_since_us = now_us;
    return;
  }

  if (level != inst->candidate_level) {
    /* A new candidate: the window starts again from here. This is the branch
     * that eats bounce — each reversal restarts it, so a contact chattering for
     * five milliseconds never accumulates a stable window. */
    inst->candidate_level = level;
    inst->candidate_since_us = now_us;
    /* Deliberately no early return. With a zero window the level is accepted on
     * this very sample, which is the documented pass-through case — a rule
     * reading a fast pulse train depends on it. Returning here instead made a
     * zero window still cost a second poll, which is a debounce of "however
     * often the scheduler happens to run". */
  }

  const int64_t held_ms = (now_us - inst->candidate_since_us) / 1000;
  if (held_ms >= (int64_t)inst->debounce_ms) {
    inst->stable_level = level;
    if (inst->transitions < UINT32_MAX) inst->transitions++;
  }
}

static lh_drv_err_t gpio_poll(lh_driver_ctx_t *ctx, lh_reading_t *out, uint8_t channel) {
  instance_t *inst = instance_of(ctx);
  if (inst == NULL) return LH_DRV_ERR_STATE;
  if (channel >= LH_GPIO_CHANNELS) return LH_DRV_ERR_NO_CHANNEL;

  const int64_t now_us = g_io.now_us(g_io.user);

  /*
   * Sample once per measurement, on the first channel read.
   *
   * Reading the pin again for the transitions channel would sample the line
   * twice within one scheduler tick, so the two channels could disagree — a
   * level of 1 alongside a transition count that has not caught up with it.
   * The dispatch layer's READY state means later channels come from here.
   */
  if (inst->last_sample_us != now_us) sample(inst, now_us);

  out->channel = channel;
  out->ts_us = inst->last_sample_us;
  out->quality = 100;
  out->value = (channel == LH_GPIO_CH_LEVEL) ? (inst->stable_level ? 1 : 0)
                                             : (int32_t)inst->transitions;
  return LH_DRV_OK;
}

const lh_driver_vtable_t LH_GPIO_DIGITAL_DRIVER = {
    .name = "gpio_digital",
    /* 17, matching packages/components/manifests/gpio_digital.json. Never
     * changed, never reused — docs/type-ids.md and risk R3.4. */
    .type_id = 17,
    .channel_count = LH_GPIO_CHANNELS,
    /* No warmup: a pin is readable the instant it is configured, and declaring
     * one would cost a scheduler slot for nothing. */
    .warmup_ms = 0,
    .min_interval_ms = LH_GPIO_MIN_INTERVAL_MS,
    .probe = gpio_probe,
    .init = gpio_init,
    /* Nothing to start. The dispatch layer still routes through READING, so
     * this component takes the same path through the scheduler as a BME680. */
    .start_read = NULL,
    .poll = gpio_poll,
    /* Nothing to power down: an input pin already costs nothing. */
    .sleep = NULL,
};
