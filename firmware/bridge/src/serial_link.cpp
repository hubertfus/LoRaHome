#include "serial_link.h"

namespace lorahome {
namespace {

/**
 * Driver-side receive buffer.
 *
 * Two buffers in series looks redundant until you name what each is for: this
 * one absorbs the interrupt latency between the FIFO filling and the reader
 * task being scheduled, while lh_ring_t absorbs the much longer stalls on the
 * *consumer* side — an NVS write, a flash erase, a 390 ms LoRa transmit. Sizing
 * this one at the ring's capacity keeps the two comparable, so `stat_hwm`
 * remains a meaningful reading rather than a measure of whichever buffer
 * happened to be smaller.
 */
constexpr int kDriverRxBuffer = LH_UART_RING_SIZE;

/**
 * How long the reader parks when the UART is quiet.
 *
 * Not zero: a spin would starve everything at lower priority on this core. Not
 * long either — this is also the worst-case delay before a byte already in the
 * driver's buffer reaches the ring, and it is charged directly to
 * `bench.e2e.latency` in T1.4.
 */
constexpr TickType_t kReadTimeout = pdMS_TO_TICKS(2);

/** One FIFO's worth per read; the ring's bulk push then costs one fence. */
constexpr size_t kReadChunk = 128;

}  // namespace

bool SerialLink::begin(uart_port_t port, int baud) {
  port_ = port;
  lh_ring_init(&ring_);

  const uart_config_t config = {
      .baud_rate = baud,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      // No flow control on purpose: SLIP recovers from a lost frame at the next
      // delimiter, and RTS/CTS would need two more wires on every bridge.
      // `stat_overrun` is what tells us if that trade stops being affordable.
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 0,
      .source_clk = UART_SCLK_APB,
  };

  if (uart_param_config(port_, &config) != ESP_OK) return false;

  // TX buffer 0 => uart_write_bytes blocks until the bytes are handed to the
  // FIFO. The transmit path is not the latency-critical one, and a blocking
  // write is one less buffer to reason about.
  if (uart_driver_install(port_, kDriverRxBuffer, /*tx_buffer_size=*/0, /*queue_size=*/0, nullptr,
                          /*intr_alloc_flags=*/0) != ESP_OK) {
    return false;
  }

  readerTask_ = xTaskCreateStatic(&SerialLink::readerTaskEntry, "lh_uart_rx", kReaderStackWords,
                                  this, kReaderPriority, readerStack_, &readerTcb_);
  if (readerTask_ == nullptr) {
    uart_driver_delete(port_);
    return false;
  }

  running_ = true;
  return true;
}

void SerialLink::readerTaskEntry(void* self) {
  static_cast<SerialLink*>(self)->readerLoop();
}

void SerialLink::readerLoop() {
  uint8_t chunk[kReadChunk];

  for (;;) {
    const int received =
        uart_read_bytes(port_, chunk, sizeof(chunk), kReadTimeout);
    if (received <= 0) continue;

    // The only thing that happens between the wire and the ring. If a byte is
    // dropped here it is because the consumer is behind, and lh_ring_push_bytes
    // has already counted it — see risk R1.2.
    lh_ring_push_bytes(&ring_, chunk, static_cast<uint16_t>(received));
  }
}

uint16_t SerialLink::read(uint8_t* dst, uint16_t cap) {
  return running_ ? lh_ring_pop_bytes(&ring_, dst, cap) : 0;
}

bool SerialLink::readByte(uint8_t* out) {
  return running_ && lh_ring_pop(&ring_, out);
}

size_t SerialLink::write(const uint8_t* src, size_t len) {
  if (!running_) return 0;
  const int written = uart_write_bytes(port_, reinterpret_cast<const char*>(src), len);
  return written < 0 ? 0 : static_cast<size_t>(written);
}

uint8_t SerialLink::ringHighWaterPct() const {
  return static_cast<uint8_t>((100u * ring_.stat_hwm) / LH_UART_RING_SIZE);
}

}  // namespace lorahome
