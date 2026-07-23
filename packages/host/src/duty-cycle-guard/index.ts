import { computeAirtimeMs } from './airtime.js';

export { computeAirtimeMs };
export type { AirtimeParams } from './airtime.js';

interface Transmission {
  atMs: number;
  durationMs: number;
}

/**
 * Enforces the ETSI EN 300 220 duty cycle limit (default 1%) over a rolling
 * window (default 1 hour) for a single 868MHz sub-band. See ARCHITECTURE.md
 * §7 — the Bridge enforces the same limit independently as a second line of
 * defense.
 */
export class DutyCycleGuard {
  private transmissions: Transmission[] = [];

  constructor(
    private readonly limitFraction = 0.01,
    private readonly windowMs = 60 * 60 * 1000,
  ) {}

  private prune(nowMs: number): void {
    const cutoff = nowMs - this.windowMs;
    this.transmissions = this.transmissions.filter((t) => t.atMs > cutoff);
  }

  usedMs(nowMs = Date.now()): number {
    this.prune(nowMs);
    return this.transmissions.reduce((sum, t) => sum + t.durationMs, 0);
  }

  usedFraction(nowMs = Date.now()): number {
    return this.usedMs(nowMs) / this.windowMs;
  }

  /** Returns true if transmitting a frame of `durationMs` would exceed the duty cycle limit. */
  wouldExceed(durationMs: number, nowMs = Date.now()): boolean {
    return this.usedMs(nowMs) + durationMs > this.limitFraction * this.windowMs;
  }

  /**
   * Attempts to record a transmission. Returns false (and records nothing)
   * if it would exceed the duty cycle limit — callers must not transmit in
   * that case.
   */
  tryRecord(durationMs: number, nowMs = Date.now()): boolean {
    if (this.wouldExceed(durationMs, nowMs)) return false;
    this.transmissions.push({ atMs: nowMs, durationMs });
    return true;
  }
}
