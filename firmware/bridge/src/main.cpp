#include <Arduino.h>
#include <RadioLib.h>
#include "duty_cycle_tracker.h"
#include "lorahome/airtime.h"
#include "lorahome/lora_transport.h"
#include "lorahome/protocol.h"
#include "lorahome/slip.h"

using namespace lorahome;

static SX1262 g_radio = new Module(/*cs=*/18, /*irq=*/26, /*rst=*/23, /*gpio=*/33);
static LoraTransport g_transport(&g_radio);

// ETSI EN 300 220: 1% duty cycle over a rolling 1h window on 868MHz.
static DutyCycleTracker g_dutyCycle(0.01f, 60UL * 60UL * 1000UL);

static uint8_t g_serialRxBuf[512];
static lorahome_slip_decoder_t g_serialDecoder;

static uint8_t g_slipEncodeBuf[2 * sizeof(g_serialRxBuf) + 2];

static void sendToHost(const uint8_t* frame, size_t len) {
  const int encodedLen = lorahome_slip_encode(frame, len, g_slipEncodeBuf, sizeof(g_slipEncodeBuf));
  if (encodedLen < 0) return; // frame too large for the encode buffer — drop rather than corrupt
  Serial.write(g_slipEncodeBuf, encodedLen);
}

static void onLoraFrameReceived(const uint8_t* data, size_t len, uint16_t srcId) {
  (void)srcId;
  sendToHost(data, len);
}

static void handleFrameFromHost(const uint8_t* frame, size_t len) {
  lorahome_header_t header;
  if (!lorahome_decode_header(frame, len, &header)) return;

  const lorahome_airtime_params_t airtimeParams = {
      static_cast<uint16_t>(len),
      /*spreading_factor=*/9,
      /*bandwidth_hz=*/125000,
      /*coding_rate=*/1,
      /*preamble_symbols=*/8,
  };
  const uint32_t durationMs = static_cast<uint32_t>(lorahome_compute_airtime_ms(&airtimeParams));

  if (!g_dutyCycle.tryRecord(durationMs, millis())) {
    // Refuse to transmit — the Host's own Duty Cycle Guard should have
    // caught this already; this is the second, independent line of
    // defense (ARCHITECTURE.md §7).
    return;
  }

  g_transport.send(frame, len, header.dst_id);
  // TODO(retransmit): if header.flags & LORAHOME_FLAG_ACK_REQ, start a
  // retry timer and resend on timeout up to a configured retry limit.
}

void setup() {
  Serial.begin(115200);
  g_transport.begin();
  g_transport.onReceive(onLoraFrameReceived);
  lorahome_slip_decoder_init(&g_serialDecoder, g_serialRxBuf, sizeof(g_serialRxBuf));
}

void loop() {
  g_transport.poll();

  while (Serial.available() > 0) {
    const uint8_t byte = static_cast<uint8_t>(Serial.read());
    if (lorahome_slip_decoder_feed(&g_serialDecoder, byte)) {
      handleFrameFromHost(g_serialDecoder.buf, g_serialDecoder.len);
      lorahome_slip_decoder_reset(&g_serialDecoder);
    }
  }
}
