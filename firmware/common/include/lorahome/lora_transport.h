#pragma once

#include <RadioLib.h>

#include "lorahome/protocol.h"
#include "lorahome/transport.h"

namespace lorahome {

/**
 * The LoRa profile, in one place. Roadmap T1.3.
 *
 * These are not tuning knobs. Frequency, sync word and bandwidth have to match
 * bit for bit on both ends or there is simply silence on the air, and silence
 * is the single most expensive failure mode to debug — hours spent reading
 * software for what is a one-line radio mismatch (risk R1.6). Every one of them
 * is named and commented here so that the answer to "what is the node
 * configured for" is a file, not an archaeology exercise.
 */
struct LoraProfile {
  /** 868.1 MHz — the first of the three 868 MHz channels ETSI leaves at 1%. */
  static constexpr float kFrequencyMHz = 868.1f;
  /** SF9: the range/airtime compromise chosen for Etap 1. See the note below. */
  static constexpr uint8_t kSpreadingFactor = 9;
  static constexpr uint32_t kBandwidthHz = 125000u;
  /** RadioLib spells coding rate as the denominator: 5 == 4/5. */
  static constexpr uint8_t kCodingRate = 5;
  static constexpr uint16_t kPreambleSymbols = 8;

  /* RadioLib and the airtime formula want the same two settings in different
   * units. Derived rather than restated: a profile written down twice is a
   * profile that will eventually disagree with itself, and here the second
   * copy is what the duty cycle tracker spends. */
  static constexpr float kBandwidthKHz = static_cast<float>(kBandwidthHz) / 1000.0f;
  /** AN1200.22 numbers the coding rate 1..4 where RadioLib uses 5..8. */
  static constexpr uint8_t kAirtimeCodingRate = kCodingRate - 4;
  /** 0x12 — the private (non-LoRaWAN) sync word. A LoRaWAN network will not hear us. */
  static constexpr uint8_t kSyncWord = RADIOLIB_SX126X_SYNC_WORD_PRIVATE;
  /** 14 dBm = 25 mW, the ETSI ceiling for this band without duty-cycle relief. */
  static constexpr int8_t kOutputPowerDbm = 14;

  /** Conservative payload ceiling referenced throughout ARCHITECTURE.md. */
  static constexpr size_t kMaxPayload = 230;
};

/**
 * ITransport over an SX1262, shared by the Bridge and the Node.
 *
 * **Non-blocking by construction** (risk R1.3). RadioLib's `transmit()` and
 * `receive()` both spin until the radio is done, and at this profile a full
 * frame occupies the air for well over a second — during which a blocking
 * bridge hears nothing from the UART and its receive ring overflows. So
 * transmission is `startTransmit()` plus a DIO1 interrupt, reception is
 * `startReceive()` plus the same interrupt, and `poll()` does the finishing
 * work on the caller's thread where it is allowed to take its time.
 *
 * The interrupt handler sets one flag and returns. Everything else — reading
 * the packet, dispatching the callback, re-arming the receiver — happens in
 * `poll()`.
 *
 * Half duplex is not solved here: while transmitting, the radio is deaf, and a
 * frame arriving in that window is lost. That is accepted for Etap 1 and paid
 * for in Etap 2 with retransmissions and jitter (risk R1.4). It is a recorded
 * debt, not an oversight.
 */
class LoraTransport : public ITransport {
 public:
  explicit LoraTransport(SX1262* radio) : radio_(radio) {}

  /** Applies the profile above and arms the receiver. False if the radio did not come up. */
  bool begin();

  /**
   * Queues a frame for transmission. Returns false if the radio is busy or the
   * frame is too large — never blocks waiting for either.
   *
   * Note what this does *not* do: check the duty cycle. That decision belongs
   * to the caller, which has the tracker and the airtime figure; a transport
   * that silently refused would be indistinguishable from a broken radio.
   */
  bool send(const uint8_t* data, size_t len, uint16_t dstId) override;

  void onReceive(ReceiveCallback callback) override { receiveCallback_ = callback; }

  size_t getMTU() const override { return LoraProfile::kMaxPayload; }

  /** Call often from the main loop. Completes whatever the last interrupt started. */
  void poll();

  /** True between send() and the TxDone interrupt being serviced by poll(). */
  bool isTransmitting() const { return state_ == State::kTransmitting; }

  /** Signal quality of the most recent packet — the first thing to look at in the field. */
  float lastRssiDbm() const { return lastRssi_; }
  float lastSnrDb() const { return lastSnr_; }

  uint32_t txCount() const { return txCount_; }
  uint32_t rxCount() const { return rxCount_; }
  uint32_t txErrorCount() const { return txErrors_; }
  uint32_t rxErrorCount() const { return rxErrors_; }

 private:
  enum class State : uint8_t { kReceiving, kTransmitting };

  static void onDio1Interrupt();

  SX1262* radio_;
  ReceiveCallback receiveCallback_ = nullptr;
  State state_ = State::kReceiving;

  float lastRssi_ = 0.0f;
  float lastSnr_ = 0.0f;
  uint32_t txCount_ = 0;
  uint32_t rxCount_ = 0;
  uint32_t txErrors_ = 0;
  uint32_t rxErrors_ = 0;
};

}  // namespace lorahome
