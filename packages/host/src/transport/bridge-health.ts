/**
 * Asks the Bridge how it is doing.
 *
 * Exists for one caller: the 24-hour soak, which needs `heap_free` and
 * `heap_largest_block` off a device with no display, no network stack and a
 * serial port carrying frames rather than a console. Without this the soak can
 * measure loss, latency and reboots but not the number the roadmap makes a
 * release depend on.
 */
import { BRIDGE_ID, FrameType, decodeFrame, encodeFrame } from '@lorahome/protocol';

import { decodeBridgeStat, type BridgeStat } from './bridge-stat.js';
import type { SerialTransport } from './serial.js';

export interface BridgeHealthClientOptions {
  /**
   * How long to wait for a reply.
   *
   * Short by design. The Bridge answers a local request without touching the
   * radio, so a slow answer means it is busy or wedged — and during a soak the
   * useful outcome is "no reading this cycle", not a stalled sampler.
   */
  timeoutMs?: number;
  /** Host's own address, echoed back by the Bridge. */
  hostId?: number;
}

export class BridgeHealthClient {
  private seq = 0;
  private readonly pending = new Map<number, (stat: BridgeStat) => void>();
  private readonly timeoutMs: number;
  private readonly hostId: number;

  constructor(
    private readonly transport: SerialTransport,
    options: BridgeHealthClientOptions = {},
  ) {
    this.timeoutMs = options.timeoutMs ?? 2000;
    this.hostId = options.hostId ?? 0x0001;

    this.transport.onFrame((frame) => this.handleFrame(frame));
  }

  private handleFrame(frame: Buffer): void {
    let decoded;
    try {
      decoded = decodeFrame(frame);
    } catch {
      // Not ours to complain about: the soak's own traffic comes through here
      // too, and a CRC failure is already counted by the transport.
      return;
    }
    if (decoded.type !== FrameType.BRIDGE_STAT_RSP) return;

    const resolve = this.pending.get(decoded.seq);
    if (resolve === undefined) return;
    this.pending.delete(decoded.seq);

    try {
      resolve(decodeBridgeStat(decoded.payload));
    } catch {
      // A malformed reply is worse than none — it means the two sides disagree
      // about the format. Dropping it lets the request time out, which the
      // caller already handles, rather than feeding fiction into heap drift.
    }
  }

  /**
   * Requests one readout. Resolves null on timeout rather than throwing: a
   * missed sample is a normal event over 17280 of them, and a soak that aborts
   * on the first one is a soak that never finishes.
   */
  async read(): Promise<BridgeStat | null> {
    // Wraps at 256 with the header's seq field. Two requests are never in
    // flight at once here, so a wrap cannot alias a live one.
    const seq = this.seq;
    this.seq = (this.seq + 1) & 0xff;

    const frame = encodeFrame(
      {
        type: FrameType.BRIDGE_STAT_REQ,
        srcId: this.hostId,
        dstId: BRIDGE_ID,
        seq,
        flags: 0,
      },
      new Uint8Array(0),
    );

    return new Promise<BridgeStat | null>((resolve) => {
      /*
       * Deliberately *not* unref'd.
       *
       * It was, on the reasoning that a health poll should never hold the
       * process open at the end of a run. But this timer is the only thing
       * keeping the event loop alive while a reply is outstanding, and an
       * unref'd one lets the loop drain with the promise still pending — Node
       * then reports "Promise resolution is still pending but the event loop
       * has already resolved" and the await never returns. It surfaced in CI as
       * three cancelled tests; on a developer machine the loop usually has
       * other work and the bug hides.
       *
       * Holding the process open is not a real cost either way: the timer is
       * cleared on every completion path below, so the longest it can outlive a
       * finished poll is zero.
       */
      const timer = setTimeout(() => {
        this.pending.delete(seq);
        resolve(null);
      }, this.timeoutMs);

      this.pending.set(seq, (stat) => {
        clearTimeout(timer);
        resolve(stat);
      });

      if (!this.transport.send(frame)) {
        clearTimeout(timer);
        this.pending.delete(seq);
        resolve(null);
      }
    });
  }

  /** Adapter matching the soak harness's `readBridgeHealth` hook. */
  readForSoak = async (): Promise<{
    heapFreeBytes: number;
    largestBlockBytes: number;
    uptimeSeconds: number;
  } | null> => {
    const stat = await this.read();
    if (stat === null) return null;
    return {
      heapFreeBytes: stat.heapFreeInternal,
      largestBlockBytes: stat.heapLargestBlock,
      uptimeSeconds: stat.uptimeMs / 1000,
    };
  };
}
