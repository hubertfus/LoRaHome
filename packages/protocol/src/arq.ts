/**
 * Stop-and-wait ARQ — twin of firmware/common/src/arq.c (T2.4).
 *
 * The Host is the sender for every config transaction, so this is not a mirror
 * kept for symmetry: it is the half that does the retrying in the normal case.
 * The Node's copy matters when a node has something it must get through.
 *
 * Keep the two structurally identical — same state names, same action codes,
 * same backoff formula, same rule that a duty-cycle deferral does not consume a
 * retry. tools/check-arq-cross.mjs replays one scripted event stream through
 * both and compares the decision, the state and the retry count at every step.
 *
 * The clock and the jitter source are injected for the same reason as in the C:
 * a retransmission schedule that can only be observed by waiting is a schedule
 * nobody tests, and R2.2 — the retransmission storm — is prevented by a
 * distribution, which is a thing you measure.
 */

import { LORA_MTU } from './frame.js';

export const ARQ_MAX_RETRIES = 5;
export const ARQ_BASE_TIMEOUT_MS = 2000;
export const ARQ_JITTER_MS = 500;

/** How long a duty-cycle-deferred retry waits before asking again. */
export const ARQ_DEFER_RETRY_MS = 250;

export enum ArqState {
  IDLE = 0,
  WAIT_ACK = 1,
  DONE = 2,
  FAILED = 3,
}

export enum ArqAction {
  NOTHING = 0,
  RETRANSMIT = 1,
  GAVE_UP = 2,
  DEFERRED = 3,
}

export interface ArqStats {
  sent: number;
  retries: number;
  acked: number;
  giveups: number;
  deferred: number;
  strayAcks: number;
  rttSumMs: number;
  rttCount: number;
}

export interface ArqOptions {
  /**
   * Jitter source, returning a uint32. Required.
   *
   * There is no default on purpose. A default would be seeded identically on
   * every device, which is not jitter — it is a scheduled collision.
   */
  random: () => number;
  /** Duty-cycle veto over retransmissions. Absent means always allowed. */
  allowTx?: (len: number) => boolean;
}

export class Arq {
  state = ArqState.IDLE;
  seq = 0;
  retryCount = 0;

  private pending: Uint8Array = new Uint8Array(0);
  private nextRetryUs = 0;
  private sentAtUs = 0;

  private readonly random: () => number;
  private allowTx: ((len: number) => boolean) | undefined;

  readonly stats: ArqStats = {
    sent: 0,
    retries: 0,
    acked: 0,
    giveups: 0,
    deferred: 0,
    strayAcks: 0,
    rttSumMs: 0,
    rttCount: 0,
  };

  constructor(options: ArqOptions) {
    this.random = options.random;
    this.allowTx = options.allowTx;
  }

  setDutyCycleGuard(allowTx: ((len: number) => boolean) | undefined): void {
    this.allowTx = allowTx;
  }

  /** Backoff for retransmission `retry`: BASE * 2^retry + jitter. */
  timeoutMs(retry: number): number {
    const capped = Math.min(retry, ARQ_MAX_RETRIES);
    return ARQ_BASE_TIMEOUT_MS * 2 ** capped + (this.random() >>> 0) % ARQ_JITTER_MS;
  }

  /**
   * Takes a frame for reliable delivery. False if one is already in flight —
   * stop-and-wait means exactly one, and silently replacing an unacknowledged
   * config is how a config goes missing with nobody retrying it.
   */
  send(seq: number, frame: Uint8Array, nowUs: number): boolean {
    if (this.state === ArqState.WAIT_ACK) return false;
    if (frame.length === 0 || frame.length > LORA_MTU) return false;

    this.pending = Uint8Array.from(frame);
    this.seq = seq;
    this.retryCount = 0;
    this.state = ArqState.WAIT_ACK;
    this.sentAtUs = nowUs;
    this.nextRetryUs = nowUs + this.timeoutMs(0) * 1000;
    this.stats.sent++;
    return true;
  }

  /**
   * Reports an ACK. True only for the one that completes the frame in flight.
   *
   * A duplicate ACK — the original was slow rather than lost — changes nothing.
   * Without that, a straggler would acknowledge whatever happens to be in
   * flight now, and the config it "delivered" would be one nobody ever retries.
   */
  onAck(seq: number, nowUs: number): boolean {
    if (this.state !== ArqState.WAIT_ACK || seq !== this.seq) {
      this.stats.strayAcks++;
      return false;
    }

    const rttUs = nowUs - this.sentAtUs;
    if (rttUs > 0) {
      this.stats.rttSumMs += Math.floor(rttUs / 1000);
      this.stats.rttCount++;
    }

    this.stats.acked++;
    this.state = ArqState.DONE;
    return true;
  }

  /** Advances the timer. `frame` is only meaningful on RETRANSMIT. */
  tick(nowUs: number): { action: ArqAction; frame?: Uint8Array } {
    if (this.state !== ArqState.WAIT_ACK) return { action: ArqAction.NOTHING };
    if (nowUs < this.nextRetryUs) return { action: ArqAction.NOTHING };

    if (this.retryCount >= ARQ_MAX_RETRIES) {
      this.state = ArqState.FAILED;
      this.stats.giveups++;
      return { action: ArqAction.GAVE_UP };
    }

    // R2.5: asked before the retry counter moves, so a busy hour cannot exhaust
    // the five attempts without a single frame going on air.
    if (this.allowTx !== undefined && !this.allowTx(this.pending.length)) {
      this.nextRetryUs = nowUs + ARQ_DEFER_RETRY_MS * 1000;
      this.stats.deferred++;
      return { action: ArqAction.DEFERRED };
    }

    this.retryCount++;
    this.stats.retries++;
    this.nextRetryUs = nowUs + this.timeoutMs(this.retryCount) * 1000;
    return { action: ArqAction.RETRANSMIT, frame: this.pending };
  }

  /** Abandons whatever is in flight. Counters are kept. */
  reset(): void {
    this.state = ArqState.IDLE;
    this.seq = 0;
    this.retryCount = 0;
    this.pending = new Uint8Array(0);
    this.nextRetryUs = 0;
    this.sentAtUs = 0;
  }

  /** Mean round trip over acknowledged frames, in ms. Includes retries. */
  meanRttMs(): number {
    if (this.stats.rttCount === 0) return 0;
    return Math.floor(this.stats.rttSumMs / this.stats.rttCount);
  }
}
