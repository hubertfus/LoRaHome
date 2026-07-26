/**
 * The reliability regression suite. Roadmap T2.6.
 *
 * Everything Etap 2 built, wired together and run against a link that is trying
 * to break it: frames are built and CRC'd (T2.2), delivered by a stop-and-wait
 * ARQ (T2.4), damaged by a seeded simulator (T2.5), validated and filtered
 * through the duplicate window (T2.1) at the far end.
 *
 * The suite exists for one number. `doubleProcessed` must be zero — not small,
 * zero. Everything else here is a rate that can be argued about; that one is an
 * invariant, and it is the difference between a system that occasionally loses
 * a reading and one that occasionally opens a valve twice.
 *
 * Frames carry a 32-bit id in their payload, which the protocol knows nothing
 * about. That is what makes "processed twice" observable at all: the 8-bit
 * sequence number wraps every 256 frames, so across 100k frames it cannot
 * identify anything, and the receiver's own opinion of what is a duplicate is
 * exactly the thing under test — it cannot also be the oracle.
 */
import {
  Arq,
  ArqAction,
  DedupWindow,
  decodeFrame,
  encodeFrame,
  FrameType,
} from '@lorahome/protocol';

import {
  LINK_PROFILES,
  SimLink,
  type LinkProfile,
  type LinkProfileName,
} from '../transport/sim-link.js';

export interface ChaosOptions {
  profile: LinkProfileName;
  frames: number;
  seed: number;
  /**
   * Apply the profile's impairments to the return path too.
   *
   * True is the honest model of a radio link: a lost ACK is indistinguishable
   * from a lost frame at the sender and costs the same retransmission. False
   * matches the model under which delivery figures are usually quoted, this
   * project's roadmap included, and exists so the two can be compared without
   * either pretending to be the other.
   */
  lossyAcks?: boolean;
  /** Payload bytes after the 4-byte frame id. */
  payloadBytes?: number;
  /**
   * Extra impairments on top of the named profile.
   *
   * The five profiles are the regression set and stay fixed — a suite whose
   * meaning drifts is a suite whose history means nothing. This is for the
   * targeted case a profile does not cover, such as duplicating every frame to
   * put the duplicate window under direct load rather than waiting for
   * retransmissions to produce a few by chance.
   */
  overrides?: Omit<LinkProfile, 'seed'>;
}

export interface ChaosResult {
  profile: LinkProfileName;
  frames: number;
  seed: number;
  lossyAcks: boolean;
  /** Frames the receiver processed at least once. */
  delivered: number;
  /** THE invariant. Any value but 0 is a failed run. */
  doubleProcessed: number;
  /** Frames that arrived damaged and were refused by the CRC. */
  crcRejects: number;
  /** Retransmissions the ARQ issued. */
  retries: number;
  /** Transfers abandoned after the retry limit. */
  giveUps: number;
  /** Arrivals the duplicate window refused. */
  dupesDropped: number;
  runtimeMs: number;
}

const SRC_ID = 0x0101;
const DST_ID = 0x0201;

/** How far the virtual clock moves per polling step, in ms. */
const STEP_MS = 100;

/**
 * Steps allowed per transfer before the harness declares it stuck.
 *
 * Generous: five retransmissions with the last timeout at 64 s plus jitter is
 * around two minutes of virtual time. The cap is a guard against a hang in the
 * code under test, not a delivery policy — a transfer that hits it is a bug
 * report, not a lost frame, so it is asserted on rather than counted.
 */
const MAX_STEPS_PER_TRANSFER = 4000;

export function runChaos(options: ChaosOptions): ChaosResult {
  const { profile, frames, seed } = options;
  const lossyAcks = options.lossyAcks ?? true;
  const payloadBytes = options.payloadBytes ?? 24;

  const linkProfile: LinkProfile = {
    seed,
    ...LINK_PROFILES[profile],
    ...(lossyAcks ? {} : { impairDirection: 'atob' as const }),
    ...(options.overrides ?? {}),
  };

  const link = new SimLink(linkProfile);

  // The ARQ's jitter source is seeded from the same seed, one step away from
  // the link's, so a run is reproducible end to end rather than only halfway.
  let arqRng = (seed ^ 0x9e3779b9) >>> 0;
  const arq = new Arq({
    random: () => {
      arqRng ^= arqRng << 13;
      arqRng ^= arqRng >>> 17;
      arqRng ^= arqRng << 5;
      return arqRng >>> 0;
    },
  });

  const dedup = new DedupWindow();
  const seen = new Uint8Array(frames);

  const result: ChaosResult = {
    profile,
    frames,
    seed,
    lossyAcks,
    delivered: 0,
    doubleProcessed: 0,
    crcRejects: 0,
    retries: 0,
    giveUps: 0,
    dupesDropped: 0,
    runtimeMs: 0,
  };

  // --- the receiving end ----------------------------------------------------

  link.b.onFrame((wire) => {
    let frame;
    try {
      frame = decodeFrame(wire);
    } catch {
      // A damaged frame. This is the CRC doing its job, and the sender will
      // find out by not getting an ACK — there is deliberately no NACK, which
      // would spend airtime telling a peer something its own timeout will
      // establish for free.
      result.crcRejects++;
      return;
    }

    // The ACK goes out whether or not the frame is new. A duplicate means the
    // *previous* ACK was lost, and staying silent would guarantee another
    // retransmission of a frame that has already been processed.
    const ack = encodeFrame(
      { type: FrameType.CONFIG_ACK, srcId: DST_ID, dstId: SRC_ID, seq: frame.seq, flags: 0 },
      new Uint8Array(0),
    );
    link.b.send(ack);

    if (!dedup.checkAndMark(frame.srcId, frame.seq, link.nowUs)) return;

    const id =
      ((frame.payload[0]! << 24) | (frame.payload[1]! << 16) | (frame.payload[2]! << 8) |
        frame.payload[3]!) >>>
      0;
    if (id >= frames) return; // corruption that survived the CRC; not our frame

    if (seen[id] === 1) result.doubleProcessed++;
    else result.delivered++;
    seen[id] = 1;
  });

  // --- the sending end ------------------------------------------------------

  let acked = false;
  link.a.onFrame((wire) => {
    let frame;
    try {
      frame = decodeFrame(wire);
    } catch {
      result.crcRejects++;
      return;
    }
    if (frame.type !== FrameType.CONFIG_ACK) return;
    if (arq.onAck(frame.seq, link.nowUs)) acked = true;
  });

  const started = process.hrtime.bigint();
  const payload = new Uint8Array(4 + payloadBytes);

  for (let id = 0; id < frames; id++) {
    payload[0] = (id >>> 24) & 0xff;
    payload[1] = (id >>> 16) & 0xff;
    payload[2] = (id >>> 8) & 0xff;
    payload[3] = id & 0xff;
    for (let i = 0; i < payloadBytes; i++) payload[4 + i] = (id + i) & 0xff;

    const seq = id & 0xff;
    const wire = encodeFrame(
      { type: FrameType.TELEMETRY, srcId: SRC_ID, dstId: DST_ID, seq, flags: 0 },
      payload,
    );

    acked = false;
    arq.reset();
    arq.send(seq, wire, link.nowUs);
    link.a.send(wire);

    let steps = 0;
    while (!acked && steps < MAX_STEPS_PER_TRANSFER) {
      steps++;
      link.advance(STEP_MS);
      if (acked) break;

      const { action, frame: retransmit } = arq.tick(link.nowUs);
      if (action === ArqAction.RETRANSMIT && retransmit !== undefined) {
        result.retries++;
        link.a.send(retransmit);
      } else if (action === ArqAction.GAVE_UP) {
        result.giveUps++;
        break;
      }
    }

    if (steps >= MAX_STEPS_PER_TRANSFER) {
      throw new Error(
        `transfer ${id} on profile ${profile} did not terminate in ${MAX_STEPS_PER_TRANSFER} steps ` +
          `(seed 0x${seed.toString(16)}) — this is a hang, not a lost frame`,
      );
    }

    // Anything still in flight belongs to a transfer that is over: a late ACK,
    // or a retransmission whose ACK arrived first. Draining keeps the queue
    // from growing across 100k transfers, and delivers those stragglers into
    // the next transfer's window — which is precisely the situation the
    // duplicate filter and the stale-ACK rule exist for.
    link.advance(STEP_MS);
  }

  result.runtimeMs = Number(process.hrtime.bigint() - started) / 1e6;
  result.dupesDropped = dedup.stats.dupesDropped;
  return result;
}

/** Delivery as a percentage, at the precision the budgets are written in. */
export function deliveryPct(result: ChaosResult): number {
  return (100 * result.delivered) / result.frames;
}
