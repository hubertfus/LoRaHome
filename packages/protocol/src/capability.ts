/**
 * Capability discovery. Twin of firmware/common/src/capability.c (T3.5).
 *
 * The Host asks with a 0x40 frame; the node answers with 0x41 carrying this
 * report. It inverts the usual flow — rather than declaring a sensor in a
 * configuration file and hoping it is wired correctly, you plug the hardware in
 * and the system tells you what it found (ARCHITECTURE.md §8).
 *
 * The report must fit one frame. Fragmenting a discovery response would make
 * discovery depend on reassembly, which depends on an ARQ — a lot of machinery
 * to answer "what are you?", and all of it is what you would be debugging when
 * a new node fails to appear.
 */

import { CborReader, CborWriter } from './cbor.js';

/** Components in a report. The node's own limit, so a report is always complete. */
export const CAP_MAX = 8;

/**
 * Wire budget for a realistic full report. Checked in the tests, not assumed.
 *
 * Applies to eight components with the field values the protocol actually
 * produces: bus types from a five-value enum, channel counts in single digits,
 * one flag bit defined. It is what constrains adding drivers.
 */
export const CAP_WIRE_BUDGET = 100;

/**
 * The largest report this codec can produce.
 *
 * 12 B per component (1 array header, 3 for a 16-bit type id, 2 each for four
 * bytes that spill past the inline form) plus 13 B of envelope. Still fits one
 * frame with more than half the payload spare, which is the requirement the
 * budget above exists to express.
 */
export const CAP_WIRE_WORST_CASE = 109;

export enum BusType {
  I2C = 0,
  GPIO = 1,
  SPI = 2,
  OneWire = 3,
  ADC = 4,
}

/** bit 0: this component already has a configuration on the node. */
export const CAP_FLAG_CONFIGURED = 0x01;

/** Map keys. Integers, never strings — CONTRIBUTING.md §3. */
export const CapKey = {
  FwVersion: 1,
  FreeHeapKb: 2,
  Components: 3,
} as const;

/** Positions inside one component's array. */
export const CAP_FIELDS = 5;

export interface Capability {
  driverTypeId: number;
  busAddr: number;
  busType: BusType;
  channelCount: number;
  flags: number;
}

export interface CapabilityReport {
  fwVersion: number;
  freeHeapKb: number;
  components: Capability[];
}

/**
 * Encodes a report.
 *
 * Each component is a positional array rather than a map. A map per component
 * would cost five key bytes each — forty bytes over eight components, against a
 * hundred-byte budget — to describe a structure the protocol fixes and which
 * could not be reordered anyway. The outer report is a map because it *will*
 * gain keys; the components array is exactly where it must not.
 */
export function encodeCapabilityReport(report: CapabilityReport): Uint8Array {
  if (report.components.length > CAP_MAX) {
    throw new RangeError(`a report holds at most ${CAP_MAX} components`);
  }

  const writer = new CborWriter();
  writer.map(3);

  writer.uint(CapKey.FwVersion).uint(report.fwVersion);
  writer.uint(CapKey.FreeHeapKb).uint(report.freeHeapKb);

  writer.uint(CapKey.Components).array(report.components.length);
  for (const component of report.components) {
    writer.array(CAP_FIELDS);
    writer.uint(component.driverTypeId);
    writer.uint(component.busAddr);
    writer.uint(component.busType);
    writer.uint(component.channelCount);
    writer.uint(component.flags);
  }

  return writer.finish();
}

/**
 * Decodes a report.
 *
 * Unknown map keys are skipped rather than refused, so a host can still
 * discover a node running firmware it does not recognise. A component array
 * with more fields than this build knows is truncated the same way, for the
 * same reason. Throws only for a message that is malformed or exceeds the
 * receiver's limits.
 */
export function decodeCapabilityReport(buf: Uint8Array): CapabilityReport {
  const reader = new CborReader(buf);
  const report: CapabilityReport = { fwVersion: 0, freeHeapKb: 0, components: [] };

  const pairs = reader.map();
  for (let pair = 0; pair < pairs; pair++) {
    const key = reader.uint();

    switch (key) {
      case CapKey.FwVersion:
        report.fwVersion = reader.uint();
        break;
      case CapKey.FreeHeapKb:
        report.freeHeapKb = reader.uint();
        break;
      case CapKey.Components: {
        const count = reader.array();
        // More components than this build can hold is a protocol mismatch, not
        // something to truncate: a host acting on the first eight of twelve
        // would show a device with parts missing and no sign anything was lost.
        if (count > CAP_MAX) throw new RangeError(`report claims ${count} components`);
        for (let i = 0; i < count; i++) report.components.push(decodeComponent(reader));
        break;
      }
      default:
        // Forward compatibility: a key from a newer protocol is stepped over,
        // so adding one does not require flashing every node on the same day.
        reader.skip();
        break;
    }
  }

  return report;
}

function decodeComponent(reader: CborReader): Capability {
  const fields = reader.array();
  if (fields < CAP_FIELDS) throw new RangeError(`component has only ${fields} fields`);

  const component: Capability = {
    driverTypeId: reader.uint(),
    busAddr: reader.uint(),
    busType: reader.uint() as BusType,
    channelCount: reader.uint(),
    flags: reader.uint(),
  };

  // A newer node may describe a component with more fields than this build
  // knows. Skipping them lets an old host still discover it, rather than
  // reporting a device it can see as unreadable.
  for (let i = CAP_FIELDS; i < fields; i++) reader.skip();

  return component;
}

/** True if two reports carry identical contents. For tests and diagnostics. */
export function capabilityReportsEqual(a: CapabilityReport, b: CapabilityReport): boolean {
  if (a.fwVersion !== b.fwVersion) return false;
  if (a.freeHeapKb !== b.freeHeapKb) return false;
  if (a.components.length !== b.components.length) return false;

  return a.components.every((component, index) => {
    const other = b.components[index]!;
    return (
      component.driverTypeId === other.driverTypeId &&
      component.busAddr === other.busAddr &&
      component.busType === other.busType &&
      component.channelCount === other.channelCount &&
      component.flags === other.flags
    );
  });
}
