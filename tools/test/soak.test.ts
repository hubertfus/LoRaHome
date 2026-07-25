import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  decodeSoakSeq,
  encodeSoakPayload,
  formatSoakMetrics,
  runSoak,
  type BridgeHealth,
  type SoakTransport,
} from '../src/soak/link-soak.js';
import { parseDuration } from '../src/soak/link-soak-cli.js';

/**
 * A link driven by a virtual clock.
 *
 * The soak harness is the one piece of tooling here that normally runs for a
 * day at a time, unattended, and whose output decides whether a release is
 * tagged. Testing it against a real clock would mean either a test that takes
 * minutes or one that only covers the first few frames — so time is injected
 * and every case below runs a full simulated hour in milliseconds.
 */
class VirtualClock {
  private nowMs = 0;
  private readonly pending: { atMs: number; fn: () => void }[] = [];

  now = (): number => this.nowMs;

  /**
   * The harness awaits this; it advances to each due callback in turn.
   *
   * Firing everything at the *end* of the sleep would be simpler and would also
   * make every measured latency equal the send interval — the harness would
   * look wrong when the clock was. Time is moved to each callback's own
   * timestamp before running it, so latencies come out as scheduled.
   */
  sleep = async (ms: number): Promise<void> => {
    const target = this.nowMs + ms;

    for (;;) {
      let earliest = -1;
      let earliestAt = Number.POSITIVE_INFINITY;
      for (const [index, entry] of this.pending.entries()) {
        if (entry.atMs <= target && entry.atMs < earliestAt) {
          earliest = index;
          earliestAt = entry.atMs;
        }
      }
      if (earliest === -1) break;

      const [entry] = this.pending.splice(earliest, 1);
      this.nowMs = Math.max(this.nowMs, entry!.atMs);
      entry!.fn();
    }

    this.nowMs = target;
    // Yield so the harness's own promise continuations run before we return.
    await Promise.resolve();
  };

  schedule = (fn: () => void, ms: number): void => {
    this.pending.push({ atMs: this.nowMs + ms, fn });
  };
}

/** Echoes frames back through the virtual clock, dropping the ones told to. */
class ScriptedLink implements SoakTransport {
  private readonly listeners: ((frame: Buffer) => void)[] = [];
  private sent = 0;

  constructor(
    private readonly clock: VirtualClock,
    private readonly options: {
      dropSeqs?: Set<number>;
      lateSeqs?: Set<number>;
      latencyMs?: number;
      lateLatencyMs?: number;
    } = {},
  ) {}

  send(frame: Uint8Array): boolean {
    const seq = decodeSoakSeq(frame);
    this.sent++;
    if (seq === null) return false;

    if (this.options.dropSeqs?.has(seq)) return true;

    const delay = this.options.lateSeqs?.has(seq)
      ? (this.options.lateLatencyMs ?? 10_000)
      : (this.options.latencyMs ?? 100);

    const copy = Buffer.from(frame);
    this.clock.schedule(() => {
      for (const listener of this.listeners) listener(copy);
    }, delay);
    return true;
  }

  onFrame(listener: (frame: Buffer) => void): void {
    this.listeners.push(listener);
  }

  get sentCount(): number {
    return this.sent;
  }
}

// ---------------------------------------------------------------------------
// Payload sequencing
// ---------------------------------------------------------------------------

test('the soak carries its own 32-bit sequence, not the protocol byte', () => {
  // The protocol seq is one byte and wraps every 256 frames. A 24 h run at 5 s
  // sends 17280, so relying on it would make a "duplicate" indistinguishable
  // from a frame sent four hours earlier.
  for (const seq of [0, 1, 255, 256, 17_279, 4_294_967_295]) {
    assert.equal(decodeSoakSeq(encodeSoakPayload(seq, 230)), seq);
  }
});

test('the payload fills the requested size', () => {
  assert.equal(encodeSoakPayload(7, 230).length, 230);
  // Never smaller than the sequence number it has to carry.
  assert.equal(encodeSoakPayload(7, 1).length, 4);
});

// ---------------------------------------------------------------------------
// Accounting
// ---------------------------------------------------------------------------

test('a clean run reports no loss', async () => {
  const clock = new VirtualClock();
  const link = new ScriptedLink(clock, { latencyMs: 100 });

  const report = await runSoak({
    transport: link,
    durationMs: 60_000,
    intervalMs: 1000,
    timeoutMs: 5000,
    warmupMs: 0,
    now: clock.now,
    sleep: clock.sleep,
  });

  assert.equal(report.framesSent, 60);
  assert.equal(report.framesLost, 0);
  assert.equal(report.framesReceived, 60);
  assert.equal(report.lossPercent, 0);
  assert.equal(report.latencyP50Ms, 100);
});

test('dropped frames are counted, and only those', async () => {
  const clock = new VirtualClock();
  const dropSeqs = new Set([3, 11, 42]);
  const link = new ScriptedLink(clock, { dropSeqs, latencyMs: 100 });

  const report = await runSoak({
    transport: link,
    durationMs: 100_000,
    intervalMs: 1000,
    timeoutMs: 5000,
    warmupMs: 0,
    now: clock.now,
    sleep: clock.sleep,
  });

  assert.equal(report.framesLost, dropSeqs.size);
  assert.equal(report.framesReceived, report.framesSent - dropSeqs.size);
  assert.ok(report.lossPercent > 0 && report.lossPercent < 5);
});

test('a frame arriving after it was written off is late, not lost', async () => {
  // Otherwise a merely slow link reads as a lossy one, and the 0.1% loss
  // budget starts failing runs that were fine.
  const clock = new VirtualClock();
  const link = new ScriptedLink(clock, {
    latencyMs: 100,
    lateSeqs: new Set([5]),
    lateLatencyMs: 8000,
  });

  const report = await runSoak({
    transport: link,
    durationMs: 60_000,
    intervalMs: 1000,
    timeoutMs: 3000,
    warmupMs: 0,
    now: clock.now,
    sleep: clock.sleep,
  });

  assert.equal(report.framesLate, 1);
  assert.equal(report.framesLost, 0, 'the late frame must not also be counted as lost');
});

test('frames still in flight at the deadline are not counted as losses', async () => {
  // The last few frames of any run are mid-round-trip when time runs out.
  // Charging them as losses would put a floor under the reported loss rate
  // that has nothing to do with the link.
  const clock = new VirtualClock();
  const link = new ScriptedLink(clock, { latencyMs: 2000 });

  const report = await runSoak({
    transport: link,
    durationMs: 20_000,
    intervalMs: 1000,
    timeoutMs: 5000,
    warmupMs: 0,
    now: clock.now,
    sleep: clock.sleep,
  });

  assert.equal(report.framesLost, 0);
  assert.equal(report.framesReceived, report.framesSent);
});

// ---------------------------------------------------------------------------
// The number that blocks a release
// ---------------------------------------------------------------------------

test('a steady heap reports zero drift', async () => {
  const clock = new VirtualClock();
  const link = new ScriptedLink(clock, { latencyMs: 50 });

  const health = async (): Promise<BridgeHealth> => ({
    heapFreeBytes: 196_108,
    largestBlockBytes: 172_032,
    uptimeSeconds: clock.now() / 1000,
  });

  const report = await runSoak({
    transport: link,
    durationMs: 200_000,
    intervalMs: 1000,
    timeoutMs: 5000,
    warmupMs: 0,
    readBridgeHealth: health,
    now: clock.now,
    sleep: clock.sleep,
  });

  assert.equal(report.heapDriftBytes, 0);
  assert.equal(report.largestBlockDriftBytes, 0);
});

test('a leaking bridge is detected as negative heap drift', async () => {
  const clock = new VirtualClock();
  const link = new ScriptedLink(clock, { latencyMs: 50 });

  let polls = 0;
  const health = async (): Promise<BridgeHealth> => ({
    heapFreeBytes: 196_108 - 512 * polls++,
    largestBlockBytes: 172_032,
    uptimeSeconds: clock.now() / 1000,
  });

  const report = await runSoak({
    transport: link,
    durationMs: 200_000,
    intervalMs: 1000,
    timeoutMs: 5000,
    warmupMs: 0,
    readBridgeHealth: health,
    now: clock.now,
    sleep: clock.sleep,
  });

  assert.ok(report.heapDriftBytes !== null && report.heapDriftBytes < -1024, 'leak not detected');

  // And the gate has to see it as a breach.
  const metrics = formatSoakMetrics(report);
  const line = metrics.split('\n').find((l) => l.includes('heap_drift_abs'));
  assert.ok(line !== undefined && line.includes('budget=1024'));
});

test('warmup allocation is excluded from the drift baseline', async () => {
  // A stack still allocating its buffers in the first minute is not leaking.
  // Baselining at t=0 would report startup as drift and fail every run.
  const clock = new VirtualClock();
  const link = new ScriptedLink(clock, { latencyMs: 50 });

  const health = async (): Promise<BridgeHealth> => ({
    heapFreeBytes: clock.now() < 60_000 ? 250_000 : 196_108,
    largestBlockBytes: 172_032,
    uptimeSeconds: clock.now() / 1000,
  });

  const report = await runSoak({
    transport: link,
    durationMs: 300_000,
    intervalMs: 1000,
    timeoutMs: 5000,
    warmupMs: 60_000,
    readBridgeHealth: health,
    now: clock.now,
    sleep: clock.sleep,
  });

  assert.equal(report.heapDriftBytes, 0, 'startup allocation leaked into the baseline');
});

test('a bridge reboot is detected by uptime going backwards', async () => {
  const clock = new VirtualClock();
  const link = new ScriptedLink(clock, { latencyMs: 50 });

  // A reset bridge comes back with its counters cleared and would otherwise
  // look perfectly healthy. Uptime is the only signal that survives.
  let rebooted = false;
  const health = async (): Promise<BridgeHealth> => {
    const elapsed = clock.now() / 1000;
    if (elapsed > 100 && !rebooted) rebooted = true;
    return {
      heapFreeBytes: 196_108,
      largestBlockBytes: 172_032,
      uptimeSeconds: rebooted ? elapsed - 100 : elapsed,
    };
  };

  const report = await runSoak({
    transport: link,
    durationMs: 200_000,
    intervalMs: 1000,
    timeoutMs: 5000,
    warmupMs: 0,
    readBridgeHealth: health,
    now: clock.now,
    sleep: clock.sleep,
  });

  assert.equal(report.bridgeReboots, 1);
});

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

test('unavailable measurements are SKIPPED, never zero', async () => {
  // Without a health probe there is no heap figure. Emitting 0 would read as a
  // perfect result and pass the gate.
  const clock = new VirtualClock();
  const link = new ScriptedLink(clock, { latencyMs: 50 });

  const report = await runSoak({
    transport: link,
    durationMs: 10_000,
    intervalMs: 1000,
    timeoutMs: 5000,
    warmupMs: 0,
    now: clock.now,
    sleep: clock.sleep,
  });

  const metrics = formatSoakMetrics(report);
  assert.match(metrics, /soak\.heap_free\.start value=SKIPPED/);
  assert.match(metrics, /soak\.heap_drift_abs value=SKIPPED/);
  assert.match(metrics, /soak\.frames_sent value=10 /);
});

test('durations parse in the units the CLI advertises', () => {
  assert.equal(parseDuration('500ms'), 500);
  assert.equal(parseDuration('30s'), 30_000);
  assert.equal(parseDuration('10m'), 600_000);
  assert.equal(parseDuration('24h'), 86_400_000);
  assert.equal(parseDuration('1500'), 1500);
  assert.throws(() => parseDuration('soon'));
});
