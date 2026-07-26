/**
 * Deterministic link simulator. Roadmap T2.5.
 *
 * Every reliability claim in Etap 2 — dedup drops duplicates, the ARQ recovers
 * from loss, fragments survive reordering — is a claim about a link that
 * misbehaves. Until now the only way to see one misbehave was to take two
 * boards outdoors and hope. That produces bugs that cannot be reproduced, and a
 * test suite that cannot be trusted because it was never run against the
 * conditions it claims to survive.
 *
 * This is the other way: a link whose every drop, delay, reorder and flipped
 * bit is decided by a seeded generator. The same seed produces the same trace,
 * byte for byte and event for event, so a failure in CI is a `seed=0x8f2a` that
 * reproduces on a laptop in a second (R2.6). No `Math.random()` anywhere in the
 * reliability tests — a flaky test is one people re-run until it is green,
 * which is the same as not having it.
 *
 * Time is virtual and advanced by the caller. A 24-hour scenario runs in
 * milliseconds, and — more importantly — the clock never moves except where
 * the test says it does, so a timeout tested here is tested exactly.
 */

/** What a caller of SimEndpoint sees. Deliberately the shape SerialTransport has. */
export type SimFrameListener = (frame: Buffer) => void;

export interface LinkProfile {
  /** Seed for every decision this link makes. Same seed, same trace. */
  seed: number;
  /** Frames dropped outright, 0..100. */
  lossPct?: number;
  /** Frames delivered with one bit flipped — the CRC's job, not the ARQ's. */
  corruptPct?: number;
  /** Frames held back far enough to arrive after the one behind them. */
  reorderPct?: number;
  /** Frames delivered twice: the retransmission that was not needed. */
  duplicatePct?: number;
  /** One-way delay before jitter. Defaults to an SF9 frame's airtime. */
  latencyMs?: number;
  /** Uniform extra delay on top of `latencyMs`. */
  jitterMs?: number;
}

export interface SimLinkStats {
  sent: number;
  delivered: number;
  dropped: number;
  corrupted: number;
  reordered: number;
  duplicated: number;
  inFlight: number;
}

interface PendingFrame {
  dueMs: number;
  /** Send order, so delivery is total-ordered even when two frames land together. */
  ordinal: number;
  target: 'a' | 'b';
  payload: Buffer;
}

/**
 * One side of the link.
 *
 * Frames go in with `send` and arrive on listeners registered with `onFrame` —
 * the same two calls SerialTransport offers, so a test and the real Host differ
 * only in which object they were handed.
 */
export class SimEndpoint {
  private readonly listeners: SimFrameListener[] = [];

  constructor(
    private readonly link: SimLink,
    private readonly side: 'a' | 'b',
  ) {}

  onFrame(listener: SimFrameListener): void {
    this.listeners.push(listener);
  }

  send(frame: Uint8Array): boolean {
    this.link.enqueue(this.side === 'a' ? 'b' : 'a', frame);
    return true;
  }

  /** @internal — delivery comes from the link, never from the caller. */
  deliver(frame: Buffer): void {
    for (const listener of this.listeners) listener(frame);
  }
}

export class SimLink {
  readonly a: SimEndpoint;
  readonly b: SimEndpoint;

  private readonly profile: Required<LinkProfile>;
  private rng: number;
  private queue: PendingFrame[] = [];
  private ordinal = 0;
  private clockMs = 0;

  private readonly counters: SimLinkStats = {
    sent: 0,
    delivered: 0,
    dropped: 0,
    corrupted: 0,
    reordered: 0,
    duplicated: 0,
    inFlight: 0,
  };

  /**
   * Every decision, in order, as text.
   *
   * The determinism check compares these rather than the counters: two runs can
   * agree on how many frames were dropped and disagree about which, and it is
   * the "which" that makes a failure reproducible. Off by default — a 100k-frame
   * chaos run would otherwise hold a 100k-entry array for nothing.
   */
  private traceLog: string[] | null = null;

  constructor(profile: LinkProfile) {
    this.profile = {
      seed: profile.seed,
      lossPct: profile.lossPct ?? 0,
      corruptPct: profile.corruptPct ?? 0,
      reorderPct: profile.reorderPct ?? 0,
      duplicatePct: profile.duplicatePct ?? 0,
      // 390 ms is a 230 B frame at SF9 — see ARCHITECTURE.md §7.3. Defaulting to
      // it means a test that sets no latency still runs against the timing the
      // radio actually has, rather than against an instantaneous link that
      // makes every timeout look generous.
      latencyMs: profile.latencyMs ?? 390,
      jitterMs: profile.jitterMs ?? 0,
    };

    // Seed 0 would leave xorshift stuck at zero for ever — every draw identical,
    // every probability collapsing to the same branch. Rejected loudly rather
    // than silently repaired, because a test that asked for seed 0 and got
    // seed 1 is a test whose seed is a lie.
    if (!Number.isInteger(profile.seed) || profile.seed === 0) {
      throw new RangeError(`sim link seed must be a non-zero integer, got ${profile.seed}`);
    }

    this.rng = profile.seed >>> 0;
    this.a = new SimEndpoint(this, 'a');
    this.b = new SimEndpoint(this, 'b');
  }

  /** xorshift32. Small, fast, and identical to the generator the C harnesses use. */
  private nextRandom(): number {
    this.rng ^= this.rng << 13;
    this.rng ^= this.rng >>> 17;
    this.rng ^= this.rng << 5;
    return this.rng >>> 0;
  }

  /** A draw in 0..99, for percentage decisions. */
  private percent(): number {
    return this.nextRandom() % 100;
  }

  private note(event: string): void {
    if (this.traceLog !== null) this.traceLog.push(`${this.clockMs}:${event}`);
  }

  /** @internal — SimEndpoint.send routes here. */
  enqueue(target: 'a' | 'b', frame: Uint8Array): void {
    this.counters.sent++;
    const ordinal = this.ordinal++;

    if (this.profile.lossPct > 0 && this.percent() < this.profile.lossPct) {
      this.counters.dropped++;
      this.note(`drop:${target}:${ordinal}`);
      return;
    }

    const payload = Buffer.from(frame);
    if (this.profile.corruptPct > 0 && payload.length > 0 && this.percent() < this.profile.corruptPct) {
      const index = this.nextRandom() % payload.length;
      const bit = 1 << (this.nextRandom() % 8);
      payload[index] = payload[index]! ^ bit;
      this.counters.corrupted++;
      this.note(`corrupt:${target}:${ordinal}:byte=${index}`);
    }

    let dueMs = this.clockMs + this.profile.latencyMs;
    if (this.profile.jitterMs > 0) dueMs += this.nextRandom() % this.profile.jitterMs;

    if (this.profile.reorderPct > 0 && this.percent() < this.profile.reorderPct) {
      // Held back by more than a frame's flight time, so it lands behind the
      // next one rather than merely late. Reordering that stays within the
      // latency spread is jitter, and jitter is already modelled above.
      dueMs += this.profile.latencyMs + 1;
      this.counters.reordered++;
      this.note(`reorder:${target}:${ordinal}`);
    }

    this.queue.push({ dueMs, ordinal, target, payload });

    if (this.profile.duplicatePct > 0 && this.percent() < this.profile.duplicatePct) {
      this.queue.push({
        dueMs: dueMs + 1,
        ordinal: this.ordinal++,
        target,
        payload: Buffer.from(payload),
      });
      this.counters.duplicated++;
      this.note(`duplicate:${target}:${ordinal}`);
    }
  }

  /**
   * Moves the clock forward and delivers everything that came due.
   *
   * Delivery is ordered by due time and then by send order, so two frames that
   * land in the same millisecond arrive in the order they were sent — a link
   * that shuffled them would be modelling reordering the caller did not ask for,
   * and no test could tell the two apart.
   */
  advance(ms: number): number {
    this.clockMs += ms;

    const due = this.queue.filter((frame) => frame.dueMs <= this.clockMs);
    if (due.length === 0) return 0;

    this.queue = this.queue.filter((frame) => frame.dueMs > this.clockMs);
    due.sort((left, right) => left.dueMs - right.dueMs || left.ordinal - right.ordinal);

    for (const frame of due) {
      this.counters.delivered++;
      this.note(`deliver:${frame.target}:${frame.ordinal}:len=${frame.payload.length}`);
      (frame.target === 'a' ? this.a : this.b).deliver(frame.payload);
    }

    return due.length;
  }

  /** Advances in `stepMs` slices until nothing is left in flight. */
  drain(stepMs = 100, maxSteps = 100_000): number {
    let delivered = 0;
    for (let step = 0; step < maxSteps && this.queue.length > 0; step++) {
      delivered += this.advance(stepMs);
    }
    return delivered;
  }

  get nowMs(): number {
    return this.clockMs;
  }

  get nowUs(): number {
    return this.clockMs * 1000;
  }

  get stats(): SimLinkStats {
    return { ...this.counters, inFlight: this.queue.length };
  }

  /** Starts recording the decision trace. Call before any traffic. */
  startTrace(): void {
    this.traceLog = [];
  }

  /** The recorded trace. Empty if `startTrace` was never called. */
  get trace(): readonly string[] {
    return this.traceLog ?? [];
  }
}

/**
 * The five profiles the chaos suite runs (T2.6).
 *
 * Named rather than written inline at each call site so that "loss30" means the
 * same thing in a benchmark, a regression test and a commit message — and so a
 * change to what the phrase means is one diff, in one place, visible in review.
 */
export const LINK_PROFILES = {
  clean: { lossPct: 0 },
  loss10: { lossPct: 10 },
  loss30: { lossPct: 30 },
  reorder: { reorderPct: 25, jitterMs: 40 },
  corrupt: { corruptPct: 15 },
} as const satisfies Record<string, Omit<LinkProfile, 'seed'>>;

export type LinkProfileName = keyof typeof LINK_PROFILES;
