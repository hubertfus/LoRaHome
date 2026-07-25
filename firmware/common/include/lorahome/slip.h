#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SLIP framing (RFC 1055) for the Bridge's serial link to the Host.
 * Roadmap T1.1; wire format described in ARCHITECTURE.md §7.
 *
 * Two properties earn SLIP its place here over newline- or timeout-delimited
 * framing: it is binary-transparent (no byte value is forbidden in a payload),
 * and it is self-synchronising (after any amount of line noise the decoder is
 * back in a known state at the next END byte, with no timeout to wait out).
 *
 * The decoder is fed one byte at a time because that is how bytes arrive: a
 * UART hands over whatever happened to be in the FIFO, so frames routinely
 * straddle reads. A decoder that needed a whole frame at once would have to
 * buffer and re-scan, and would still have to solve this problem.
 *
 * Allocation: none, ever. The caller owns the frame buffer and passes it to
 * lh_slip_init(); the decoder never resizes, copies or frees it. This is the
 * project's zero-copy doctrine — the owner is always higher up the call stack
 * and statically allocated.
 */

#define LH_SLIP_END 0xC0u     /* frame delimiter                    */
#define LH_SLIP_ESC 0xDBu     /* escape prefix                      */
#define LH_SLIP_ESC_END 0xDCu /* second byte of an escaped 0xC0     */
#define LH_SLIP_ESC_ESC 0xDDu /* second byte of an escaped 0xDB     */

/**
 * Encoded size of the worst possible payload: every byte escaped, plus the two
 * delimiters. Used for `_Static_assert` on TX buffer sizing — risk R1.1 is a
 * transmit buffer sized for the typical case and overrun by an unlucky payload.
 */
#define LH_SLIP_ENCODED_MAX(payload_len) (2u * (uint32_t)(payload_len) + 2u)

/**
 * Decoder state.
 *
 * RESYNC is the one state the roadmap sketch did not name, and it is the one
 * that makes recovery real. Without it, a structural error (a bad escape pair,
 * an overrun) would leave the decoder ready to start a fresh frame from the
 * very next byte — which is the middle of a frame it has already decided is
 * damaged. It would then emit the tail of that frame as if it were whole, and
 * the corruption would be pushed one layer up to be caught by CRC, or not.
 * RESYNC discards to the next END, which is exactly what "self-synchronising"
 * is supposed to buy.
 */
typedef enum {
  LH_SLIP_IDLE = 0,     /* between frames; the next data byte opens one    */
  LH_SLIP_IN_FRAME = 1, /* accumulating payload                            */
  LH_SLIP_ESCAPED = 2,  /* last byte was ESC; awaiting ESC_END/ESC_ESC     */
  LH_SLIP_RESYNC = 3,   /* frame damaged; discarding until the next END    */
} lh_slip_state_t;

/** Result of feeding one byte. */
typedef enum {
  LH_SLIP_NEED_MORE = 0,   /* keep feeding                                  */
  LH_SLIP_FRAME_READY = 1, /* complete frame in dec->buf[0 .. dec->len)     */
  LH_SLIP_ERROR = -1,      /* frame dropped; decoder is resynchronising     */
} lh_slip_feed_r;

/**
 * Streaming decoder.
 *
 * The counters are not diagnostics-for-later; they are the link's health
 * readout. `stat_overflow` rising means somebody is sending frames larger than
 * this buffer — a configuration error, not line noise. `stat_bad_escape` rising
 * means genuine corruption on the wire. Distinguishing the two while standing
 * in a field with a laptop is worth the sixteen bytes.
 */
typedef struct {
  uint8_t *buf; /* caller-owned frame buffer                       */
  uint16_t cap; /* its capacity in bytes                           */
  uint16_t len; /* decoded bytes currently in buf                  */
  lh_slip_state_t state;
  uint32_t stat_frames_ok;   /* frames delivered to the caller       */
  uint32_t stat_overflow;    /* frames exceeding cap                 */
  uint32_t stat_bad_escape;  /* ESC followed by an illegal byte      */
  uint32_t stat_dropped;     /* frames discarded (overflow + escape) */
} lh_slip_decoder_t;

/**
 * Binds a caller-owned buffer to the decoder and clears state and counters.
 * `cap` is the largest frame that can be received; anything longer is counted
 * in `stat_overflow` and dropped rather than written past the end.
 */
void lh_slip_init(lh_slip_decoder_t *dec, uint8_t *buf, uint16_t cap);

/**
 * Clears framing state without touching the counters.
 *
 * For a deliberate restart of the link (port reopened, peer rebooted), where
 * any partial frame in flight is meaningless but the running totals are still
 * the history of this session.
 */
void lh_slip_reset(lh_slip_decoder_t *dec);

/**
 * Feeds one received byte.
 *
 * On LH_SLIP_FRAME_READY the frame occupies dec->buf[0 .. dec->len) and stays
 * there until the next call to this function, which is free to overwrite it.
 * There is deliberately no "consume" call to forget: the caller reads the frame
 * before feeding the next byte, or it does not get to read it at all.
 *
 * On LH_SLIP_ERROR the offending frame has been discarded and the decoder is in
 * RESYNC; feeding continues normally and the next END starts a clean frame.
 * The error is informational — there is nothing for the caller to do about it
 * beyond noticing.
 */
lh_slip_feed_r lh_slip_feed(lh_slip_decoder_t *dec, uint8_t byte);

/**
 * Encodes one frame: END, escaped payload, END.
 *
 * Returns the number of bytes written, or 0 if `out_cap` is too small for the
 * worst case — nothing is written in that case. 0 is unambiguous as a failure
 * value because a successful encode always writes at least the two delimiters.
 *
 * The leading END is not decoration. RFC 1055 recommends it precisely so that
 * noise accumulated on an idle line is flushed as an empty frame rather than
 * being prepended to real data.
 */
uint16_t lh_slip_encode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t out_cap);

#ifdef __cplusplus
}
#endif
