#include "lorahome/lora_transport.h"

namespace lorahome {
namespace {

/**
 * The interrupt handler needs a way back to the object, and an ISR cannot carry
 * a `this`. One radio per board makes a file-scope pointer the honest answer;
 * anything cleverer would be indirection for a case that does not exist. If a
 * second radio ever appears, this is the line that has to change, and it will
 * fail loudly rather than silently servicing the wrong object.
 */
LoraTransport* g_instance = nullptr;

/**
 * Set by DIO1, cleared by poll().
 *
 * The handler does nothing else. On the SX1262 the same pin signals TxDone and
 * RxDone, so which one it was is inferred from the state the driver was in —
 * reading the IRQ status register from inside an interrupt would mean an SPI
 * transaction there, which is exactly the kind of work R1.2 and R1.3 both say
 * does not belong on this path.
 */
volatile bool g_irqPending = false;

#if defined(ARDUINO_ARCH_ESP32) || defined(ESP32)
#define LH_ISR_ATTR IRAM_ATTR
#else
#define LH_ISR_ATTR
#endif

}  // namespace

void LH_ISR_ATTR LoraTransport::onDio1Interrupt() { g_irqPending = true; }

bool LoraTransport::begin() {
  g_instance = this;

  const int16_t state = radio_->begin(
      LoraProfile::kFrequencyMHz, LoraProfile::kBandwidthKHz, LoraProfile::kSpreadingFactor,
      LoraProfile::kCodingRate, LoraProfile::kSyncWord, LoraProfile::kOutputPowerDbm,
      LoraProfile::kPreambleSymbols);
  if (state != RADIOLIB_ERR_NONE) return false;

  radio_->setDio1Action(&LoraTransport::onDio1Interrupt);

  // Armed immediately and re-armed after every event: a receiver that is only
  // listening when somebody remembered to ask is a receiver that misses the
  // beacon it was waiting for.
  if (radio_->startReceive() != RADIOLIB_ERR_NONE) return false;

  state_ = State::kReceiving;
  g_irqPending = false;
  return true;
}

bool LoraTransport::send(const uint8_t* data, size_t len, uint16_t /*dstId*/) {
  if (len > getMTU()) return false;

  // Half duplex: refusing while a transmission is in flight is the honest
  // answer. Queueing here would hide airtime from the duty cycle tracker, which
  // is the one accounting error in this system with legal consequences.
  if (state_ == State::kTransmitting) return false;

  const int16_t status = radio_->startTransmit(const_cast<uint8_t*>(data), len);
  if (status != RADIOLIB_ERR_NONE) {
    txErrors_++;
    radio_->startReceive();
    return false;
  }

  state_ = State::kTransmitting;
  return true;
}

void LoraTransport::poll() {
  if (!g_irqPending) return;
  g_irqPending = false;

  if (state_ == State::kTransmitting) {
    radio_->finishTransmit();
    txCount_++;
    state_ = State::kReceiving;
    radio_->startReceive();
    return;
  }

  uint8_t buffer[LoraProfile::kMaxPayload];
  const size_t length = radio_->getPacketLength();
  if (length == 0 || length > sizeof(buffer)) {
    rxErrors_++;
    radio_->startReceive();
    return;
  }

  const int16_t status = radio_->readData(buffer, length);
  if (status != RADIOLIB_ERR_NONE) {
    rxErrors_++;
    radio_->startReceive();
    return;
  }

  // Captured before re-arming: startReceive() resets the packet status, and
  // these two numbers are the whole story when a link works on the bench and
  // not in the field (risk R1.6).
  lastRssi_ = radio_->getRSSI();
  lastSnr_ = radio_->getSNR();

  lorahome_header_t header;
  if (lorahome_decode_header(buffer, length, &header)) {
    rxCount_++;
    if (receiveCallback_ != nullptr) receiveCallback_(buffer, length, header.src_id);
  } else {
    rxErrors_++;
  }

  // Re-armed last, so the callback above runs with the radio quiet rather than
  // racing a packet that arrives while it is still working.
  radio_->startReceive();
}

}  // namespace lorahome
