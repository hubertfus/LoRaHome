/**
 * Config fragmentation — twin of firmware/common/src/frag.c (T2.3).
 *
 * The Host is the sender in almost every config transaction, so this side owns
 * the split; the Node owns the reassembly. Both halves are implemented in both
 * languages anyway, because a splitter with no reassembler to answer it can
 * only be tested against its own assumptions, and because a Node's capability
 * report travels the other way.
 *
 * The rule this file enforces on behalf of the whole system: a config larger
 * than 1600 B is refused *here*, on a machine with a screen and a person in
 * front of it, and never sent. The alternative is discovering the limit on a
 * device on a roof, one fragment into a transaction that cannot complete.
 *
 * Keep it structurally identical to the C: same header order, same big-endian
 * encoding, same geometry checks, same result codes.
 */

import { crc16 } from './crc16.js';

/** Fragments per config: 8 * 200 B = 1600 B, the largest config we accept. */
export const FRAG_MAX_FRAGMENTS = 8;

/** Payload bytes per fragment; 12 B below what would fit, as slack. */
export const FRAG_PAYLOAD_MAX = 200;

export const FRAG_CONFIG_MAX = FRAG_MAX_FRAGMENTS * FRAG_PAYLOAD_MAX;

/** On-wire size of the fragment header that precedes each slice. */
export const FRAG_HDR_SIZE = 8;

/** How long an incomplete transaction holds the reassembly slot. */
export const FRAG_TIMEOUT_MS = 30000;

export interface FragHeader {
  cfgId: number;
  fragIndex: number;
  fragTotal: number;
  totalLen: number;
  /** CRC over the whole assembled config — not over this fragment. */
  crcTotal: number;
}

export function encodeFragHeader(hdr: FragHeader, out: Uint8Array, offset = 0): void {
  if (out.length - offset < FRAG_HDR_SIZE) {
    throw new RangeError(`need ${FRAG_HDR_SIZE} bytes at offset ${offset}`);
  }
  out[offset] = hdr.cfgId >>> 8;
  out[offset + 1] = hdr.cfgId & 0xff;
  out[offset + 2] = hdr.fragIndex;
  out[offset + 3] = hdr.fragTotal;
  out[offset + 4] = hdr.totalLen >>> 8;
  out[offset + 5] = hdr.totalLen & 0xff;
  out[offset + 6] = hdr.crcTotal >>> 8;
  out[offset + 7] = hdr.crcTotal & 0xff;
}

export function decodeFragHeader(buf: Uint8Array, offset = 0): FragHeader {
  if (buf.length - offset < FRAG_HDR_SIZE) {
    throw new Error(`fragment header truncated: ${buf.length - offset} B`);
  }
  return {
    cfgId: (buf[offset]! << 8) | buf[offset + 1]!,
    fragIndex: buf[offset + 2]!,
    fragTotal: buf[offset + 3]!,
    totalLen: (buf[offset + 4]! << 8) | buf[offset + 5]!,
    crcTotal: (buf[offset + 6]! << 8) | buf[offset + 7]!,
  };
}

/** Fragments a config of `totalLen` bytes needs, or 0 if it is too large. */
export function fragmentCount(totalLen: number): number {
  if (totalLen > FRAG_CONFIG_MAX) return 0;
  // An empty config is still one fragment: "nothing to configure" is a
  // statement, and a transaction of zero fragments could never complete.
  if (totalLen === 0) return 1;
  return Math.ceil(totalLen / FRAG_PAYLOAD_MAX);
}

/**
 * Splits a config into fragment payloads, ready to go into CONFIG_FRAG frames.
 *
 * Throws on an oversized config rather than returning an empty array. This is
 * the boundary where a too-large config must stop, and a silent empty result is
 * how a caller ends up "sending" a config that was never transmitted.
 */
export function splitConfig(cfgId: number, config: Uint8Array): Uint8Array[] {
  if (config.length > FRAG_CONFIG_MAX) {
    throw new RangeError(
      `config of ${config.length} B exceeds the ${FRAG_CONFIG_MAX} B maximum ` +
        `(${FRAG_MAX_FRAGMENTS} fragments of ${FRAG_PAYLOAD_MAX} B)`,
    );
  }

  const total = fragmentCount(config.length);
  const crcTotal = crc16(config);
  const fragments: Uint8Array[] = [];

  for (let index = 0; index < total; index++) {
    const offset = index * FRAG_PAYLOAD_MAX;
    const slice = config.subarray(offset, Math.min(offset + FRAG_PAYLOAD_MAX, config.length));
    const out = new Uint8Array(FRAG_HDR_SIZE + slice.length);
    encodeFragHeader(
      { cfgId, fragIndex: index, fragTotal: total, totalLen: config.length, crcTotal },
      out,
    );
    out.set(slice, FRAG_HDR_SIZE);
    fragments.push(out);
  }

  return fragments;
}

export enum FragResult {
  NEED_MORE = 0,
  COMPLETE = 1,
  DUPLICATE = 2,
  ERR_HEADER = -1,
  ERR_TOO_LARGE = -2,
  ERR_FOREIGN = -3,
  ERR_CRC = -4,
}

export interface FragStats {
  completed: number;
  timeouts: number;
  crcFail: number;
  duplicates: number;
  foreign: number;
}

/**
 * Reassembles fragments into a config.
 *
 * One transaction at a time, exactly like the C twin — the Node has room for
 * one 1600 B buffer and not two, and a Host that accepted several concurrently
 * would be testing a behaviour the device cannot have.
 */
export class Reassembler {
  private readonly buf = new Uint8Array(FRAG_CONFIG_MAX);
  private cfgId = 0;
  private receivedMask = 0;
  private expectedTotal = 0;
  private totalLen = 0;
  private crcExpected = 0;
  private firstFragUs = 0;
  private active = false;

  readonly stats: FragStats = {
    completed: 0,
    timeouts: 0,
    crcFail: 0,
    duplicates: 0,
    foreign: 0,
  };

  /** The assembled config, valid immediately after COMPLETE. A view, not a copy. */
  assembled(): Uint8Array {
    return this.buf.subarray(0, this.totalLen);
  }

  reset(): void {
    // Counters survive: they are the history of the session, and a transaction
    // ending is not a reason to forget how many timed out.
    this.cfgId = 0;
    this.receivedMask = 0;
    this.expectedTotal = 0;
    this.totalLen = 0;
    this.crcExpected = 0;
    this.firstFragUs = 0;
    this.active = false;
  }

  feed(payload: Uint8Array, nowUs: number): FragResult {
    if (payload.length < FRAG_HDR_SIZE) return FragResult.ERR_HEADER;

    const hdr = decodeFragHeader(payload);
    const sliceLen = payload.length - FRAG_HDR_SIZE;

    if (hdr.totalLen > FRAG_CONFIG_MAX) return FragResult.ERR_TOO_LARGE;
    if (!headerIsSane(hdr, sliceLen)) return FragResult.ERR_HEADER;

    // Expired here as well as in tick(), so a caller that never ticks still
    // recovers — on the next fragment rather than on the clock.
    if (this.active && nowUs - this.firstFragUs >= FRAG_TIMEOUT_MS * 1000) {
      this.stats.timeouts++;
      this.reset();
    }

    if (!this.active) {
      this.reset();
      this.active = true;
      this.cfgId = hdr.cfgId;
      this.expectedTotal = hdr.fragTotal;
      this.totalLen = hdr.totalLen;
      this.crcExpected = hdr.crcTotal;
      this.firstFragUs = nowUs;
    } else if (
      hdr.cfgId !== this.cfgId ||
      hdr.fragTotal !== this.expectedTotal ||
      hdr.totalLen !== this.totalLen ||
      hdr.crcTotal !== this.crcExpected
    ) {
      // Refused, and the transaction in progress is left untouched: a stray
      // fragment must not destroy a config that was one frame from done.
      this.stats.foreign++;
      return FragResult.ERR_FOREIGN;
    }

    const bit = 1 << hdr.fragIndex;
    if ((this.receivedMask & bit) !== 0) {
      this.stats.duplicates++;
      return FragResult.DUPLICATE;
    }

    this.buf.set(payload.subarray(FRAG_HDR_SIZE), hdr.fragIndex * FRAG_PAYLOAD_MAX);
    this.receivedMask |= bit;

    const completeMask = (1 << this.expectedTotal) - 1;
    if (this.receivedMask !== completeMask) return FragResult.NEED_MORE;

    const assembledLen = this.totalLen;
    const crcOk = crc16(this.buf.subarray(0, assembledLen)) === this.crcExpected;

    this.reset();
    this.totalLen = assembledLen; // the caller still needs the length

    if (!crcOk) {
      this.stats.crcFail++;
      return FragResult.ERR_CRC;
    }

    this.stats.completed++;
    return FragResult.COMPLETE;
  }

  /** Expires a transaction stalled past the timeout. True if one just expired. */
  tick(nowUs: number): boolean {
    if (!this.active) return false;
    if (nowUs - this.firstFragUs < FRAG_TIMEOUT_MS * 1000) return false;
    this.stats.timeouts++;
    this.reset();
    return true;
  }
}

/** Geometry a fragment header must satisfy before anything is stored. */
function headerIsSane(hdr: FragHeader, sliceLen: number): boolean {
  if (hdr.fragTotal === 0 || hdr.fragTotal > FRAG_MAX_FRAGMENTS) return false;
  if (hdr.fragIndex >= hdr.fragTotal) return false;
  if (hdr.totalLen > FRAG_CONFIG_MAX) return false;
  // A sender claiming 3 fragments for 100 bytes is broken or hostile; either
  // way its fragments must not be stored.
  if (fragmentCount(hdr.totalLen) !== hdr.fragTotal) return false;

  const offset = hdr.fragIndex * FRAG_PAYLOAD_MAX;
  if (offset > hdr.totalLen) return false;

  // The slice must be exactly the length its position implies. A short one
  // would leave stale bytes inside a config that then passes its CRC by luck.
  const expected = Math.min(hdr.totalLen - offset, FRAG_PAYLOAD_MAX);
  return sliceLen === expected;
}
