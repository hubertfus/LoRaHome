/**
 * Duplicate-suppression window — twin of firmware/common/src/dedup.c (T2.1).
 *
 * The Host needs this for the same reason the Node does. It receives
 * retransmissions too: an ACK that was slow rather than lost means the sender
 * sends the frame again, and telemetry counted twice is worse than telemetry
 * missed once, because nobody looks at a plausible number twice.
 *
 * Two implementations of one rule is the arrangement Etap 0 exists to prevent,
 * and as with SLIP it is unavoidable — one end runs on an ESP32, the other on
 * Node, and there is no artefact to generate both from. So they are held
 * together by evidence: tools/check-dedup-cross.mjs replays the same event
 * stream through both and requires identical verdicts, frame for frame.
 *
 * Keep the two structurally identical. Same bitmap orientation (bit n is
 * `lastSeq - n`), same modular comparison, same eviction rule — so a reviewer
 * can diff them by eye.
 */

/** Sequence numbers behind the newest that are still remembered. */
export const DEDUP_WINDOW = 32;

/** Senders tracked concurrently; the ninth evicts the least recently heard. */
export const DEDUP_PEERS = 8;

export interface DedupPeer {
  srcId: number;
  lastSeq: number;
  /** Bit n: sequence number `lastSeq - n` has been seen. Bit 0 is always set. */
  bitmap: number;
  lastSeenUs: number;
}

export interface DedupStats {
  dupesDropped: number;
  tooOld: number;
  peerEvicted: number;
  accepted: number;
}

/**
 * Modular distance between two sequence numbers.
 *
 * Positive means `seq` is newer. The `<< 24 >> 24` is JavaScript's int8 cast:
 * without it 0x00 would read as 255 frames older than 0xFF instead of one
 * frame newer, and the link would start rejecting every valid frame after the
 * 256th. That is risk R2.1, and it is the single line this whole file turns on.
 */
function seqDelta(seq: number, reference: number): number {
  return (((seq - reference) & 0xff) << 24) >> 24;
}

export class DedupWindow {
  private readonly peers: DedupPeer[] = [];

  readonly stats: DedupStats = { dupesDropped: 0, tooOld: 0, peerEvicted: 0, accepted: 0 };

  /**
   * Returns true for a frame that should be processed, false for a duplicate or
   * one older than the window. Not idempotent: calling it twice for the same
   * frame is the thing it exists to detect.
   *
   * `nowUs` orders peers for eviction only; entries never expire on their own.
   */
  checkAndMark(srcId: number, seq: number, nowUs: number): boolean {
    const peer = this.peers.find((candidate) => candidate.srcId === srcId);

    if (peer === undefined) {
      const claimed = this.claimPeer(srcId, nowUs);
      claimed.lastSeq = seq;
      claimed.bitmap = 1;
      this.stats.accepted++;
      return true;
    }

    peer.lastSeenUs = nowUs;
    const delta = seqDelta(seq, peer.lastSeq);

    if (delta > 0) {
      // A step of 32 or more leaves nothing of the old window; written out
      // rather than relying on a shift the width of the register.
      peer.bitmap = delta >= DEDUP_WINDOW ? 0 : (peer.bitmap << delta) >>> 0;
      peer.bitmap = (peer.bitmap | 1) >>> 0;
      peer.lastSeq = seq;
      this.stats.accepted++;
      return true;
    }

    const age = -delta;

    if (age >= DEDUP_WINDOW) {
      // Too far behind to know whether it was seen. Rejecting costs one frame;
      // accepting could cost correctness, and the sender's ARQ copes either way.
      this.stats.tooOld++;
      return false;
    }

    const mask = (1 << age) >>> 0;
    if ((peer.bitmap & mask) !== 0) {
      this.stats.dupesDropped++;
      return false;
    }

    peer.bitmap = (peer.bitmap | mask) >>> 0;
    this.stats.accepted++;
    return true;
  }

  /** Diagnostics; the returned object is live state, not a copy. */
  findPeer(srcId: number): DedupPeer | undefined {
    return this.peers.find((candidate) => candidate.srcId === srcId);
  }

  get peerCount(): number {
    return this.peers.length;
  }

  reset(): void {
    this.peers.length = 0;
    this.stats.dupesDropped = 0;
    this.stats.tooOld = 0;
    this.stats.peerEvicted = 0;
    this.stats.accepted = 0;
  }

  /**
   * Evicts the least recently heard sender when the table is full.
   *
   * LRU rather than refusing the newcomer: a node that has gone off the air
   * must not be able to lock out one that is on it. The count matters — past
   * the first eviction, dedup is a rotating cache rather than a guarantee.
   */
  private claimPeer(srcId: number, nowUs: number): DedupPeer {
    if (this.peers.length < DEDUP_PEERS) {
      const fresh: DedupPeer = { srcId, lastSeq: 0, bitmap: 0, lastSeenUs: nowUs };
      this.peers.push(fresh);
      return fresh;
    }

    let victim = this.peers[0]!;
    for (const candidate of this.peers) {
      if (candidate.lastSeenUs < victim.lastSeenUs) victim = candidate;
    }
    this.stats.peerEvicted++;

    victim.srcId = srcId;
    victim.lastSeq = 0;
    victim.bitmap = 0;
    victim.lastSeenUs = nowUs;
    return victim;
  }
}
