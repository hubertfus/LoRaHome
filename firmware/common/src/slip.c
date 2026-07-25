#include "lorahome/slip.h"

/*
 * Roadmap T1.1 budget: the decoder's own state must fit in 32 B, exclusive of
 * the frame buffer it points at. Checked here rather than in a test because a
 * test only fails on the machine that runs it, while this fails on every target
 * that compiles the file — including the two ESP ABIs, which is where the
 * budget actually matters. On ILP32 (xtensa, riscv32) the struct is 28 B; on
 * LP64 the wider pointer takes it to exactly 32 B.
 */
_Static_assert(sizeof(lh_slip_decoder_t) <= 32, "slip decoder struct budget breach (T1.1: 32 B)");

/*
 * Nothing in this file may grow a state that survives a frame boundary. The
 * decoder is restartable from any byte, which is what lets the bridge recover
 * from a peer reboot without anybody noticing.
 */

void lh_slip_init(lh_slip_decoder_t *dec, uint8_t *buf, uint16_t cap) {
  dec->buf = buf;
  dec->cap = cap;
  dec->len = 0;
  dec->state = LH_SLIP_IDLE;
  dec->stat_frames_ok = 0;
  dec->stat_overflow = 0;
  dec->stat_bad_escape = 0;
  dec->stat_dropped = 0;
}

void lh_slip_reset(lh_slip_decoder_t *dec) {
  dec->len = 0;
  dec->state = LH_SLIP_IDLE;
}

/** Discards the frame in progress and waits for the next delimiter. */
static void enter_resync(lh_slip_decoder_t *dec) {
  dec->len = 0;
  dec->state = LH_SLIP_RESYNC;
  dec->stat_dropped++;
}

/** Appends one decoded byte. Returns false when the frame no longer fits. */
static int append(lh_slip_decoder_t *dec, uint8_t byte) {
  if (dec->len >= dec->cap) {
    /* The one thing this function must never do is write past cap. Everything
     * else about an oversized frame — which counter, which state — is
     * bookkeeping; this is memory safety. */
    dec->stat_overflow++;
    enter_resync(dec);
    return 0;
  }
  dec->buf[dec->len++] = byte;
  return 1;
}

lh_slip_feed_r lh_slip_feed(lh_slip_decoder_t *dec, uint8_t byte) {
  /* A frame delivered on the previous call has now been read (or forfeited).
   * Clearing it here rather than exposing a consume() call removes the one
   * ordering mistake a caller could make: reading dec->buf after it has already
   * been overwritten. In IDLE there is never a partial frame to lose. */
  if (dec->state == LH_SLIP_IDLE) dec->len = 0;

  switch (dec->state) {
    case LH_SLIP_IDLE:
      if (byte == LH_SLIP_END) return LH_SLIP_NEED_MORE; /* idle-line delimiter */
      if (byte == LH_SLIP_ESC) {
        dec->state = LH_SLIP_ESCAPED;
        return LH_SLIP_NEED_MORE;
      }
      dec->state = LH_SLIP_IN_FRAME;
      return append(dec, byte) ? LH_SLIP_NEED_MORE : LH_SLIP_ERROR;

    case LH_SLIP_IN_FRAME:
      if (byte == LH_SLIP_END) {
        dec->state = LH_SLIP_IDLE;
        if (dec->len == 0) return LH_SLIP_NEED_MORE; /* empty frame: not a frame */
        dec->stat_frames_ok++;
        return LH_SLIP_FRAME_READY;
      }
      if (byte == LH_SLIP_ESC) {
        dec->state = LH_SLIP_ESCAPED;
        return LH_SLIP_NEED_MORE;
      }
      return append(dec, byte) ? LH_SLIP_NEED_MORE : LH_SLIP_ERROR;

    case LH_SLIP_ESCAPED:
      /* RFC 1055 defines exactly two legal continuations. Anything else is
       * corruption, and the frame it belongs to cannot be trusted. The
       * permissive reading — pass the byte through unescaped — turns a
       * detectable framing error into undetectable payload corruption, to be
       * found later by CRC or not at all. */
      if (byte == LH_SLIP_ESC_END) {
        dec->state = LH_SLIP_IN_FRAME;
        return append(dec, (uint8_t)LH_SLIP_END) ? LH_SLIP_NEED_MORE : LH_SLIP_ERROR;
      }
      if (byte == LH_SLIP_ESC_ESC) {
        dec->state = LH_SLIP_IN_FRAME;
        return append(dec, (uint8_t)LH_SLIP_ESC) ? LH_SLIP_NEED_MORE : LH_SLIP_ERROR;
      }
      dec->stat_bad_escape++;
      enter_resync(dec);
      /* An ESC immediately followed by END is a truncated frame, and the END
       * that revealed it is also the delimiter we would resynchronise on. Treat
       * it as consumed: staying in RESYNC would swallow the whole next frame. */
      if (byte == LH_SLIP_END) dec->state = LH_SLIP_IDLE;
      return LH_SLIP_ERROR;

    case LH_SLIP_RESYNC:
    default:
      if (byte == LH_SLIP_END) dec->state = LH_SLIP_IDLE;
      return LH_SLIP_NEED_MORE;
  }
}

uint16_t lh_slip_encode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t out_cap) {
  /*
   * Worst case is demanded up front, not discovered halfway through.
   *
   * The alternative — encode until the buffer runs out, then fail — leaves
   * garbage in the caller's buffer and makes success depend on payload content,
   * so a link can run for weeks and then refuse a frame because that frame
   * happened to be full of 0xC0. Refusing on capacity alone makes the failure
   * deterministic and testable, and it is what risk R1.1 asks for: TX buffers
   * are sized 2*MAX+2, checked with _Static_assert at the call site.
   */
  const uint32_t worst_case = LH_SLIP_ENCODED_MAX(in_len);
  if (worst_case > (uint32_t)out_cap) return 0;

  uint16_t out_len = 0;
  out[out_len++] = (uint8_t)LH_SLIP_END;

  for (uint16_t i = 0; i < in_len; i++) {
    const uint8_t byte = in[i];
    if (byte == LH_SLIP_END) {
      out[out_len++] = (uint8_t)LH_SLIP_ESC;
      out[out_len++] = (uint8_t)LH_SLIP_ESC_END;
    } else if (byte == LH_SLIP_ESC) {
      out[out_len++] = (uint8_t)LH_SLIP_ESC;
      out[out_len++] = (uint8_t)LH_SLIP_ESC_ESC;
    } else {
      out[out_len++] = byte;
    }
  }

  out[out_len++] = (uint8_t)LH_SLIP_END;
  return out_len;
}
