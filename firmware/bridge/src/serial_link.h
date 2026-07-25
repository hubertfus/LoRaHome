#pragma once

#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>

#include "lorahome/ring.h"

namespace lorahome {

/**
 * The Bridge's serial link to the Host. Roadmap T1.2.
 *
 * Structure follows risk R1.2: the fast path does nothing but move bytes. A
 * dedicated reader task drains the UART into a static ring and returns
 * immediately; SLIP decoding, CRC and frame dispatch all happen on the consumer
 * side, out of the way. At 921600 baud a byte lands every 10.9 us, and any
 * real work on the receive path either drops data or trips the watchdog.
 *
 * **Deviation from the roadmap sketch, stated plainly.** T1.2 describes the
 * producer as the UART ISR. This uses a high-priority reader task sitting on
 * top of the ESP-IDF driver's ISR instead. Installing a bare `uart_isr_register`
 * handler means taking the port away from the driver that the Arduino framework
 * and the rest of the stack already use, and hand-rolling FIFO and error
 * handling that the driver does correctly — for no gain the ring does not
 * already provide, since the driver's own ISR is already the "do nothing but
 * move bytes" layer. The property R1.2 actually asks for is preserved: nothing
 * between the wire and the ring does any protocol work.
 *
 * Allocation: none after `begin()`. The task uses a static stack and a static
 * TCB (`xTaskCreateStatic`), and the ring is a member, so the steady-state
 * heap delta is zero. `uart_driver_install()` does allocate, once, during
 * `begin()` — which §0.6 permits, since the budget is zero `malloc` *after*
 * `app_main`, not zero ever.
 */
class SerialLink {
 public:
  /** Roadmap T1.2. High enough to matter, low enough that the FIFO is the limit. */
  static constexpr int kDefaultBaud = 921600;

  /**
   * Stack for the reader task.
   *
   * The T1.2 budget is a 2048 B high-water mark. Allocated at 3072 so that the
   * budget is a measurement to pass rather than a wall to hit: a task sized
   * exactly at its budget overflows before it can report that it was close.
   * The HWM itself needs `uxTaskGetStackHighWaterMark` on real hardware.
   */
  static constexpr uint32_t kReaderStackWords = 3072 / sizeof(StackType_t);

  /** Above the application, below the drivers. */
  static constexpr UBaseType_t kReaderPriority = 12;

  /**
   * Starts the driver and the reader task. Returns false if either fails; the
   * caller has no fallback, but it should say so rather than run deaf.
   */
  bool begin(uart_port_t port = UART_NUM_0, int baud = kDefaultBaud);

  /** Consumer: copies up to `cap` bytes out of the ring. Never blocks. */
  uint16_t read(uint8_t* dst, uint16_t cap);

  /** Consumer: pops a single byte. Returns false when the ring is empty. */
  bool readByte(uint8_t* out);

  /** Transmits synchronously. The TX path has no ring — it is not the fast one. */
  size_t write(const uint8_t* src, size_t len);

  /** Peak ring occupancy as a percentage of capacity — the R1.2 warning light. */
  uint8_t ringHighWaterPct() const;

  /** Bytes lost because the consumer could not keep up. Should stay at zero. */
  uint32_t overrunCount() const { return ring_.stat_overrun; }

  const lh_ring_t& ring() const { return ring_; }

 private:
  static void readerTaskEntry(void* self);
  void readerLoop();

  lh_ring_t ring_{};
  uart_port_t port_ = UART_NUM_0;
  bool running_ = false;

  StaticTask_t readerTcb_{};
  StackType_t readerStack_[kReaderStackWords]{};
  TaskHandle_t readerTask_ = nullptr;
};

}  // namespace lorahome
