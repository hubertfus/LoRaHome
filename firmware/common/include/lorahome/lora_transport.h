#pragma once

#include <RadioLib.h>
#include "lorahome/protocol.h"
#include "lorahome/transport.h"

namespace lorahome {

/**
 * ITransport implementation over an SX1262, shared by the Bridge and the
 * Node (both carry the same radio). Concrete RadioLib wiring is the only
 * thing this class knows about — callers only ever see ITransport. See
 * ARCHITECTURE.md §2.
 */
class LoraTransport : public ITransport {
 public:
  explicit LoraTransport(SX1262* radio) : radio_(radio) {}

  bool begin(float frequencyMHz = 868.0f, float bandwidthKHz = 125.0f, uint8_t spreadingFactor = 9,
             uint8_t codingRate = 7, int8_t outputPowerDbm = 14) {
    const int state = radio_->begin(frequencyMHz, bandwidthKHz, spreadingFactor, codingRate, RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                                     outputPowerDbm);
    return state == RADIOLIB_ERR_NONE;
  }

  bool send(const uint8_t* data, size_t len, uint16_t /*dstId*/) override {
    if (len > getMTU()) return false;
    return radio_->transmit(const_cast<uint8_t*>(data), len) == RADIOLIB_ERR_NONE;
  }

  void onReceive(ReceiveCallback callback) override { receiveCallback_ = callback; }

  size_t getMTU() const override { return kMaxLoraPayload; }

  /** Call frequently from the main loop; dispatches to the receive callback when a packet arrives. */
  void poll() {
    uint8_t buf[kMaxLoraPayload];
    const int state = radio_->receive(buf, sizeof(buf));
    if (state != RADIOLIB_ERR_NONE) return;

    const size_t len = radio_->getPacketLength();
    lorahome_header_t header;
    if (!lorahome_decode_header(buf, len, &header)) return;

    if (receiveCallback_) receiveCallback_(buf, len, header.src_id);
  }

 private:
  // Conservative LoRa payload ceiling referenced throughout ARCHITECTURE.md (~230B at typical SF/BW).
  static constexpr size_t kMaxLoraPayload = 230;

  SX1262* radio_;
  ReceiveCallback receiveCallback_ = nullptr;
};

}  // namespace lorahome
