#include "lorahome/arq.h"

#include <string.h>

/*
 * RAM budget, and a deviation from the roadmap's 288 B that is worth naming.
 *
 * That figure was written for a struct with the frame buffer, the timer and
 * four counters. This one also carries two callbacks — the jitter source (R2.2
 * requires jitter that is genuinely random on a device and controllable in a
 * test) and the duty cycle guard (R2.5 requires the hook to exist now) — plus
 * the send timestamp behind the mean-RTT metric the same roadmap section asks
 * for. On a 64-bit host that is 318 B; on the ESP32's 32-bit ABI, 302 B.
 *
 * The constraint that actually matters is unaffected and still asserted, in
 * reliability.h: dedup + reassembler + ARQ under 2304 B. At 152 + 1640 + 320
 * the whole reliability layer is 2112 B, with 192 B to spare.
 */
_Static_assert(sizeof(lh_arq_t) <= 384, "ARQ RAM budget breach — see roadmap Etap 2");

/** How long a duty-cycle-deferred retry waits before asking again. */
#define LH_ARQ_DEFER_RETRY_MS 250u

void lh_arq_init(lh_arq_t *arq, lh_arq_rand_fn rand_fn, void *rand_user) {
  memset(arq, 0, sizeof *arq);
  arq->state = LH_ARQ_IDLE;
  arq->rand_fn = rand_fn;
  arq->rand_user = rand_user;
}

void lh_arq_set_duty_cycle_guard(lh_arq_t *arq, lh_arq_allow_tx_fn allow_tx, void *user) {
  arq->allow_tx = allow_tx;
  arq->allow_tx_user = user;
}

uint32_t lh_arq_timeout_ms(const lh_arq_t *arq, uint8_t retry) {
  const uint8_t capped = retry > LH_ARQ_MAX_RETRIES ? (uint8_t)LH_ARQ_MAX_RETRIES : retry;
  const uint32_t base = LH_ARQ_BASE_TIMEOUT_MS << capped;

  /* No source, no jitter — and no pretending otherwise. A caller that forgot
   * the generator gets deterministic backoff, which is wrong in the field and
   * visible in a test, rather than a plausible-looking constant. */
  if (arq->rand_fn == NULL) return base;

  return base + (arq->rand_fn(arq->rand_user) % LH_ARQ_JITTER_MS);
}

void lh_arq_reset(lh_arq_t *arq) {
  arq->state = LH_ARQ_IDLE;
  arq->seq = 0;
  arq->retry_count = 0;
  arq->pending_len = 0;
  arq->next_retry_us = 0;
  arq->sent_at_us = 0;
}

bool lh_arq_send(lh_arq_t *arq, uint8_t seq, const uint8_t *frame, uint16_t len, int64_t now_us) {
  if (arq->state == LH_ARQ_WAIT_ACK) return false;
  if (len == 0 || len > LORAHOME_MAX_FRAME_SIZE) return false;

  memcpy(arq->pending, frame, len);
  arq->pending_len = len;
  arq->seq = seq;
  arq->retry_count = 0;
  arq->state = LH_ARQ_WAIT_ACK;
  arq->sent_at_us = now_us;
  arq->next_retry_us = now_us + (int64_t)lh_arq_timeout_ms(arq, 0) * 1000;
  arq->stat_sent++;
  return true;
}

bool lh_arq_on_ack(lh_arq_t *arq, uint8_t seq, int64_t now_us) {
  /* A late ACK for a frame that is already done, or for one that gave up, or
   * for a sequence number we never sent. All three are the same answer: nothing
   * happens. The dangerous version of this function is one that would let a
   * straggler acknowledge whatever happens to be in flight now. */
  if (arq->state != LH_ARQ_WAIT_ACK || seq != arq->seq) {
    arq->stat_stray_acks++;
    return false;
  }

  const int64_t rtt_us = now_us - arq->sent_at_us;
  if (rtt_us > 0) {
    arq->stat_rtt_sum_ms += (uint32_t)(rtt_us / 1000);
    arq->stat_rtt_count++;
  }

  arq->stat_acked++;
  arq->state = LH_ARQ_DONE;
  arq->pending_len = 0;
  return true;
}

lh_arq_action_t lh_arq_tick(lh_arq_t *arq, int64_t now_us, const uint8_t **frame_out,
                            uint16_t *len_out) {
  if (arq->state != LH_ARQ_WAIT_ACK) return LH_ARQ_NOTHING;
  if (now_us < arq->next_retry_us) return LH_ARQ_NOTHING;

  if (arq->retry_count >= LH_ARQ_MAX_RETRIES) {
    arq->state = LH_ARQ_FAILED;
    arq->stat_giveups++;
    return LH_ARQ_GAVE_UP;
  }

  /* R2.5: the duty cycle can postpone a retry, never cancel one. Asked before
   * the retry counter moves, so a deferral does not consume one of the five
   * attempts — the frame was not transmitted, and counting it as an attempt
   * would let a busy hour exhaust the retries without a single frame on air. */
  if (arq->allow_tx != NULL && !arq->allow_tx(arq->allow_tx_user, arq->pending_len)) {
    arq->next_retry_us = now_us + (int64_t)LH_ARQ_DEFER_RETRY_MS * 1000;
    arq->stat_deferred++;
    return LH_ARQ_DEFERRED;
  }

  arq->retry_count++;
  arq->stat_retries++;
  arq->next_retry_us = now_us + (int64_t)lh_arq_timeout_ms(arq, arq->retry_count) * 1000;

  if (frame_out != NULL) *frame_out = arq->pending;
  if (len_out != NULL) *len_out = arq->pending_len;
  return LH_ARQ_RETRANSMIT;
}

uint32_t lh_arq_mean_rtt_ms(const lh_arq_t *arq) {
  if (arq->stat_rtt_count == 0) return 0;
  return arq->stat_rtt_sum_ms / arq->stat_rtt_count;
}
