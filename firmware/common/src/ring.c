#include "lorahome/ring.h"

#include <string.h>

/*
 * Layout and arithmetic invariants.
 *
 * The wraparound assertion is the one that would otherwise bite silently. head
 * and tail are uint16_t free-running counters, so they wrap at 65536. As long
 * as the ring size divides 65536 exactly, that wrap is invisible: the masked
 * index continues in sequence across it. Pick a size that does not divide
 * evenly — 1500, say, for an MTU-shaped buffer — and the ring quietly corrupts
 * one buffer's worth of data every 65536 bytes, which at 921600 baud is roughly
 * once every 0.7 seconds and looks exactly like line noise.
 */
_Static_assert((LH_UART_RING_SIZE & LH_UART_RING_MASK) == 0u,
               "ring size must be a power of two for the mask to work");
_Static_assert(65536u % LH_UART_RING_SIZE == 0u,
               "ring size must divide the uint16_t counter range, or wraparound corrupts data");
_Static_assert(LH_UART_RING_SIZE <= 32768u,
               "occupancy must stay representable as a uint16_t difference");
_Static_assert(sizeof(lh_ring_t) <= LH_UART_RING_SIZE + 16u, "ring overhead budget breach");

/*
 * Ordering between the ISR and the task.
 *
 * `volatile` stops the compiler caching head and tail in registers, but it says
 * nothing about the order of the *data* stores relative to the head store —
 * data[] is not volatile, and making it so would cost the bulk copy dearly. So
 * a release fence sits between filling the slots and publishing them, and an
 * acquire fence between reading the published index and touching the slots.
 * Without the release fence the consumer can legally observe an advanced head
 * over a slot that has not been written yet, and the resulting corruption is
 * both rare and load-dependent — the worst kind to debug in the field.
 */
#if defined(__GNUC__) || defined(__clang__)
#define LH_FENCE_RELEASE() __atomic_thread_fence(__ATOMIC_RELEASE)
#define LH_FENCE_ACQUIRE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#elif defined(_MSC_VER)
#include <intrin.h>
/* The host build is a correctness and throughput harness on x86-64, which is
 * strongly ordered; a compiler fence is the part that actually matters here. */
#define LH_FENCE_RELEASE() _ReadWriteBarrier()
#define LH_FENCE_ACQUIRE() _ReadWriteBarrier()
#else
#error "no memory fence available for this compiler — the ring would be unsafe"
#endif

void lh_ring_init(lh_ring_t *r) {
  r->head = 0;
  r->tail = 0;
  r->stat_overrun = 0;
  r->stat_hwm = 0;
  /* data[] is deliberately not cleared: every byte is written before it can be
   * read, and zeroing 2 kB on every init would be pure ceremony. */
}

uint16_t lh_ring_count(const lh_ring_t *r) {
  /* Modular subtraction — correct across the uint16_t wrap, given the size
   * assertion above. */
  return (uint16_t)(r->head - r->tail);
}

uint16_t lh_ring_free_space(const lh_ring_t *r) {
  return (uint16_t)(LH_UART_RING_SIZE - lh_ring_count(r));
}

/** Producer-side bookkeeping after `count` bytes have been published. */
static void note_occupancy(lh_ring_t *r) {
  const uint16_t used = lh_ring_count(r);
  /* Written by the producer only, so no interlock is needed. */
  if (used > r->stat_hwm) r->stat_hwm = used;
}

bool lh_ring_push(lh_ring_t *r, uint8_t byte) {
  if (lh_ring_count(r) >= LH_UART_RING_SIZE) {
    r->stat_overrun++;
    return false;
  }

  r->data[r->head & LH_UART_RING_MASK] = byte;
  LH_FENCE_RELEASE();
  r->head = (uint16_t)(r->head + 1u);

  note_occupancy(r);
  return true;
}

uint16_t lh_ring_push_bytes(lh_ring_t *r, const uint8_t *src, uint16_t len) {
  const uint16_t space = lh_ring_free_space(r);
  const uint16_t accepted = len > space ? space : len;

  if (accepted < len) r->stat_overrun += (uint32_t)(len - accepted);
  if (accepted == 0u) return 0u;

  const uint16_t start = (uint16_t)(r->head & LH_UART_RING_MASK);
  /* At most two memcpys: the run up to the end of the buffer, then the
   * remainder from the start. Byte-at-a-time would be simpler and would also
   * be the thing running inside the interrupt. */
  const uint16_t first = (uint16_t)(LH_UART_RING_SIZE - start) < accepted
                             ? (uint16_t)(LH_UART_RING_SIZE - start)
                             : accepted;
  memcpy(&r->data[start], src, first);
  if (accepted > first) memcpy(&r->data[0], src + first, (size_t)(accepted - first));

  LH_FENCE_RELEASE();
  r->head = (uint16_t)(r->head + accepted);

  note_occupancy(r);
  return accepted;
}

bool lh_ring_pop(lh_ring_t *r, uint8_t *out) {
  if (lh_ring_count(r) == 0u) return false;

  LH_FENCE_ACQUIRE();
  *out = r->data[r->tail & LH_UART_RING_MASK];
  r->tail = (uint16_t)(r->tail + 1u);
  return true;
}

uint16_t lh_ring_pop_bytes(lh_ring_t *r, uint8_t *dst, uint16_t cap) {
  const uint16_t available = lh_ring_count(r);
  const uint16_t taken = cap < available ? cap : available;
  if (taken == 0u) return 0u;

  LH_FENCE_ACQUIRE();

  const uint16_t start = (uint16_t)(r->tail & LH_UART_RING_MASK);
  const uint16_t first =
      (uint16_t)(LH_UART_RING_SIZE - start) < taken ? (uint16_t)(LH_UART_RING_SIZE - start) : taken;
  memcpy(dst, &r->data[start], first);
  if (taken > first) memcpy(dst + first, &r->data[0], (size_t)(taken - first));

  r->tail = (uint16_t)(r->tail + taken);
  return taken;
}
