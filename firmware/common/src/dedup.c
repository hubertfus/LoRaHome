#include "lorahome/dedup.h"

#include <string.h>

/**
 * Budget from the roadmap's Etap 2 data architecture: the whole reliability
 * context is 2304 B, of which dedup owns 256. Asserted here so that adding a
 * field to the peer record is a build failure and a conversation, not a silent
 * 200 bytes off some other component's budget.
 */
_Static_assert(sizeof(lh_dedup_t) <= 256, "dedup RAM budget breach — see roadmap Etap 2");

void lh_dedup_init(lh_dedup_t *dedup) { memset(dedup, 0, sizeof *dedup); }

/**
 * Modular distance between two sequence numbers.
 *
 * Positive: `seq` is newer than `reference`. Negative: older. The cast to
 * int8_t is what makes 0x00 newer than 0xFF rather than 255 frames older —
 * risk R2.1, and the reason no comparison in this file uses `<` or `>` on raw
 * sequence numbers.
 *
 * The window is 32 and the sequence space is 256, so "newer" is unambiguous for
 * any gap under 128. A gap over 128 is indistinguishable from a large step
 * backwards, and is treated as a step forwards — which is the right guess: a
 * peer that has been silent long enough to skip 128 sequence numbers is far
 * more likely to have kept transmitting out of range than to have travelled
 * back in time.
 */
static int8_t seq_delta(uint8_t seq, uint8_t reference) {
  return (int8_t)((uint8_t)(seq - reference));
}

static lh_dedup_peer_t *find_peer(lh_dedup_t *dedup, uint16_t src_id) {
  for (uint8_t i = 0; i < dedup->peer_count; i++) {
    if (dedup->peers[i].src_id == src_id) return &dedup->peers[i];
  }
  return NULL;
}

const lh_dedup_peer_t *lh_dedup_find_peer(const lh_dedup_t *dedup, uint16_t src_id) {
  for (uint8_t i = 0; i < dedup->peer_count; i++) {
    if (dedup->peers[i].src_id == src_id) return &dedup->peers[i];
  }
  return NULL;
}

/**
 * Claims a slot for a sender we have not heard from before.
 *
 * With a free slot this is trivial. Full, it evicts the peer heard from longest
 * ago — LRU rather than, say, refusing the newcomer, because a node that has
 * gone off the air must not be able to lock out a node that is on it. The
 * eviction is counted: losing a peer's history means its next frame is
 * unconditionally new, so `stat_peer_evicted` is the exact point past which the
 * no-double-processing guarantee is only best effort.
 */
static lh_dedup_peer_t *claim_peer(lh_dedup_t *dedup, uint16_t src_id, int64_t now_us) {
  lh_dedup_peer_t *slot;

  if (dedup->peer_count < LH_DEDUP_PEERS) {
    slot = &dedup->peers[dedup->peer_count++];
  } else {
    slot = &dedup->peers[0];
    for (uint8_t i = 1; i < LH_DEDUP_PEERS; i++) {
      if (dedup->peers[i].last_seen_us < slot->last_seen_us) slot = &dedup->peers[i];
    }
    dedup->stat_peer_evicted++;
  }

  slot->src_id = src_id;
  slot->last_seq = 0;
  slot->bitmap = 0;
  slot->last_seen_us = now_us;
  return slot;
}

bool lh_dedup_check_and_mark(lh_dedup_t *dedup, uint16_t src_id, uint8_t seq, int64_t now_us) {
  lh_dedup_peer_t *peer = find_peer(dedup, src_id);

  if (peer == NULL) {
    peer = claim_peer(dedup, src_id, now_us);
    peer->last_seq = seq;
    peer->bitmap = 1u; /* bit 0 = this frame */
    dedup->stat_accepted++;
    return true;
  }

  peer->last_seen_us = now_us;
  const int8_t delta = seq_delta(seq, peer->last_seq);

  if (delta > 0) {
    /* Newer than anything seen. Slide the window; a step of 32 or more leaves
     * nothing of the old window worth keeping, and C's shift by >= width is
     * undefined, so that case is written out rather than relied upon. */
    peer->bitmap = ((uint8_t)delta >= LH_DEDUP_WINDOW) ? 0u : (peer->bitmap << (uint8_t)delta);
    peer->bitmap |= 1u;
    peer->last_seq = seq;
    dedup->stat_accepted++;
    return true;
  }

  const uint8_t age = (uint8_t)(-delta); /* 0 = the newest frame itself */

  if (age >= LH_DEDUP_WINDOW) {
    /* Too far behind to know whether we have seen it. Rejecting is the safe
     * answer: a late duplicate costs one dropped frame, a late *original*
     * processed twice costs correctness, and the sender's ARQ will notice
     * either way. */
    dedup->stat_too_old++;
    return false;
  }

  const uint32_t mask = 1u << age;
  if ((peer->bitmap & mask) != 0u) {
    dedup->stat_dupes_dropped++;
    return false;
  }

  peer->bitmap |= mask;
  dedup->stat_accepted++;
  return true;
}
