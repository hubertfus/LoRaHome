#pragma once

#include <cstddef>
#include <cstdint>

namespace lorahome {

/**
 * Transport abstraction — see ARCHITECTURE.md §2. Nothing above this
 * interface (frame parsing, rule evaluation, config store) may know that
 * LoRa/SX1262 is the concrete transport underneath. Swapping to ESP-NOW
 * later should mean writing one new implementation of this interface, not
 * touching any calling code.
 */
class ITransport {
 public:
  using ReceiveCallback = void (*)(const uint8_t* data, size_t len, uint16_t srcId);

  virtual ~ITransport() = default;

  virtual bool send(const uint8_t* data, size_t len, uint16_t dstId) = 0;
  virtual void onReceive(ReceiveCallback callback) = 0;
  virtual size_t getMTU() const = 0;
};

}  // namespace lorahome
