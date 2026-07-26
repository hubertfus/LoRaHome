/*
 * On-target GPIO debounce tests (T3.4).
 *
 * The host harness in firmware/common/test/gpio_digital_selftest.c drives the
 * bounce patterns and measures the window. This carries the two behaviours that
 * would be cheapest to reintroduce and most expensive to notice:
 *
 *   - twenty bounces produce one event, not twenty;
 *   - a node that reboots does not report the door as having just opened.
 *
 * Neither is visible from the outside. The first shows up as a rule firing
 * repeatedly on one physical action, the second as an event at three in the
 * morning after a brownout, and both look like a hardware problem.
 *
 * Run with `pio test -e native -f test_gpio_digital`, or `-e esp32dev`.
 */
#include <string.h>
#include <unity.h>

#include "lorahome/driver.h"
#include "lorahome/gpio_digital.h"

const lh_driver_vtable_t* const LH_DRIVERS[] = {&LH_GPIO_DIGITAL_DRIVER};
const uint8_t LH_DRIVER_COUNT = 1;

static bool g_pin_level;
static int64_t g_clock_us;

static bool readPin(uint8_t pin, void* user) {
  (void)pin;
  (void)user;
  return g_pin_level;
}
static int64_t nowUs(void* user) {
  (void)user;
  return g_clock_us;
}

static lh_driver_ctx_t g_ctx;

void setUp(void) {
  g_pin_level = false;
  g_clock_us = 1000000;

  lh_gpio_io_t io;
  io.read_pin = readPin;
  io.now_us = nowUs;
  io.user = nullptr;

  lh_gpio_reset();
  lh_gpio_set_io(&io);
  memset(&g_ctx, 0, sizeof g_ctx);
}
void tearDown(void) {}

static int32_t poll(uint8_t channel) {
  lh_reading_t reading;
  memset(&reading, 0, sizeof reading);
  lh_driver_start_read(&g_ctx, g_clock_us);
  if (lh_driver_poll(&g_ctx, &reading, channel, g_clock_us) != LH_DRV_OK) return -1;
  return reading.value;
}

/** Runs the scheduler for `ms`, polling every millisecond. */
static void runMs(int ms) {
  for (int i = 0; i < ms; i++) {
    g_clock_us += 1000;
    poll(LH_GPIO_CH_LEVEL);
  }
}

static void bindPin(uint8_t pin, uint16_t debounceMs, bool activeLow) {
  lh_gpio_configure(pin, debounceMs, activeLow);
  lh_driver_bind(&g_ctx, &LH_GPIO_DIGITAL_DRIVER, pin, 0, g_clock_us);
}

/** The vtable's declared shape is a contract with the manifest (T3.6). */
void test_driver_declaration(void) {
  TEST_ASSERT_EQUAL_STRING("gpio_digital", LH_GPIO_DIGITAL_DRIVER.name);
  TEST_ASSERT_EQUAL_UINT16(17, LH_GPIO_DIGITAL_DRIVER.type_id);
  TEST_ASSERT_EQUAL_UINT8(2, LH_GPIO_DIGITAL_DRIVER.channel_count);
  TEST_ASSERT_EQUAL_UINT16(0, LH_GPIO_DIGITAL_DRIVER.warmup_ms);
  /* No start_read, and that is the point: the dispatch layer still routes
   * through READING so this component takes the same path as a BME680. */
  TEST_ASSERT_NULL(LH_GPIO_DIGITAL_DRIVER.start_read);
  TEST_ASSERT_NOT_NULL(LH_GPIO_DIGITAL_DRIVER.poll);
}

void test_reboot_does_not_fire_an_event(void) {
  g_pin_level = true;
  bindPin(4, 50, false);

  TEST_ASSERT_EQUAL_INT32(1, poll(LH_GPIO_CH_LEVEL));
  TEST_ASSERT_EQUAL_INT32(0, poll(LH_GPIO_CH_TRANSITIONS));
}

void test_twenty_bounces_are_one_event(void) {
  bindPin(4, 50, false);
  poll(LH_GPIO_CH_LEVEL);

  for (int i = 0; i < 20; i++) {
    g_pin_level = (i % 2) == 0;
    g_clock_us += 250; /* 5 ms of chatter, sampled faster than it bounces */
    poll(LH_GPIO_CH_LEVEL);
  }
  TEST_ASSERT_EQUAL_INT32(0, poll(LH_GPIO_CH_TRANSITIONS));

  g_pin_level = true;
  runMs(60);
  TEST_ASSERT_EQUAL_INT32(1, poll(LH_GPIO_CH_TRANSITIONS));
  TEST_ASSERT_EQUAL_INT32(1, poll(LH_GPIO_CH_LEVEL));
}

void test_glitch_shorter_than_the_window_is_ignored(void) {
  bindPin(4, 50, false);
  poll(LH_GPIO_CH_LEVEL);

  g_pin_level = true;
  runMs(20);
  g_pin_level = false;
  runMs(200);

  TEST_ASSERT_EQUAL_INT32(0, poll(LH_GPIO_CH_TRANSITIONS));
  TEST_ASSERT_EQUAL_INT32(0, poll(LH_GPIO_CH_LEVEL));
}

/** A zero window is a pass-through, accepted on the sample that sees it. */
void test_zero_window_is_immediate(void) {
  bindPin(4, 0, false);
  poll(LH_GPIO_CH_LEVEL);

  g_pin_level = true;
  g_clock_us += 1000;
  TEST_ASSERT_EQUAL_INT32(1, poll(LH_GPIO_CH_LEVEL));
  TEST_ASSERT_EQUAL_INT32(1, poll(LH_GPIO_CH_TRANSITIONS));
}

/** A pulled-up button reads pressed as 1, not as 0. */
void test_active_low(void) {
  g_pin_level = true; /* idle high */
  bindPin(5, 20, true);
  TEST_ASSERT_EQUAL_INT32(0, poll(LH_GPIO_CH_LEVEL));

  g_pin_level = false; /* shorted to ground */
  runMs(30);
  TEST_ASSERT_EQUAL_INT32(1, poll(LH_GPIO_CH_LEVEL));
}

/** A window past the cap is refused rather than silently clamped. */
void test_configuration_is_bounded(void) {
  TEST_ASSERT_TRUE(lh_gpio_configure(4, LH_GPIO_MAX_DEBOUNCE_MS, false));
  TEST_ASSERT_FALSE(lh_gpio_configure(4, LH_GPIO_MAX_DEBOUNCE_MS + 1, false));
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_driver_declaration);
  RUN_TEST(test_reboot_does_not_fire_an_event);
  RUN_TEST(test_twenty_bounces_are_one_event);
  RUN_TEST(test_glitch_shorter_than_the_window_is_ignored);
  RUN_TEST(test_zero_window_is_immediate);
  RUN_TEST(test_active_low);
  RUN_TEST(test_configuration_is_bounded);
  return UNITY_END();
}
