/**
 * An in-process stand-in for the bridge pair, for exercising the soak harness
 * without hardware.
 *
 * This does not simulate a radio and makes no attempt to. It exists so the
 * harness's own arithmetic — loss accounting, late arrivals, latency
 * percentiles, heap drift, reboot detection — is tested, because that
 * arithmetic is what has to be right unattended at 3 a.m. on the second day,
 * and a harness that only runs with two boards attached is one whose bugs are
 * discovered by a wasted 24-hour run.
 *
 * Loss, latency and reboots are injectable so the accounting can be checked
 * against a known answer rather than against whatever the link happened to do.
 */
import type { BridgeHealth, SoakTransport } from './link-soak.js';

export interface LoopbackOptions {
  /** Fraction of frames to swallow, 0..1. */
  lossRate?: number;
  /** Round-trip delay applied before echoing. */
  latencyMs?: number;
  /** Extra delay applied to a fraction of frames, to give the tail some shape. */
  jitterMs?: number;
  /** Bytes of free heap "lost" per frame — a leak to be detected, or 0. */
  leakBytesPerFrame?: number;
  /** Simulated starting free heap on the far side. */
  heapFreeBytes?: number;
  largestBlockBytes?: number;
  /** Deterministic seed, so a failing test reproduces. */
  seed?: number;
  /** Injectable for tests that drive their own clock. */
  schedule?: (fn: () => void, ms: number) => void;
  now?: () => number;
}

export class LoopbackLink implements SoakTransport {
  private readonly listeners: ((frame: Buffer) => void)[] = [];
  private rng: number;
  private framesHandled = 0;
  private reboots = 0;
  private readonly startedAt: number;

  private readonly lossRate: number;
  private readonly latencyMs: number;
  private readonly jitterMs: number;
  private readonly leakBytesPerFrame: number;
  private readonly heapFreeBytes: number;
  private readonly largestBlockBytes: number;
  private readonly schedule: (fn: () => void, ms: number) => void;
  private readonly now: () => number;

  constructor(options: LoopbackOptions = {}) {
    this.lossRate = options.lossRate ?? 0;
    this.latencyMs = options.latencyMs ?? 0;
    this.jitterMs = options.jitterMs ?? 0;
    this.leakBytesPerFrame = options.leakBytesPerFrame ?? 0;
    this.heapFreeBytes = options.heapFreeBytes ?? 196_108;
    this.largestBlockBytes = options.largestBlockBytes ?? 172_032;
    this.rng = options.seed ?? 0x5EED_1234;
    this.schedule = options.schedule ?? ((fn, ms) => void setTimeout(fn, ms));
    this.now = options.now ?? (() => Date.now());
    this.startedAt = this.now();
  }

  private nextRandom(): number {
    this.rng ^= this.rng << 13;
    this.rng ^= this.rng >>> 17;
    this.rng ^= this.rng << 5;
    return (this.rng >>> 0) / 0xffff_ffff;
  }

  send(frame: Uint8Array): boolean {
    this.framesHandled++;

    if (this.nextRandom() < this.lossRate) return true; // accepted, never returns

    const delay = this.latencyMs + (this.nextRandom() < 0.05 ? this.jitterMs : 0);
    const copy = Buffer.from(frame);
    this.schedule(() => {
      for (const listener of this.listeners) listener(copy);
    }, delay);

    return true;
  }

  onFrame(listener: (frame: Buffer) => void): void {
    this.listeners.push(listener);
  }

  /** Simulates the bridge resetting: uptime goes backwards, which is the signal. */
  reboot(): void {
    this.reboots++;
  }

  readHealth = async (): Promise<BridgeHealth> => {
    return {
      heapFreeBytes: this.heapFreeBytes - this.framesHandled * this.leakBytesPerFrame,
      largestBlockBytes: this.largestBlockBytes,
      // Reset by each reboot, which is exactly what the harness watches for.
      uptimeSeconds: (this.now() - this.startedAt) / 1000 - this.reboots * 1e6,
    };
  };
}
