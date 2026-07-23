#include <Arduino.h>
#include <RadioLib.h>
#include "config_store.h"
#include "drivers/driver_registry.h"
#include "lorahome/lora_transport.h"
#include "lorahome/rule_evaluator.h"

using namespace lorahome;

// Static pools only — no dynamic allocation (CONTRIBUTING.md §1.1).
#ifndef LORAHOME_MAX_COMPONENTS
#define LORAHOME_MAX_COMPONENTS 8
#endif
#ifndef LORAHOME_MAX_RULES
#define LORAHOME_MAX_RULES 16
#endif

struct ComponentInstance {
  const DriverVTable* driver;
  uint8_t addr;
  uint32_t next_poll_ms;
};

static ComponentInstance g_components[LORAHOME_MAX_COMPONENTS];
static uint8_t g_component_count = 0;

static lorahome_rule_t g_rules[LORAHOME_MAX_RULES];
static lorahome_rule_state_t g_rule_states[LORAHOME_MAX_RULES];
static uint8_t g_rule_count = 0;

static SX1262 g_radio = new Module(/*cs=*/18, /*irq=*/26, /*rst=*/23, /*gpio=*/33);
static LoraTransport g_transport(&g_radio);
static ConfigStore g_configStore;

static void onFrameReceived(const uint8_t* data, size_t len, uint16_t srcId) {
  lorahome_header_t header;
  if (!lorahome_decode_header(data, len, &header)) return;
  // Frame-type dispatch (CONFIG_BEGIN/FRAG/COMMIT, CMD, CAPABILITY_REQ) wires
  // up here in a later pass — this scaffold establishes the static
  // structures and control flow, not the full protocol state machine.
  (void)srcId;
}

void setup() {
  Serial.begin(115200);

  g_configStore.begin();
  g_transport.begin();
  g_transport.onReceive(onFrameReceived);

  // TODO(config): populate g_components/g_rules from the active NVS slot
  // via g_configStore.loadActive() once a config has been received.
  for (uint8_t i = 0; i < g_component_count; i++) {
    g_components[i].driver->init(g_components[i].addr);
    g_components[i].next_poll_ms = millis() + g_components[i].driver->get_warmup_ms();
  }
  for (uint8_t i = 0; i < g_rule_count; i++) {
    lorahome_rule_state_init(&g_rule_states[i]);
  }
}

void loop() {
  g_transport.poll();

  const uint32_t now = millis();
  for (uint8_t i = 0; i < g_component_count; i++) {
    ComponentInstance& component = g_components[i];
    if (now < component.next_poll_ms) continue;

    SensorReading readings[4];
    uint8_t reading_count = 0;
    if (component.driver->read(component.addr, readings, 4, &reading_count)) {
      for (uint8_t r = 0; r < reading_count; r++) {
        for (uint8_t ruleIdx = 0; ruleIdx < g_rule_count; ruleIdx++) {
          if (g_rules[ruleIdx].src_sensor_id != readings[r].output_index) continue;
          if (lorahome_rule_step(&g_rule_states[ruleIdx], &g_rules[ruleIdx], readings[r].value, now)) {
            // TODO(actions): dispatch g_rules[ruleIdx].action_id/action_param
            // to the local actuator registry.
          }
        }
      }
    }

    component.next_poll_ms = now + 1000; // TODO(config): use the configured interval_s per component
  }
}
