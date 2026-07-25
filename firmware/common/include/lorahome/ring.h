#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Single-producer / single-consumer byte ring for the UART receive path.
 * Roadmap T1.2.
 *
 * The producer is the UART interrupt; the consumer is the serial task. Risk
 * R1.2 is the whole reason this type exists: the ISR must do nothing but move
 * bytes. No SLIP decoding, no CRC, no logging — at 921600 baud a byte arrives
 * every 10.9 us, and an ISR that does real work drops data or trips the
 * watchdog. The ring is the handover point, and it is the only thing the ISR
 * touches.
 *
 * Lives in `common` rather than in the bridge because the node needs the same
 * structure, and because the interesting properties — index wraparound, overrun
 * accounting, the high-water mark — are pure logic that can be tested on a host
 * without a board on the desk. The bridge-specific half (UART driver, task,
 * pinning) is firmware/bridge/src/serial_link.
 *
 * Storage is inline and static. Nothing here allocates, ever.
 */

#define LH_UART_RING_SIZE 2048u
#define LH_UART_RING_MASK (LH_UART_RING_SIZE - 1u)

/**
 * Ring state.
 *
 * `head` and `tail` are **free-running** counters, masked only when indexing
 * `data`. They are not wrapped to the buffer size, and that is deliberate: with
 * wrapped indices, head == tail means both "empty" and "full", and every
 * implementation that goes down that road ends up either wasting a slot or
 * carrying a separate `count` that both sides write — which is a race. Free
 * running, occupancy is simply `head - tail` in modular arithmetic, each index
 * is written by exactly one side, and there is nothing to race over.
 *
 * `stat_hwm` is the early-warning metric, not a curiosity. If peak occupancy at
 * 921600 baud sits above ~60% of capacity, the consumer is already too slow and
 * the first hiccup — an NVS write, a flash erase — will cost data. That is
 * knowable before it happens, but only if somebody records it.
 */
typedef struct {
  uint8_t data[LH_UART_RING_SIZE];
  volatile uint16_t head; /* written by the producer (ISR) only */
  volatile uint16_t tail; /* written by the consumer (task) only */
  uint32_t stat_overrun;  /* bytes dropped because the ring was full */
  uint16_t stat_hwm;      /* peak occupancy in bytes — see above */
} lh_ring_t;

/** Clears the ring and its counters. Not safe to call while either side runs. */
void lh_ring_init(lh_ring_t *r);

/** Bytes currently waiting. Safe to call from either side. */
uint16_t lh_ring_count(const lh_ring_t *r);

/** Space remaining, in bytes. */
uint16_t lh_ring_free_space(const lh_ring_t *r);

/**
 * Producer: appends one byte. Returns false if the ring was full, in which case
 * the byte is dropped and `stat_overrun` is incremented.
 *
 * Drop-newest, not drop-oldest. Both lose data, but dropping the incoming byte
 * damages only the frame currently arriving — SLIP resynchronises at the next
 * delimiter and one frame is lost. Dropping the oldest would corrupt a frame
 * that had already arrived intact, turning one lost frame into two.
 */
bool lh_ring_push(lh_ring_t *r, uint8_t byte);

/**
 * Producer: appends up to `len` bytes, returning how many were accepted.
 *
 * The bulk form exists because a UART hands over a FIFO-full at a time, and
 * paying the fence cost per byte inside an ISR is exactly the overhead R1.2
 * warns about. One fence per batch, not per byte.
 */
uint16_t lh_ring_push_bytes(lh_ring_t *r, const uint8_t *src, uint16_t len);

/** Consumer: removes one byte. Returns false if the ring was empty. */
bool lh_ring_pop(lh_ring_t *r, uint8_t *out);

/** Consumer: removes up to `cap` bytes, returning how many were copied. */
uint16_t lh_ring_pop_bytes(lh_ring_t *r, uint8_t *dst, uint16_t cap);

#ifdef __cplusplus
}
#endif
