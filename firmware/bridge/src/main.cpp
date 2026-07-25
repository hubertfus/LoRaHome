/*
 * The Bridge: Serial <-> LoRa frame translator. Roadmap T1.4.
 *
 * This file is glue and nothing else. Every decision about whether a frame is
 * well formed, whether it may go on air and which counter a rejection belongs
 * to lives in firmware/common/src/bridge_core.c, where it is tested a thousand
 * frames at a time on a host. What is left here is the four things that need
 * real hardware: a UART, a radio, a clock, and the wiring between them.
 *
 * That split is the point. The forwarding logic that ships is the same code the
 * end-to-end test exercises, rather than a second implementation that resembles
 * it.
 */
#include <Arduino.h>
#include <RadioLib.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "duty_cycle_tracker.h"
#include "lorahome/airtime.h"
#include "lorahome/bridge_core.h"
#include "lorahome/lora_transport.h"
#include "serial_link.h"

using namespace lorahome;

static SX1262 g_radio = new Module(/*cs=*/18, /*irq=*/26, /*rst=*/23, /*gpio=*/33);
static LoraTransport g_transport(&g_radio);
static SerialLink g_serial;

// ETSI EN 300 220: 1% duty cycle over a rolling 1 h window on 868 MHz. At this
// profile a full 230 B frame costs 1147.9 ms, so the budget permits roughly one
// such frame every 115 s per channel.
static DutyCycleTracker g_dutyCycle(0.01f, 60UL * 60UL * 1000UL);

// One global instance, statically allocated, for the lifetime of the program —
// the project's standing rule for anything that owns a buffer.
static lh_bridge_ctx_t g_bridge;

/**
 * Duty cycle veto.
 *
 * Airtime is derived from LoraProfile so the tracker spends exactly what the
 * radio is about to use. The Host has its own guard; this is the second,
 * independent line of defence (ARCHITECTURE.md §7), and it is the one that
 * still works when the Host is wrong.
 */
static bool allowTransmit(void* /*user*/, uint16_t len) {
  const lorahome_airtime_params_t params = {
      len,
      LoraProfile::kSpreadingFactor,
      LoraProfile::kBandwidthHz,
      LoraProfile::kAirtimeCodingRate,
      static_cast<uint8_t>(LoraProfile::kPreambleSymbols),
  };
  const uint32_t durationMs = static_cast<uint32_t>(lorahome_compute_airtime_ms(&params));
  return g_dutyCycle.tryRecord(durationMs, millis());
}

/** Validated frame on its way to the air. The pointer aims into the SLIP buffer. */
static bool emitToRadio(void* /*user*/, const uint8_t* frame, uint16_t len) {
  return g_transport.send(frame, len, /*dstId=*/0);
}

/** Already SLIP-encoded by the core; this only has to put bytes on the wire. */
static bool emitToHost(void* /*user*/, const uint8_t* bytes, uint16_t len) {
  return g_serial.write(bytes, len) == len;
}

/**
 * The platform half of the diagnostic readout.
 *
 * `heap_largest_block` is here for the same reason §0.4 insists on it: 180 kB
 * free in fragments too small to hold a 4 kB buffer is a device that dies in
 * week three, and the free total on its own cannot show that. `min_free_ever`
 * catches the transient low-water mark that a periodic sample would walk past.
 */
static void readHealth(void* /*user*/, lh_bridge_stat_t* out) {
  out->uptime_ms = millis();
  out->heap_free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  out->heap_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  out->heap_min_free_ever = esp_get_minimum_free_heap_size();
  out->ring_overrun = g_serial.overrunCount();
  out->ring_hwm = g_serial.ring().stat_hwm;
  out->ring_capacity = LH_UART_RING_SIZE;
}

static void onLoraFrameReceived(const uint8_t* data, size_t len, uint16_t /*srcId*/) {
  lh_bridge_on_radio_frame(&g_bridge, data, static_cast<uint16_t>(len));
}

void setup() {
  g_serial.begin();

  lh_bridge_init(&g_bridge, emitToRadio, nullptr, emitToHost, nullptr);
  lh_bridge_set_duty_cycle_guard(&g_bridge, allowTransmit, nullptr);
  lh_bridge_set_health_source(&g_bridge, readHealth, nullptr);

  g_transport.begin();
  g_transport.onReceive(onLoraFrameReceived);
}

void loop() {
  // Completes whatever the last DIO1 interrupt started — a finished
  // transmission, or a packet waiting to be read.
  g_transport.poll();

  // Drained in blocks: the reader task is filling the ring concurrently, and one
  // bulk pop per pass costs one fence instead of one per byte. All protocol work
  // happens on this side of the ring, never on the receive path (risk R1.2).
  static uint8_t block[128];
  for (;;) {
    const uint16_t received = g_serial.read(block, sizeof(block));
    if (received == 0) break;
    lh_bridge_feed_serial(&g_bridge, block, received);
  }
}
