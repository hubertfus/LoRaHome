/** 8-byte application-layer header. See ARCHITECTURE.md §3.1. */

export const MAGIC_VER = 0x4b; // 'K' — KNI protocol, version 1
export const HEADER_SIZE = 8;
export const CRC_SIZE = 2;

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
}

export enum FrameFlags {
  NONE = 0,
  ACK_REQ = 1 << 0,
  FRAG = 1 << 1,
  LAST = 1 << 2,
  ENCR = 1 << 3,
}

export const BROADCAST_ID = 0xffff;

export interface FrameHeader {
  type: FrameType;
  srcId: number;
  dstId: number;
  seq: number;
  flags: number;
}

export interface Frame extends FrameHeader {
  payload: Uint8Array;
}

export function encodeHeader(header: FrameHeader): Uint8Array {
  const buf = new Uint8Array(HEADER_SIZE);
  const view = new DataView(buf.buffer);
  view.setUint8(0, MAGIC_VER);
  view.setUint8(1, header.type);
  view.setUint16(2, header.srcId, false);
  view.setUint16(4, header.dstId, false);
  view.setUint8(6, header.seq);
  view.setUint8(7, header.flags);
  return buf;
}

export function decodeHeader(buf: Uint8Array): FrameHeader {
  if (buf.length < HEADER_SIZE) {
    throw new Error(`frame too short: expected >= ${HEADER_SIZE} bytes, got ${buf.length}`);
  }
  const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const magic = view.getUint8(0);
  if (magic !== MAGIC_VER) {
    throw new Error(`bad magic/version byte: 0x${magic.toString(16)}`);
  }
  return {
    type: view.getUint8(1),
    srcId: view.getUint16(2, false),
    dstId: view.getUint16(4, false),
    seq: view.getUint8(6),
    flags: view.getUint8(7),
  };
}

/** CRC16/CCITT-FALSE — must match the implementation in firmware/common/src/crc16.c. */
export function crc16(data: Uint8Array): number {
  let crc = 0xffff;
  for (const byte of data) {
    crc ^= byte << 8;
    for (let i = 0; i < 8; i++) {
      crc = (crc & 0x8000) !== 0 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

export function encodeFrame(header: FrameHeader, payload: Uint8Array): Uint8Array {
  const headerBytes = encodeHeader(header);
  const body = new Uint8Array(headerBytes.length + payload.length);
  body.set(headerBytes, 0);
  body.set(payload, headerBytes.length);

  const crc = crc16(body);
  const out = new Uint8Array(body.length + CRC_SIZE);
  out.set(body, 0);
  new DataView(out.buffer).setUint16(body.length, crc, false);
  return out;
}

export function decodeFrame(buf: Uint8Array): Frame {
  if (buf.length < HEADER_SIZE + CRC_SIZE) {
    throw new Error('frame too short to contain header + CRC');
  }
  const body = buf.subarray(0, buf.length - CRC_SIZE);
  const expectedCrc = new DataView(buf.buffer, buf.byteOffset).getUint16(buf.length - CRC_SIZE, false);
  const actualCrc = crc16(body);
  if (expectedCrc !== actualCrc) {
    throw new Error(`CRC mismatch: expected 0x${expectedCrc.toString(16)}, got 0x${actualCrc.toString(16)}`);
  }

  const header = decodeHeader(body);
  return { ...header, payload: body.subarray(HEADER_SIZE) };
}
