#include "lorahome/frag.h"

#include <string.h>

#include "lorahome/crc16.h"

/*
 * Etap 2 RAM budget: the reassembler is 1664 B of the reliability context's
 * 2304. The buffer alone is 1600, so this assertion is really about the
 * bookkeeping around it — a field added here is a field taken from dedup or
 * from the ARQ, and that trade should be a build failure and a conversation
 * rather than a surprise in the field.
 */
_Static_assert(sizeof(lh_reassembler_t) <= 1664, "reassembler RAM budget breach — roadmap Etap 2");

/* The mask is one bit per fragment. More fragments than bits would silently
 * alias fragment 8 onto fragment 0 — the exact class of bug this file exists
 * to prevent, arriving through the back door. */
_Static_assert(LH_FRAG_MAX_FRAGMENTS <= 8, "received_mask holds 8 fragments");

void lh_frag_hdr_encode(const lh_frag_hdr_t *hdr, uint8_t *out) {
  out[0] = (uint8_t)(hdr->cfg_id >> 8);
  out[1] = (uint8_t)(hdr->cfg_id & 0xFF);
  out[2] = hdr->frag_index;
  out[3] = hdr->frag_total;
  out[4] = (uint8_t)(hdr->total_len >> 8);
  out[5] = (uint8_t)(hdr->total_len & 0xFF);
  out[6] = (uint8_t)(hdr->crc_total >> 8);
  out[7] = (uint8_t)(hdr->crc_total & 0xFF);
}

void lh_frag_hdr_decode(const uint8_t *in, lh_frag_hdr_t *out) {
  out->cfg_id = (uint16_t)((in[0] << 8) | in[1]);
  out->frag_index = in[2];
  out->frag_total = in[3];
  out->total_len = (uint16_t)((in[4] << 8) | in[5]);
  out->crc_total = (uint16_t)((in[6] << 8) | in[7]);
}

uint8_t lh_frag_count(uint16_t total_len) {
  if (total_len > LH_FRAG_CONFIG_MAX) return 0;
  /* An empty config is still one fragment: "nothing to configure" is a
   * statement, and a transaction of zero fragments could never complete. */
  if (total_len == 0) return 1;
  return (uint8_t)((total_len + LH_FRAG_PAYLOAD_MAX - 1u) / LH_FRAG_PAYLOAD_MAX);
}

int lh_frag_build(uint16_t cfg_id, uint8_t index, const uint8_t *config, uint16_t total_len,
                  uint8_t *out, uint16_t out_cap) {
  const uint8_t total = lh_frag_count(total_len);
  if (total == 0 || index >= total) return -5; /* LH_ERR_TOO_LONG */

  const uint16_t offset = (uint16_t)(index * LH_FRAG_PAYLOAD_MAX);
  uint16_t slice = (uint16_t)(total_len - offset);
  if (slice > LH_FRAG_PAYLOAD_MAX) slice = LH_FRAG_PAYLOAD_MAX;

  if (out_cap < LH_FRAG_HDR_SIZE + slice) return -1; /* LH_ERR_TOO_SHORT */

  const lh_frag_hdr_t hdr = {
      cfg_id, index, total, total_len, lorahome_crc16(config, total_len),
  };
  lh_frag_hdr_encode(&hdr, out);
  if (slice > 0) memcpy(out + LH_FRAG_HDR_SIZE, config + offset, slice);

  return (int)(LH_FRAG_HDR_SIZE + slice);
}

void lh_frag_reset(lh_reassembler_t *reasm) {
  /* Counters survive a reset: they are the history of this session, and a
   * transaction ending is not a reason to forget how many timed out. The buffer
   * is left as it is — nothing reads it while `active` is false, and clearing
   * 1600 B on every config would be pure ceremony. */
  reasm->cfg_id = 0;
  reasm->received_mask = 0;
  reasm->expected_total = 0;
  reasm->total_len = 0;
  reasm->crc_expected = 0;
  reasm->first_frag_us = 0;
  reasm->active = false;
}

/** Geometry a fragment header must satisfy before anything is stored. */
static bool header_is_sane(const lh_frag_hdr_t *hdr, uint16_t slice_len) {
  if (hdr->frag_total == 0 || hdr->frag_total > LH_FRAG_MAX_FRAGMENTS) return false;
  if (hdr->frag_index >= hdr->frag_total) return false;
  if (hdr->total_len > LH_FRAG_CONFIG_MAX) return false;
  /* The declared total and the declared fragment count must describe the same
   * config. A sender claiming 3 fragments for 100 bytes is either broken or
   * hostile, and either way its fragments must not be stored. */
  if (lh_frag_count(hdr->total_len) != hdr->frag_total) return false;

  const uint16_t offset = (uint16_t)(hdr->frag_index * LH_FRAG_PAYLOAD_MAX);
  if (offset > hdr->total_len) return false;

  uint16_t expected = (uint16_t)(hdr->total_len - offset);
  if (expected > LH_FRAG_PAYLOAD_MAX) expected = LH_FRAG_PAYLOAD_MAX;
  /* The slice must be exactly the length its position implies. Accepting a
   * short one would leave uninitialised bytes inside a config that then passes
   * its CRC only by luck. */
  return slice_len == expected;
}

lh_frag_result_t lh_frag_feed(lh_reassembler_t *reasm, const uint8_t *payload, uint16_t len,
                              int64_t now_us) {
  if (len < LH_FRAG_HDR_SIZE) return LH_FRAG_ERR_HEADER;

  lh_frag_hdr_t hdr;
  lh_frag_hdr_decode(payload, &hdr);
  const uint16_t slice_len = (uint16_t)(len - LH_FRAG_HDR_SIZE);

  if (hdr.total_len > LH_FRAG_CONFIG_MAX) return LH_FRAG_ERR_TOO_LARGE;
  if (!header_is_sane(&hdr, slice_len)) return LH_FRAG_ERR_HEADER;

  /* A stale transaction is expired here as well as in lh_frag_tick, so a
   * caller that never ticks still recovers — it simply recovers on the next
   * fragment rather than on the clock. */
  if (reasm->active && (now_us - reasm->first_frag_us) >= (int64_t)LH_FRAG_TIMEOUT_MS * 1000) {
    reasm->stat_timeouts++;
    lh_frag_reset(reasm);
  }

  if (!reasm->active) {
    lh_frag_reset(reasm);
    reasm->active = true;
    reasm->cfg_id = hdr.cfg_id;
    reasm->expected_total = hdr.frag_total;
    reasm->total_len = hdr.total_len;
    reasm->crc_expected = hdr.crc_total;
    reasm->first_frag_us = now_us;
  } else if (hdr.cfg_id != reasm->cfg_id || hdr.frag_total != reasm->expected_total ||
             hdr.total_len != reasm->total_len || hdr.crc_total != reasm->crc_expected) {
    /* Refused, and the transaction in progress is left untouched. The other
     * choice — letting the newcomer take the slot — would let a stray fragment
     * from a retransmission destroy a config that was one frame from done.
     *
     * All four fields are compared, not just the id: a `cfg_id` that wrapped
     * onto a live transaction agrees on the id and on nothing else. */
    reasm->stat_foreign++;
    return LH_FRAG_ERR_FOREIGN;
  }

  const uint8_t bit = (uint8_t)(1u << hdr.frag_index);
  if ((reasm->received_mask & bit) != 0) {
    reasm->stat_duplicates++;
    return LH_FRAG_DUPLICATE;
  }

  const uint16_t offset = (uint16_t)(hdr.frag_index * LH_FRAG_PAYLOAD_MAX);
  if (slice_len > 0) memcpy(reasm->buf + offset, payload + LH_FRAG_HDR_SIZE, slice_len);
  reasm->received_mask |= bit;

  const uint8_t complete_mask =
      (uint8_t)((reasm->expected_total >= 8) ? 0xFFu : ((1u << reasm->expected_total) - 1u));
  if (reasm->received_mask != complete_mask) return LH_FRAG_NEED_MORE;

  const uint16_t crc = lorahome_crc16(reasm->buf, reasm->total_len);
  const bool crc_ok = (crc == reasm->crc_expected);
  const uint16_t assembled_len = reasm->total_len;

  /* The slot is released either way. A failed assembly is not going to be
   * repaired by another fragment, and holding the slot would block the retry
   * that can repair it. */
  lh_frag_reset(reasm);
  reasm->total_len = assembled_len; /* the caller still needs the length */

  if (!crc_ok) {
    reasm->stat_crc_fail++;
    return LH_FRAG_ERR_CRC;
  }

  reasm->stat_completed++;
  return LH_FRAG_COMPLETE;
}

bool lh_frag_tick(lh_reassembler_t *reasm, int64_t now_us) {
  if (!reasm->active) return false;
  if ((now_us - reasm->first_frag_us) < (int64_t)LH_FRAG_TIMEOUT_MS * 1000) return false;

  reasm->stat_timeouts++;
  lh_frag_reset(reasm);
  return true;
}
