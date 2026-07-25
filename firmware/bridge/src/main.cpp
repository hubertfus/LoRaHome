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
static lh_slip_decoder_t g_serialDecoder;

// Risk R1.1: a TX buffer sized for the typical frame survives every test and
// overruns the first time a payload happens to be all 0xC0. Derived from the
// RX buffer rather than written as a literal, so the two cannot drift apart.
static uint8_t g_slipEncodeBuf[LH_SLIP_ENCODED_MAX(sizeof(g_serialRxBuf))];

// Encoded lengths are uint16_t all the way through the codec. At 512 B of RX
// buffer that is nowhere near the limit — but the day somebody raises the RX
// buffer to 40 kB, this fails the build instead of silently truncating.
static_assert(LH_SLIP_ENCODED_MAX(sizeof(g_serialRxBuf)) <= UINT16_MAX,
              "SLIP encoded frame no longer fits the uint16_t length type");

static void sendToHost(const uint8_t* frame, size_t len) {
  const uint16_t encodedLen = lh_slip_encode(frame, static_cast<uint16_t>(len), g_slipEncodeBuf,
                                             static_cast<uint16_t>(sizeof(g_slipEncodeBuf)));
  if (encodedLen == 0) return;  // no room for the worst case — drop rather than corrupt
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
  lh_slip_init(&g_serialDecoder, g_serialRxBuf, sizeof(g_serialRxBuf));
}

void loop() {
  g_transport.poll();

  while (Serial.available() > 0) {
    const uint8_t byte = static_cast<uint8_t>(Serial.read());
    // The frame stays valid only until the next feed, so it is handled here and
    // now. LH_SLIP_ERROR needs no branch: the decoder has already counted the
    // damaged frame and resynchronised itself.
    if (lh_slip_feed(&g_serialDecoder, byte) == LH_SLIP_FRAME_READY) {
      handleFrameFromHost(g_serialDecoder.buf, g_serialDecoder.len);
    }
  }
}
