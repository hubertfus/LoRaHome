/**
 * Canonical definition of the 8-byte on-air header. See ARCHITECTURE.md §3.1.
 *
 * `HEADER_LAYOUT` below is the single source of truth for the wire format. The
 * TypeScript codec in this file is built from it at module load, and the C
 * header (`firmware/common/include/lorahome/protocol_generated.h`) is emitted
 * from it by `pnpm gen:c`. Nothing in either language may hardcode an offset.
 *
 * The failure mode this exists to prevent: someone moves a field in the .h,
 * forgets the .ts, and we spend three weeks chasing "random" CRC errors on the
 * radio. Change the layout here and both sides move together, or neither does.
 */

import { crc16 } from './crc16.js';

export const FRAME_MAGIC = 0x4b; // 'K' — magic + version nibble
export const HEADER_SIZE = 8;
export const CRC_SIZE = 2;

/** Largest single LoRa payload we will put on air at SF9/CR4-5. */
export const LORA_MTU = 230;
/** Payload room left once the header and trailing CRC are accounted for. */
export const MAX_PAYLOAD = LORA_MTU - HEADER_SIZE - CRC_SIZE; // 220
/** A frame carrying no payload is still header + CRC. Anything shorter is junk. */
export const MIN_FRAME_SIZE = HEADER_SIZE + CRC_SIZE; // 10

/** @deprecated Use FRAME_MAGIC. Retained so existing imports keep compiling. */
export const MAGIC_VER = FRAME_MAGIC;

export const HEADER_LAYOUT = [
  { name: 'magic_ver', offset: 0, size: 1, endian: 'u8' },
  { name: 'type', offset: 1, size: 1, endian: 'u8' },
  { name: 'src_id', offset: 2, size: 2, endian: 'be16' },
  { name: 'dst_id', offset: 4, size: 2, endian: 'be16' },
  { name: 'seq', offset: 6, size: 1, endian: 'u8' },
  { name: 'flags', offset: 7, size: 1, endian: 'u8' },
] as const;

export type HeaderEndian = (typeof HEADER_LAYOUT)[number]['endian'];
export type HeaderFieldName = (typeof HEADER_LAYOUT)[number]['name'];

export enum FrameType {
  BEACON = 0x01,
  JOIN_REQ = 0x02,
  JOIN_ACK = 0x03,
  CONFIG_BEGIN = 0x10,
  CONFIG_FRAG = 0x11,
  CONFIG_COMMIT = 0x12,
  CONFIG_ACK = 0x13,
  TELEMETRY = 0x20,
  EVENT = 0x21,
  CMD = 0x30,
  CMD_ACK = 0x31,
  CAPABILITY_REQ = 0x40,
  CAPABILITY_RSP = 0x41,
  /**
   * Bridge diagnostics, addressed to `BRIDGE_ID` and never put on air.
   *
   * The Bridge is the one component in the system whose memory nobody can see.
   * It has no display, no network stack, and its serial port carries frames
   * rather than a console — so a leak in it is invisible until the device stops
   * responding some weeks later. These two types are how the 24-hour soak reads
   * `heap_free` and `heap_largest_block` off it, which is the measurement the
   * roadmap makes a release depend on.
   */
  BRIDGE_STAT_REQ = 0x50,
  BRIDGE_STAT_RSP = 0x51,
}

export enum FrameFlags {
  NONE = 0,
  ACK_REQ = 1 << 0,
  FRAG = 1 << 1,
  LAST = 1 << 2,
  ENCR = 1 << 3,
}

export const BROADCAST_ID = 0xffff;

/**
 * Addresses the Bridge itself rather than anything beyond it.
 *
 * A frame carrying this destination is answered locally and never transmitted.
 * 0x0000 is reserved for it and is not a valid node id — the alternative, a
 * magic node id somewhere in the normal range, would eventually collide with a
 * real device and send diagnostics into the air.
 */
export const BRIDGE_ID = 0x0000;

export interface FrameHeader {
  /**
   * Normally one of `FrameType`, but typed as `number` on purpose: a decoded
   * frame can carry any byte here. Rejecting unknown types is a routing
   * decision made upstack, not a parse error — pretending otherwise would make
   * the enum a lie about what came off the air.
   */
  type: number;
  srcId: number;
  dstId: number;
  seq: number;
  flags: number;
}

export interface Frame extends FrameHeader {
  payload: Uint8Array;
}

/**
 * Wire field name -> property on `FrameHeader`. `null` means the field is a
 * protocol constant with no user-supplied value.
 *
 * Typed as a total `Record` over the layout's field names so that adding a
 * field to `HEADER_LAYOUT` without mapping it here is a compile error rather
 * than a field that silently encodes as zero.
 */
const FIELD_TO_PROP: Record<HeaderFieldName, keyof FrameHeader | null> = {
  magic_ver: null,
  type: 'type',
  src_id: 'srcId',
  dst_id: 'dstId',
  seq: 'seq',
  flags: 'flags',
};

type Writer = (buf: Uint8Array, offset: number, value: number) => void;
type Reader = (buf: Uint8Array, offset: number) => number;

// Multi-byte fields are big-endian on the wire (network order) while both the
// ESP32 and x86 hosts are little-endian, so the conversion is explicit here and
// in the generated C accessors. Never read hdr.src_id directly. See risk R0.2.
//
// Deliberately plain byte arithmetic rather than DataView: a DataView is a
// fresh heap object on every call, which cost ~230 B/op and pushed encode to
// ~330 ns/op — over both T0.2 budgets. Indexing the Uint8Array directly leaves
// one allocation (the buffer itself) and makes the byte order legible at the
// point it happens. Callers guarantee offset+size <= buf.length before calling.
const WRITERS: Record<HeaderEndian, Writer> = {
  u8: (buf, offset, value) => {
    buf[offset] = value;
  },
  be16: (buf, offset, value) => {
    buf[offset] = value >>> 8;
    buf[offset + 1] = value & 0xff;
  },
};

const READERS: Record<HeaderEndian, Reader> = {
  u8: (buf, offset) => buf[offset]!,
  be16: (buf, offset) => (buf[offset]! << 8) | buf[offset + 1]!,
};

interface CodecStep {
  readonly wireName: HeaderFieldName;
  readonly prop: keyof FrameHeader | null;
  readonly offset: number;
  readonly max: number;
  readonly write: Writer;
  readonly read: Reader;
}

/** The codec, derived from the layout once at module load rather than per call. */
const CODEC: readonly CodecStep[] = HEADER_LAYOUT.map((field) => ({
  wireName: field.name,
  prop: FIELD_TO_PROP[field.name],
  offset: field.offset,
  max: field.size === 1 ? 0xff : 0xffff,
  write: WRITERS[field.endian],
  read: READERS[field.endian],
}));

/**
 * Writes the header into `buf` at offset 0. `buf` must hold at least
 * HEADER_SIZE bytes.
 *
 * Range-checks every field. Assigning to a Uint8Array element truncates
 * silently, so an out-of-range node id would otherwise go on air as a
 * different, valid-looking id — the kind of bug that surfaces as "node 0x0102
 * sometimes answers as 0x02".
 */
function writeHeaderInto(header: FrameHeader, buf: Uint8Array): void {
  for (const step of CODEC) {
    const value = step.prop === null ? FRAME_MAGIC : header[step.prop];
    if (!Number.isInteger(value) || value < 0 || value > step.max) {
      throw new RangeError(
        `header field ${step.wireName} must be an integer in 0..${step.max}, got ${value}`,
      );
    }
    step.write(buf, step.offset, value);
  }
}

/**
 * Zero-allocation encode: writes the header into a caller-owned buffer.
 *
 * This is the hot path and the one the zero-copy doctrine actually wants — the
 * caller owns the buffer, we never take ownership of it. `encodeHeader` below
 * is the convenience wrapper for tests and one-off call sites.
 *
 * Worth knowing before "optimising" the wrapper: on V8 a bare
 * `new Uint8Array(8)` costs ~227 B of heap (typed-array object + ArrayBuffer +
 * backing store), regardless of the 8 payload bytes. No allocating API can get
 * under that, so a caller in a loop should reuse one buffer and call this.
 */
export function encodeHeaderInto(header: FrameHeader, buf: Uint8Array, offset = 0): void {
  if (offset < 0 || buf.length - offset < HEADER_SIZE) {
    throw new RangeError(
      `need ${HEADER_SIZE} bytes at offset ${offset}, buffer holds ${buf.length}`,
    );
  }
  writeHeaderInto(header, offset === 0 ? buf : buf.subarray(offset));
}

export function encodeHeader(header: FrameHeader): Uint8Array {
  const buf = new Uint8Array(HEADER_SIZE);
  writeHeaderInto(header, buf);
  return buf;
}

export function decodeHeader(buf: Uint8Array): FrameHeader {
  if (buf.length < HEADER_SIZE) {
    throw new Error(`frame too short: expected >= ${HEADER_SIZE} bytes, got ${buf.length}`);
  }

  // All-number scratch so the layout-driven loop can assign by property name;
  // the single cast on return is the only place types are asserted.
  const out: { [K in keyof FrameHeader]: number } = {
    type: 0,
    srcId: 0,
    dstId: 0,
    seq: 0,
    flags: 0,
  };

  for (const step of CODEC) {
    const value = step.read(buf, step.offset);
    if (step.prop === null) {
      if (value !== FRAME_MAGIC) {
        throw new Error(`bad magic/version byte: 0x${value.toString(16)}`);
      }
    } else {
      out[step.prop] = value;
    }
  }
  return out;
}

// crc16 lives in ./crc16.ts, paired with firmware/common/src/crc16.c. It is not
// re-exported here: `index.ts` exports both modules, and a name exported twice
// is ambiguous under `export *`.

/**
 * Builds header + payload + CRC into a single freshly allocated buffer.
 *
 * One allocation, not three: the header is written in place and the CRC is
 * computed over a subarray view rather than a concatenated copy. On the host
 * this is only a benchmark number, but the same structure is what lets the
 * firmware build frames straight into its static TX buffer with no scratch.
 */
export function encodeFrame(header: FrameHeader, payload: Uint8Array): Uint8Array {
  if (payload.length > MAX_PAYLOAD) {
    throw new RangeError(`payload of ${payload.length} B exceeds MAX_PAYLOAD (${MAX_PAYLOAD} B)`);
  }
  const bodyLen = HEADER_SIZE + payload.length;
  const out = new Uint8Array(bodyLen + CRC_SIZE);

  writeHeaderInto(header, out);
  out.set(payload, HEADER_SIZE);
  const crc = crc16(out.subarray(0, bodyLen));
  out[bodyLen] = crc >>> 8;
  out[bodyLen + 1] = crc & 0xff;
  return out;
}

/**
 * Validates and decodes a full frame.
 *
 * The returned payload is a view into `buf`, not a copy — the zero-copy rule
 * from ARCHITECTURE.md applies on the host too, so callers must not retain it
 * past the life of the receive buffer.
 */
export function decodeFrame(buf: Uint8Array): Frame {
  if (buf.length < MIN_FRAME_SIZE) {
    throw new Error(`frame too short: expected >= ${MIN_FRAME_SIZE} bytes, got ${buf.length}`);
  }
  if (buf.length > LORA_MTU) {
    throw new Error(`frame too long: ${buf.length} B exceeds LORA_MTU (${LORA_MTU} B)`);
  }

  const bodyLen = buf.length - CRC_SIZE;
  const expectedCrc = (buf[bodyLen]! << 8) | buf[bodyLen + 1]!;
  const actualCrc = crc16(buf.subarray(0, bodyLen));
  if (expectedCrc !== actualCrc) {
    throw new Error(
      `CRC mismatch: expected 0x${expectedCrc.toString(16)}, got 0x${actualCrc.toString(16)}`,
    );
  }

  const header = decodeHeader(buf);
  return { ...header, payload: buf.subarray(HEADER_SIZE, bodyLen) };
}
