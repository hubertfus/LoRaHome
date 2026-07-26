/**
 * The CBOR subset this project puts on the air. Twin of firmware/common/src/cbor.c.
 *
 * Deliberately not a CBOR library, and deliberately not a dependency either.
 * It handles unsigned integers, negative integers, byte strings, arrays and
 * maps in definite-length form and nothing else — no tags, no floats, no
 * indefinite lengths, no text strings.
 *
 * Every absence is a decision, and the one worth restating is text strings:
 * CONTRIBUTING.md §3 forbids string keys on the wire, and the enforcement is
 * that there is no encoder for them here. A reviewer does not have to catch it.
 *
 * The two implementations are checked against each other byte for byte by
 * tools/check-capability-cross.mjs. That check is only meaningful because both
 * sides encode canonically — shortest form for every argument — so "the same
 * value" and "the same bytes" mean the same thing.
 */

export const CborMajor = {
  Uint: 0,
  Nint: 1,
  Bytes: 2,
  Array: 4,
  Map: 5,
} as const;

/** Deepest nesting `skip` will descend before refusing. Matches SKIP_MAX_DEPTH in C. */
export const CBOR_SKIP_MAX_DEPTH = 8;

export class CborWriter {
  private readonly parts: number[] = [];

  /** Encodes a major type and argument in the shortest form that fits. */
  private head(major: number, argument: number): void {
    if (!Number.isInteger(argument) || argument < 0) {
      throw new RangeError(`CBOR argument must be a non-negative integer, got ${argument}`);
    }
    const prefix = major << 5;

    if (argument < 24) {
      this.parts.push(prefix | argument);
    } else if (argument <= 0xff) {
      this.parts.push(prefix | 24, argument);
    } else if (argument <= 0xffff) {
      this.parts.push(prefix | 25, (argument >>> 8) & 0xff, argument & 0xff);
    } else if (argument <= 0xffffffff) {
      this.parts.push(
        prefix | 26,
        (argument >>> 24) & 0xff,
        (argument >>> 16) & 0xff,
        (argument >>> 8) & 0xff,
        argument & 0xff,
      );
    } else {
      // Above 2^32 JavaScript's bitwise operators stop working, so the split is
      // done with arithmetic. Reachable only from a 64-bit field, which nothing
      // in this protocol has yet — but silently emitting a truncated argument
      // would be a wire format that disagrees with C for large values.
      const high = Math.floor(argument / 0x100000000);
      const low = argument >>> 0;
      this.parts.push(
        prefix | 27,
        (high >>> 24) & 0xff,
        (high >>> 16) & 0xff,
        (high >>> 8) & 0xff,
        high & 0xff,
        (low >>> 24) & 0xff,
        (low >>> 16) & 0xff,
        (low >>> 8) & 0xff,
        low & 0xff,
      );
    }
  }

  uint(value: number): this {
    this.head(CborMajor.Uint, value);
    return this;
  }

  int(value: number): this {
    if (!Number.isInteger(value)) throw new RangeError(`not an integer: ${value}`);
    if (value >= 0) return this.uint(value);
    // Negative integers encode -1 - n, so -1 has argument 0.
    this.head(CborMajor.Nint, -1 - value);
    return this;
  }

  bytes(value: Uint8Array): this {
    this.head(CborMajor.Bytes, value.length);
    for (const byte of value) this.parts.push(byte);
    return this;
  }

  array(items: number): this {
    this.head(CborMajor.Array, items);
    return this;
  }

  map(pairs: number): this {
    this.head(CborMajor.Map, pairs);
    return this;
  }

  finish(): Uint8Array {
    return Uint8Array.from(this.parts);
  }
}

export class CborError extends Error {}

export class CborReader {
  private pos = 0;

  constructor(private readonly buf: Uint8Array) {}

  get position(): number {
    return this.pos;
  }

  get done(): boolean {
    return this.pos >= this.buf.length;
  }

  private head(): { major: number; argument: number } {
    if (this.pos >= this.buf.length) throw new CborError('truncated: no initial byte');

    const initial = this.buf[this.pos++]!;
    const major = initial >>> 5;
    const info = initial & 0x1f;

    if (info < 24) return { major, argument: info };

    const widths: Record<number, number> = { 24: 1, 25: 2, 26: 4, 27: 8 };
    const width = widths[info];
    if (width === undefined) {
      // 28..30 are reserved; 31 is the indefinite-length marker. Refused rather
      // than skipped — an indefinite item is a stream with no stated end, and
      // the C side has a fixed receive buffer.
      throw new CborError(`unsupported additional information ${info}`);
    }
    if (this.pos + width > this.buf.length) throw new CborError('truncated argument');

    let argument = 0;
    for (let i = 0; i < width; i++) argument = argument * 256 + this.buf[this.pos++]!;
    return { major, argument };
  }

  private typed(expected: number): number {
    const { major, argument } = this.head();
    if (major !== expected) {
      throw new CborError(`expected major type ${expected}, got ${major}`);
    }
    return argument;
  }

  uint(): number {
    return this.typed(CborMajor.Uint);
  }

  int(): number {
    const { major, argument } = this.head();
    if (major === CborMajor.Uint) return argument;
    if (major === CborMajor.Nint) return -1 - argument;
    throw new CborError(`expected an integer, got major type ${major}`);
  }

  bytes(): Uint8Array {
    const length = this.typed(CborMajor.Bytes);
    if (this.pos + length > this.buf.length) throw new CborError('truncated byte string');
    const out = this.buf.slice(this.pos, this.pos + length);
    this.pos += length;
    return out;
  }

  array(): number {
    const items = this.typed(CborMajor.Array);
    // A declared count larger than the bytes remaining cannot be honest: every
    // item costs at least one byte. Refusing it here stops a crafted header
    // from driving a caller's loop far past the buffer.
    if (items > this.buf.length - this.pos) throw new CborError('array longer than the buffer');
    return items;
  }

  map(): number {
    const pairs = this.typed(CborMajor.Map);
    if (pairs > Math.floor((this.buf.length - this.pos) / 2) + 1) {
      throw new CborError('map longer than the buffer');
    }
    return pairs;
  }

  /**
   * Skips one item, whatever it is, including a nested array or map.
   *
   * The forward-compatibility hook: a host meeting a key from a newer node
   * steps over it rather than refusing the message, so a protocol addition is
   * not a flag day across every device in a building.
   *
   * Iterative with an explicit depth cap, matching the C side. The input is a
   * radio frame from a device that may be faulty, and depth is the one thing a
   * sender controls that a recursive walk cannot survive.
   */
  skip(): void {
    let pending = 1;
    let depth = 0;

    while (pending > 0) {
      const { major, argument } = this.head();
      pending--;

      switch (major) {
        case CborMajor.Uint:
        case CborMajor.Nint:
          break;
        case CborMajor.Bytes:
          if (argument > this.buf.length - this.pos) throw new CborError('truncated byte string');
          this.pos += argument;
          break;
        case CborMajor.Array:
        case CborMajor.Map: {
          if (++depth > CBOR_SKIP_MAX_DEPTH) throw new CborError('nesting too deep');
          const items = major === CborMajor.Map ? argument * 2 : argument;
          if (items > this.buf.length - this.pos) throw new CborError('container longer than buffer');
          pending += items;
          break;
        }
        default:
          throw new CborError(`major type ${major} is not in this subset`);
      }
    }
  }
}
