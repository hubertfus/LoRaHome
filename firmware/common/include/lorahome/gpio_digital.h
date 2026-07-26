#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lorahome/driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Digital input with debouncing. Roadmap T3.4.
 *
 * A mechanical contact does not close once. It closes, bounces open, closes
 * again — a dozen or more transitions inside a few milliseconds, all of them
 * electrically real and none of them meaning anything. Reported raw, a door
 * sensor produces twenty "door opened" events for one door, and any rule
 * counting them or toggling on them is wrong from the first use.
 *
 * The state machine here accepts a new level only once it has been observed at
 * that level continuously for the configured window. That is all it does, and
 * it is worth its own file because the alternative — every driver and rule
 * doing its own filtering — is how one of them ends up not doing it.
 *
 * A digital read is instant, and this driver still goes through
 * start_read/poll like every other. It gains nothing from the split. The point
 * is that the scheduler has no fast path to special-case, so there is no
 * blocking API for a future driver to reach for. Uniformity beats convenience
 * (T3.1, risk R3.1).
 */

/** Pins that can be bound at once. Eight, matching the component limit. */
#define LH_GPIO_MAX_INSTANCES 8u

/** Window used when nothing has configured the pin. */
#define LH_GPIO_DEFAULT_DEBOUNCE_MS 50u

/**
 * Longest debounce that can be configured.
 *
 * A second is already far beyond any contact's bounce, and the cap exists so a
 * configuration typo produces a rejected value rather than an input that
 * appears dead for an hour.
 */
#define LH_GPIO_MAX_DEBOUNCE_MS 1000u

/** Polled often, because a debounce window is only as good as its sampling. */
#define LH_GPIO_MIN_INTERVAL_MS 10u

typedef enum {
  /** The debounced level: 0 or 1, after `active_low` has been applied. */
  LH_GPIO_CH_LEVEL = 0,
  /**
   * Accepted transitions since bind, saturating.
   *
   * A counter rather than an event, because a cooperative scheduler has nowhere
   * to deliver an event to. It is also what makes "twenty bounces produced
   * exactly one event" a thing a test can assert rather than a thing a person
   * watches an LED for.
   */
  LH_GPIO_CH_TRANSITIONS = 1,
  LH_GPIO_CHANNELS = 2,
} lh_gpio_channel_t;

/** Reads one pin. True means the pin is electrically high. */
typedef struct {
  bool (*read_pin)(uint8_t pin, void *user);
  int64_t (*now_us)(void *user);
  void *user;
} lh_gpio_io_t;

/** Installs the pin access. Must be called before any component is bound. */
void lh_gpio_set_io(const lh_gpio_io_t *io);

/**
 * Sets a pin's debounce window and polarity, before it is bound.
 *
 * Configuration arrives from the host in Etap 4; until then this is how a pin
 * gets anything other than the defaults. Returns false for a window above
 * LH_GPIO_MAX_DEBOUNCE_MS or when the configuration table is full.
 *
 * `active_low` inverts the reported level, which is the normal wiring for a
 * button: pulled up, shorted to ground when pressed. Reporting that as 0 would
 * make every rule in the UI read backwards.
 */
bool lh_gpio_configure(uint8_t pin, uint16_t debounce_ms, bool active_low);

/** Forgets every pin configuration and releases every instance. */
void lh_gpio_reset(void);

/** Instance slots currently in use. Diagnostics, and the slot-leak test. */
uint8_t lh_gpio_instances_in_use(void);

/** The registry entry. Bound through lh_driver_bind(); `bus_addr` is the pin. */
extern const lh_driver_vtable_t LH_GPIO_DIGITAL_DRIVER;

#ifdef __cplusplus
}
#endif
