/**
 * Decoder for the Bridge's health readout — the twin of
 * firmware/common/src/bridge_stat.c.
 *
 * This is what makes the 24-hour soak able to report heap drift, which is the
 * measurement the roadmap makes a release depend on. Held against the C encoder
 * by `tools/check-bridge-stat.mjs`, which runs a value grid through both.
 *
 * Fixed offsets, big-endian, same byte order as the frame header. Not CBOR:
 * the Bridge does not interpret CBOR anywhere else (ARCHITECTURE.md §7) and a
 * diagnostic frame is not the place to introduce a decoder to it.
 */

/** Bumped whenever a field moves. A payload of another version is refused. */
export const BRIDGE_STAT_VERSION = 1;

/** Wire size of the payload. Well inside MAX_PAYLOAD (220). */
export const BRIDGE_STAT_SIZE = 82;

export interface BridgeStat {
  version: number;
  uptimeMs: number;

  /** §0.4 memory probe — the numbers that matter over a long run. */
  heapFreeInternal: number;
  /**
   * Fragmentation indicator, and every bit as important as the free total:
   * 180 kB free in pieces too small for a 4 kB buffer is a device that dies in
   * week three.
   */
  heapLargestBlock: number;
  heapMinFreeEver: number;

  serialFramesIn: number;
  serialFramesOut: number;
  radioFramesIn: number;
  radioFramesOut: number;
  rejectedCrc: number;
  rejectedMagic: number;
  rejectedLen: number;
  rejectedDutyCycle: number;
  radioTxErrors: number;
  serialTxErrors: number;

  slipFramesOk: number;
  slipOverflow: number;
  slipBadEscape: number;
  slipDropped: number;

  ringOverrun: number;
  /** Peak ring occupancy. Above ~60% of capacity the consumer is already behind. */
  ringHwm: number;
  ringCapacity: number;
}

/**
 * Parses a BRIDGE_STAT_RSP payload.
 *
 * Throws rather than returning null on a bad payload. A malformed health frame
 * during a soak is not a condition to shrug at — it means the two sides
 * disagree about the format, and every heap number after it would be fiction.
 */
export function decodeBridgeStat(payload: Uint8Array): BridgeStat {
  if (payload.length !== BRIDGE_STAT_SIZE) {
    throw new Error(`bridge stat payload must be ${BRIDGE_STAT_SIZE} B, got ${payload.length}`);
  }

  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const version = view.getUint16(0, false);
  if (version !== BRIDGE_STAT_VERSION) {
    throw new Error(`unsupported bridge stat version ${version} (expected ${BRIDGE_STAT_VERSION})`);
  }

  let at = 2;
  const u32 = (): number => {
    const value = view.getUint32(at, false);
    at += 4;
    return value;
  };
  const u16 = (): number => {
    const value = view.getUint16(at, false);
    at += 2;
    return value;
  };

  const stat: BridgeStat = {
    version,
    uptimeMs: u32(),
    heapFreeInternal: u32(),
    heapLargestBlock: u32(),
    heapMinFreeEver: u32(),
    serialFramesIn: u32(),
    serialFramesOut: u32(),
    radioFramesIn: u32(),
    radioFramesOut: u32(),
    rejectedCrc: u32(),
    rejectedMagic: u32(),
    rejectedLen: u32(),
    rejectedDutyCycle: u32(),
    radioTxErrors: u32(),
    serialTxErrors: u32(),
    slipFramesOk: u32(),
    slipOverflow: u32(),
    slipBadEscape: u32(),
    slipDropped: u32(),
    ringOverrun: u32(),
    ringHwm: u16(),
    ringCapacity: u16(),
  };

  /* The field list and the declared size must agree, or one side has been
   * edited without the other. */
  if (at !== BRIDGE_STAT_SIZE) {
    throw new Error(`bridge stat decoder read ${at} B, expected ${BRIDGE_STAT_SIZE}`);
  }
  return stat;
}

/** Encodes a payload. Used by the cross-language check and by test fixtures. */
export function encodeBridgeStat(stat: Omit<BridgeStat, 'version'>): Uint8Array {
  const out = new Uint8Array(BRIDGE_STAT_SIZE);
  const view = new DataView(out.buffer);

  view.setUint16(0, BRIDGE_STAT_VERSION, false);
  let at = 2;
  const u32 = (value: number): void => {
    view.setUint32(at, value >>> 0, false);
    at += 4;
  };
  const u16 = (value: number): void => {
    view.setUint16(at, value & 0xffff, false);
    at += 2;
  };

  u32(stat.uptimeMs);
  u32(stat.heapFreeInternal);
  u32(stat.heapLargestBlock);
  u32(stat.heapMinFreeEver);
  u32(stat.serialFramesIn);
  u32(stat.serialFramesOut);
  u32(stat.radioFramesIn);
  u32(stat.radioFramesOut);
  u32(stat.rejectedCrc);
  u32(stat.rejectedMagic);
  u32(stat.rejectedLen);
  u32(stat.rejectedDutyCycle);
  u32(stat.radioTxErrors);
  u32(stat.serialTxErrors);
  u32(stat.slipFramesOk);
  u32(stat.slipOverflow);
  u32(stat.slipBadEscape);
  u32(stat.slipDropped);
  u32(stat.ringOverrun);
  u16(stat.ringHwm);
  u16(stat.ringCapacity);

  return out;
}
