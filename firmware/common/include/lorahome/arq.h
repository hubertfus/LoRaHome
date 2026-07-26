#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lorahome/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Stop-and-wait ARQ with exponential backoff and jitter. Roadmap T2.4.
 *
 * Stop-and-wait rather than a sliding window, and not because it is easier:
 * one frame at SF9 is 1147.9 ms on air, and the ETSI duty cycle permits roughly
 * one such frame every 115 s. There is never a second frame in flight to
 * manage, so a window would be state kept for a situation that cannot arise —
 * about ten times the bookkeeping for no delivery it could have improved.
 *
 * The jitter is not a refinement. Without it, every node that lost its link at
 * the same moment — a power cut, a gateway reboot — retransmits at the same
 * moment, collides, backs off by the same doubling, and collides again. The
 * retransmission storm sustains itself and the network stays down after the
 * cause is gone. Risk R2.2; the jitter is a requirement, and its distribution
 * is measured in CI rather than assumed.
 *
 * The duty cycle hook is here from the start (R2.5). Five retransmissions of a
 * 230 B frame is close to two seconds of airtime, which is a meaningful slice
 * of an hourly budget that is law rather than policy. When the budget is spent
 * the retry is *deferred*, never abandoned — a config that arrives late is a
 * config; one dropped because the radio was busy is a support call.
 */

#define LH_ARQ_MAX_RETRIES 5u

/**
 * First retransmission timeout, in ms.
 *
 * Derived, not chosen. A 230 B frame at SF9/BW125/CR4-5 is 1147.9 ms on air
 * (ARCHITECTURE.md §7.3), so a frame and its acknowledgement cannot complete a
 * round trip in less than 2295.8 ms even on a perfect link with an instant
 * peer. The roadmap's 2000 ms predates that correction — it was sized against
 * the 390 ms figure that turned out to be SF7 — and it guarantees that the
 * timeout fires while the ACK is still in the air. Measured: 0.6 spurious
 * retransmissions per frame on a *clean* link, each one 1147.9 ms of airtime
 * against a 1% duty cycle.
 *
 * 4000 ms is that floor plus ~74% headroom for the peer's turnaround and a
 * frame arriving at the far end of the jitter window. The schedule that follows
 * is 4, 8, 16, 32, 64 s.
 */
#define LH_ARQ_BASE_TIMEOUT_MS 4000u

#define LH_ARQ_JITTER_MS 500u

typedef enum {
  LH_ARQ_IDLE = 0,     /* nothing in flight                              */
  LH_ARQ_WAIT_ACK = 1, /* sent; waiting for an ACK or the timeout        */
  LH_ARQ_DONE = 2,     /* acknowledged; the slot is free                 */
  LH_ARQ_FAILED = 3,   /* gave up after LH_ARQ_MAX_RETRIES               */
} lh_arq_state_t;

/** What the caller should do as a result of a tick. */
typedef enum {
  LH_ARQ_NOTHING = 0,     /* still waiting, or nothing in flight          */
  LH_ARQ_RETRANSMIT = 1,  /* send *frame_out again, now                   */
  LH_ARQ_GAVE_UP = 2,     /* out of retries; state is FAILED              */
  LH_ARQ_DEFERRED = 3,    /* retry due, but the duty cycle said not yet   */
} lh_arq_action_t;

/**
 * Source of jitter.
 *
 * A callback rather than a call to esp_random(), because firmware/common has no
 * platform — and because a jitter source that cannot be controlled cannot be
 * tested, and an untested jitter distribution is how R2.2 gets into the field.
 * On the ESP32 pass a wrapper around esp_random(). Do NOT pass a fixed-seed
 * generator on a real device: nodes that agree on their "random" delays are
 * nodes with no jitter at all.
 */
typedef uint32_t (*lh_arq_rand_fn)(void *user);

/**
 * Asks whether `len` bytes may go on air right now. NULL means always.
 *
 * Same shape as the Bridge's guard, and for the same reason: answering it needs
 * a clock and a rolling window, neither of which belongs in this file. Full
 * enforcement arrives in Etap 7; the hook is here now so the ARQ never has to
 * learn about the duty cycle later, which is the change nobody makes safely
 * once retransmission timing is load-bearing.
 */
typedef bool (*lh_arq_allow_tx_fn)(void *user, uint16_t len);

typedef struct {
  lh_arq_state_t state;
  uint8_t seq;         /* sequence number of the frame in flight        */
  uint8_t retry_count; /* retransmissions so far, 0..LH_ARQ_MAX_RETRIES */
  uint16_t pending_len;
  int64_t next_retry_us;
  int64_t sent_at_us; /* for the RTT average                           */

  lh_arq_rand_fn rand_fn;
  void *rand_user;
  lh_arq_allow_tx_fn allow_tx;
  void *allow_tx_user;

  uint32_t stat_sent;
  uint32_t stat_retries;
  uint32_t stat_acked;
  uint32_t stat_giveups;
  uint32_t stat_deferred;
  uint32_t stat_stray_acks; /* ACKs for something not in flight        */
  uint32_t stat_rtt_sum_ms;
  uint32_t stat_rtt_count;

  /* One frame in flight, so one buffer, statically. The alternative — a
   * pointer to the caller's buffer — would make every caller responsible for
   * keeping it alive across five retransmissions and thirty seconds. */
  uint8_t pending[LORAHOME_MAX_FRAME_SIZE];
} lh_arq_t;

/** Clears state and counters. `rand_fn` is required; see the note above. */
void lh_arq_init(lh_arq_t *arq, lh_arq_rand_fn rand_fn, void *rand_user);

/** Optional duty-cycle veto over retransmissions (R2.5). */
void lh_arq_set_duty_cycle_guard(lh_arq_t *arq, lh_arq_allow_tx_fn allow_tx, void *user);

/**
 * Backoff for retransmission number `retry`: BASE * 2^retry + jitter.
 *
 * Exposed so the distribution can be measured directly rather than inferred
 * from timing traces.
 */
uint32_t lh_arq_timeout_ms(const lh_arq_t *arq, uint8_t retry);

/**
 * Takes a frame for reliable delivery. The caller transmits it; this remembers
 * it and arms the timeout.
 *
 * Returns false if something is already in flight or the frame is too long —
 * stop-and-wait means exactly one, and a caller that ignores the answer would
 * silently replace a config that had not been acknowledged.
 */
bool lh_arq_send(lh_arq_t *arq, uint8_t seq, const uint8_t *frame, uint16_t len, int64_t now_us);

/**
 * Reports an ACK.
 *
 * Returns true only for the ACK that completes the frame in flight. A duplicate
 * ACK — the original was slow, not lost, so a retransmission produced a second
 * one — returns false and changes nothing. That idempotence is what stops a
 * late ACK from acknowledging the *next* config by accident.
 */
bool lh_arq_on_ack(lh_arq_t *arq, uint8_t seq, int64_t now_us);

/**
 * Advances the timer.
 *
 * On LH_ARQ_RETRANSMIT, `*frame_out` and `*len_out` describe the frame to send
 * again; both may be NULL if the caller only wants the decision. Deferred
 * retries are rescheduled a short way out rather than dropped, so a busy duty
 * cycle costs latency and never delivery.
 */
lh_arq_action_t lh_arq_tick(lh_arq_t *arq, int64_t now_us, const uint8_t **frame_out,
                            uint16_t *len_out);

/** Abandons whatever is in flight and returns to IDLE. Counters are kept. */
void lh_arq_reset(lh_arq_t *arq);

/** Mean round-trip time over acknowledged frames, in ms. 0 if none yet. */
uint32_t lh_arq_mean_rtt_ms(const lh_arq_t *arq);

#ifdef __cplusplus
}
#endif
