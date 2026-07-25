/**
 * SLIP framing (RFC 1055) — the TypeScript twin of firmware/common/src/slip.c.
 * Roadmap T1.5; wire format in ARCHITECTURE.md §7.1.
 *
 * Two implementations of one wire format is the exact shape of the problem this
 * project exists to prevent, so they are held together by evidence rather than
 * discipline: `tools/check-slip-cross.mjs` runs a thousand buffers through
 * C-encode/TS-decode and TS-encode/C-decode and requires byte equality. If the
 * two ever drift, that fails before anything reaches a radio.
 *
 * The behaviour therefore matches slip.c deliberately and in detail, including
 * the parts that look like they could be relaxed:
 *
 *   - An empty frame (END END) is a delimiter pair, not a zero-length message,
 *     and is not delivered.
 *   - An illegal escape pair is an error, not a byte passed through raw.
 *     Silently accepting it turns detectable framing damage into undetectable
 *     payload damage.
 *   - After a structural error the decoder discards until the next END, so one
 *     damaged frame costs one frame.
 *   - `encode` refuses a buffer that could not hold the worst case, rather than
 *     failing part-way through a payload that happened to need escaping.
 */

export const SLIP_END = 0xc0;
export const SLIP_ESC = 0xdb;
export const SLIP_ESC_END = 0xdc;
export const SLIP_ESC_ESC = 0xdd;

/** Worst-case encoded size: every byte escaped, plus both delimiters. */
export function slipEncodedMax(payloadLength: number): number {
  return 2 * payloadLength + 2;
}

export enum SlipState {
  Idle = 0,
  InFrame = 1,
  Escaped = 2,
  /** Frame damaged; discarding until the next delimiter. See slip.c. */
  Resync = 3,
}

export interface SlipStats {
  framesOk: number;
  /** Frames exceeding the decoder's capacity. */
  overflow: number;
  /** ESC followed by an illegal byte. */
  badEscape: number;
  /** Frames discarded, for either reason above. */
  dropped: number;
}

/**
 * Encodes one frame: END, escaped payload, END.
 *
 * Allocates exactly one Buffer. The two-pass shape (measure, then fill) is
 * deliberate: growing an array and converting costs several allocations per
 * frame, and T1.5 budgets at most two.
 */
export function slipEncode(payload: Uint8Array): Buffer {
  let escapes = 0;
  for (const byte of payload) {
    if (byte === SLIP_END || byte === SLIP_ESC) escapes++;
  }

  const out = Buffer.allocUnsafe(payload.length + escapes + 2);
  let index = 0;
  out[index++] = SLIP_END;

  for (const byte of payload) {
    if (byte === SLIP_END) {
      out[index++] = SLIP_ESC;
      out[index++] = SLIP_ESC_END;
    } else if (byte === SLIP_ESC) {
      out[index++] = SLIP_ESC;
      out[index++] = SLIP_ESC_ESC;
    } else {
      out[index++] = byte;
    }
  }

  out[index++] = SLIP_END;
  return out;
}

/**
 * Streaming decoder, fed whatever arrives from the port.
 *
 * A serial read hands over whatever was in the OS buffer, so frames routinely
 * straddle chunks; there is no useful "decode one frame" entry point.
 */
export class SlipDecoder {
  private readonly buffer: Uint8Array;
  private length = 0;
  private state: SlipState = SlipState.Idle;

  readonly stats: SlipStats = { framesOk: 0, overflow: 0, badEscape: 0, dropped: 0 };

  /**
   * @param capacity Largest frame accepted. Matches the Bridge's own receive
   *   buffer by default — a frame the Bridge could never have sent is one we do
   *   not need to be able to receive.
   */
  constructor(capacity = 256) {
    this.buffer = new Uint8Array(capacity);
  }

  get currentState(): SlipState {
    return this.state;
  }

  /** Clears framing state, keeping the counters — for a reopened port. */
  reset(): void {
    this.length = 0;
    this.state = SlipState.Idle;
  }

  private enterResync(): void {
    this.length = 0;
    this.state = SlipState.Resync;
    this.stats.dropped++;
  }

  /** Returns false when the frame no longer fits, having written nothing. */
  private append(byte: number): boolean {
    if (this.length >= this.buffer.length) {
      this.stats.overflow++;
      this.enterResync();
      return false;
    }
    this.buffer[this.length++] = byte;
    return true;
  }

  /**
   * Feeds a chunk and returns the frames it completed.
   *
   * Each frame is a copy, not a view into the internal buffer. The C decoder
   * hands back a pointer and requires the caller to consume it before the next
   * byte; that contract is enforceable in C and a trap in JavaScript, where the
   * natural thing to do with a frame is put it on a queue and look at it later.
   * One copy per frame is the right price for removing that whole class of bug.
   */
  push(chunk: Uint8Array): Buffer[] {
    const frames: Buffer[] = [];

    for (const byte of chunk) {
      if (this.state === SlipState.Idle) this.length = 0;

      switch (this.state) {
        case SlipState.Idle:
          if (byte === SLIP_END) break; // idle-line delimiter
          if (byte === SLIP_ESC) {
            this.state = SlipState.Escaped;
            break;
          }
          this.state = SlipState.InFrame;
          this.append(byte);
          break;

        case SlipState.InFrame:
          if (byte === SLIP_END) {
            this.state = SlipState.Idle;
            if (this.length === 0) break; // empty frame: not a frame
            this.stats.framesOk++;
            frames.push(Buffer.from(this.buffer.subarray(0, this.length)));
            break;
          }
          if (byte === SLIP_ESC) {
            this.state = SlipState.Escaped;
            break;
          }
          this.append(byte);
          break;

        case SlipState.Escaped:
          if (byte === SLIP_ESC_END) {
            this.state = SlipState.InFrame;
            this.append(SLIP_END);
            break;
          }
          if (byte === SLIP_ESC_ESC) {
            this.state = SlipState.InFrame;
            this.append(SLIP_ESC);
            break;
          }
          this.stats.badEscape++;
          this.enterResync();
          // ESC immediately followed by END is a truncated frame, and the END
          // that revealed it is also the delimiter we would resynchronise on.
          // Consume it, or the next frame is swallowed too.
          if (byte === SLIP_END) this.state = SlipState.Idle;
          break;

        case SlipState.Resync:
        default:
          if (byte === SLIP_END) this.state = SlipState.Idle;
          break;
      }
    }

    return frames;
  }
}
