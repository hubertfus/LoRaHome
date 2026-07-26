#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Duplicate-suppression window, per sender. Roadmap T2.1.
 *
 * A radio link that retransmits will deliver the same frame twice, and a frame
 * processed twice is not a cosmetic problem: a config fragment applied twice is
 * harmless, but a "toggle the relay" command applied twice leaves the relay in
 * the wrong state, and a counter incremented twice corrupts telemetry nobody
 * will think to distrust. Etap 2's hard invariant — `chaos.double_processed ==
 * 0` — is enforced here or nowhere.
 *
 * The window is a 32-bit bitmap per peer rather than a list of recent sequence
 * numbers. Bit n means "sequence number (last_seq - n) has been seen", so bit 0
 * is always the newest frame accepted. Checking and marking are a shift, a test
 * and an or: O(1) with no loop over history, and 4 bytes of state per peer
 * where a 32-entry table would have cost 32. On a node with a 48 kB static RAM
 * budget that is the difference between tracking eight peers and arguing about
 * whether we can afford to.
 *
 * Sequence numbers are 8-bit and wrap every 256 frames. Every comparison here
 * goes through `(int8_t)(a - b)` — modular distance, never `a > b`. Risk R2.1
 * is exactly the bug where a link works perfectly for 256 frames and then
 * rejects everything as a duplicate, in the field, after the installer has gone
 * home.
 */

/** Sequence numbers behind the newest that are still remembered. */
#define LH_DEDUP_WINDOW 32u

/** Senders tracked concurrently; the ninth evicts the least recently heard. */
#define LH_DEDUP_PEERS 8u

typedef struct {
  uint16_t src_id;      /* sender's node id                                  */
  uint8_t last_seq;     /* newest sequence number accepted from it           */
  uint32_t bitmap;      /* bit n: (last_seq - n) has been seen; bit 0 always set */
  int64_t last_seen_us; /* for LRU eviction, not for expiry                  */
} lh_dedup_peer_t;

typedef struct {
  lh_dedup_peer_t peers[LH_DEDUP_PEERS];
  uint8_t peer_count;
  /**
   * Counters, split by cause because they mean different things in the field.
   *
   * `stat_dupes_dropped` rising is the ARQ doing its job — retransmissions that
   * were not actually lost, just slow. `stat_too_old` rising means frames are
   * arriving more than a window behind the newest, which is reordering deeper
   * than 32 frames and points at a link problem, not a duplicate. And
   * `stat_peer_evicted` rising at all means more than eight senders are talking
   * to this node, at which point dedup is a rotating cache rather than a
   * guarantee — the one condition under which the invariant above can break.
   */
  uint32_t stat_dupes_dropped;
  uint32_t stat_too_old;
  uint32_t stat_peer_evicted;
  uint32_t stat_accepted;
} lh_dedup_t;

/** Clears the table and the counters. Safe on a struct in .bss that is already zero. */
void lh_dedup_init(lh_dedup_t *dedup);

/**
 * Decides whether a frame is new, and remembers it if it is.
 *
 * Returns true for a frame that should be processed, false for a duplicate or
 * one older than the window. The call is not idempotent — that is the point.
 *
 * `now_us` is passed in rather than read from a clock inside, because
 * firmware/common has no platform: the same object file is linked into the
 * ESP32 firmware, the native harness and the cross-compile checks. The roadmap
 * sketch showed a three-argument signature; taking the timestamp explicitly is
 * the same choice already made for `lh_bridge_ctx_t`'s callbacks, and it is
 * what lets the eviction path be tested in a millisecond rather than by waiting.
 * It is used only to order peers for eviction; entries never expire on their own.
 */
bool lh_dedup_check_and_mark(lh_dedup_t *dedup, uint16_t src_id, uint8_t seq, int64_t now_us);

/**
 * Looks up a peer without modifying anything. NULL if it is not tracked.
 *
 * For diagnostics and tests. The returned pointer is into the table and is
 * invalidated by the next accepted frame from a new sender.
 */
const lh_dedup_peer_t *lh_dedup_find_peer(const lh_dedup_t *dedup, uint16_t src_id);

#ifdef __cplusplus
}
#endif
