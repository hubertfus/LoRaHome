/**
 * 24-hour link stability soak. Roadmap T1.6.
 *
 * Sends a frame at a fixed interval for a fixed duration and records what came
 * back: losses, latency, reordering, and — the number that decides whether a
 * release ships — the drift in the Bridge's free heap between the first hour
 * and the last.
 *
 * > "`heap_free.end - heap_free.start` is the most important number in the
 * > whole project. If the drift is non-zero we have a leak and the release must
 * > not be tagged."
 *
 * Everything here is designed around making that number trustworthy over a day,
 * which mostly means not lying when the run is imperfect:
 *
 *   - Loss is counted against frames that had time to come back, not against
 *     everything ever sent, so the last few in flight at the deadline are not
 *     reported as losses.
 *   - A frame arriving after it was written off is counted as late, not as a
 *     second copy of something else, and the loss count is corrected.
 *   - Heap drift is measured between two *samples*, and the first is taken
 *     after a warmup period rather than at t=0, because a stack that is still
 *     allocating its buffers during the first minute is not leaking.
 *   - Every sample is written out as it is taken. A 24-hour run that loses its
 *     results to a crash at hour 23 is worse than no run.
 *
 * The transport is injected, so this drives either a real serial link or the
 * in-process loopback in `loopback.ts`. Without that, a harness that only runs
 * with two boards attached is a harness whose own arithmetic is never tested —
 * and the arithmetic is the part that has to be right at 3 a.m. on day two.
 */

/** One frame's round trip, as it is being tracked. */
interface InFlight {
  seq: number;
  sentAtMs: number;
}

/** What the soak needs from a link. Satisfied by SerialTransport and by the loopback. */
export interface SoakTransport {
  send(frame: Uint8Array): boolean;
  onFrame(listener: (frame: Buffer) => void): void;
}

/** Optional health readout from the far side, if the firmware reports one. */
export interface BridgeHealth {
  heapFreeBytes: number;
  largestBlockBytes: number;
  uptimeSeconds: number;
}

export interface SoakOptions {
  transport: SoakTransport;
  /** How long to run, in milliseconds. */
  durationMs: number;
  /** Gap between transmissions. The roadmap's nightly run uses 5 s. */
  intervalMs: number;
  /**
   * How long a frame may take before it counts as lost.
   *
   * Must exceed the round-trip airtime with margin. At SF9 a full frame is
   * 1147.9 ms each way, so anything under ~3 s would report healthy traffic as
   * loss.
   */
  timeoutMs: number;
  /**
   * Ignore this much of the start when computing heap drift.
   *
   * A stack still allocating its buffers in the first minute is not leaking,
   * and starting the baseline at t=0 would report that startup as drift.
   */
  warmupMs?: number;
  /** Payload size. Defaults to the MTU, which is the case worth soaking. */
  payloadBytes?: number;
  /** Polled periodically if the far side can report it. */
  readBridgeHealth?: () => Promise<BridgeHealth | null>;
  /** Called for every sample, so a long run is durable against a crash. */
  onSample?: (sample: SoakSample) => void;
  /** Injectable for tests. */
  now?: () => number;
  sleep?: (ms: number) => Promise<void>;
}

export interface SoakSample {
  atMs: number;
  framesSent: number;
  framesReceived: number;
  framesLost: number;
  heapFreeBytes: number | null;
  largestBlockBytes: number | null;
  hostHeapUsedBytes: number;
}

export interface SoakReport {
  durationHours: number;
  framesSent: number;
  framesReceived: number;
  framesLost: number;
  /** Arrived after being written off. Counted, and removed from the loss total. */
  framesLate: number;
  /** Arrived out of sequence. Expected on a retransmitting link, not on this one. */
  framesOutOfOrder: number;
  lossPercent: number;
  latencyP50Ms: number | null;
  latencyP99Ms: number | null;
  latencyMaxMs: number | null;
  bridgeReboots: number;
  heapFreeStart: number | null;
  heapFreeEnd: number | null;
  /** end - start. Negative means the far side is losing memory. */
  heapDriftBytes: number | null;
  largestBlockStart: number | null;
  largestBlockEnd: number | null;
  largestBlockDriftBytes: number | null;
  hostHeapDriftBytes: number;
  samples: SoakSample[];
}

const DEFAULT_WARMUP_MS = 60_000;
const HEALTH_POLL_EVERY = 20; /* frames */

function percentile(sorted: number[], q: number): number | null {
  if (sorted.length === 0) return null;
  const index = Math.min(sorted.length - 1, Math.floor(q * sorted.length));
  return sorted[index] ?? null;
}

/**
 * Frames carry an 8-byte header whose `seq` is one byte, which wraps every 256
 * frames — far too fast for a 24-hour run at 5 s intervals (17280 frames). The
 * soak therefore writes its own 32-bit counter into the payload rather than
 * relying on the protocol's sequence number, so a "duplicate" is never actually
 * a frame from four hours ago.
 */
export function encodeSoakPayload(seq: number, sizeBytes: number): Uint8Array {
  const payload = new Uint8Array(Math.max(4, sizeBytes));
  new DataView(payload.buffer).setUint32(0, seq >>> 0, false);
  for (let i = 4; i < payload.length; i++) payload[i] = (seq + i) & 0xff;
  return payload;
}

export function decodeSoakSeq(frame: Uint8Array): number | null {
  if (frame.length < 4) return null;
  return new DataView(frame.buffer, frame.byteOffset, frame.byteLength).getUint32(0, false);
}

export async function runSoak(options: SoakOptions): Promise<SoakReport> {
  const {
    transport,
    durationMs,
    intervalMs,
    timeoutMs,
    warmupMs = DEFAULT_WARMUP_MS,
    payloadBytes = 230,
    readBridgeHealth,
    onSample,
    now = () => Date.now(),
    sleep = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms)),
  } = options;

  const inFlight = new Map<number, InFlight>();
  const writtenOff = new Set<number>();
  const latencies: number[] = [];
  const samples: SoakSample[] = [];

  let framesSent = 0;
  let framesReceived = 0;
  let framesLost = 0;
  let framesLate = 0;
  let framesOutOfOrder = 0;
  let highestSeqSeen = -1;
  let bridgeReboots = 0;
  let lastUptime: number | null = null;

  let heapFreeStart: number | null = null;
  let heapFreeEnd: number | null = null;
  let largestBlockStart: number | null = null;
  let largestBlockEnd: number | null = null;

  transport.onFrame((frame) => {
    const seq = decodeSoakSeq(frame);
    if (seq === null) return;

    const pending = inFlight.get(seq);
    if (pending === undefined) {
      // Either it arrived after we gave up on it, or it is not ours at all.
      // The distinction matters: writing a late frame off as a loss would
      // overstate loss on a link that is merely slow.
      if (writtenOff.delete(seq)) {
        framesLate++;
        framesLost--;
      }
      return;
    }

    inFlight.delete(seq);
    framesReceived++;
    latencies.push(now() - pending.sentAtMs);

    if (seq < highestSeqSeen) framesOutOfOrder++;
    else highestSeqSeen = seq;
  });

  /** Anything past its deadline is written off, but remembered in case it lands. */
  const expire = (atMs: number): void => {
    for (const [seq, pending] of inFlight) {
      if (atMs - pending.sentAtMs <= timeoutMs) continue;
      inFlight.delete(seq);
      writtenOff.add(seq);
      framesLost++;
    }
  };

  const startedAt = now();
  const hostHeapStart = process.memoryUsage().heapUsed;

  while (now() - startedAt < durationMs) {
    const iterationStart = now();

    const payload = encodeSoakPayload(framesSent, payloadBytes);
    if (transport.send(payload)) {
      inFlight.set(framesSent, { seq: framesSent, sentAtMs: iterationStart });
      framesSent++;
    }

    await sleep(intervalMs);
    expire(now());

    if (framesSent % HEALTH_POLL_EVERY === 0 && readBridgeHealth !== undefined) {
      const health = await readBridgeHealth();
      if (health !== null) {
        // Uptime going backwards is the only reliable reboot signal we have —
        // a bridge that resets comes back with its counters cleared and would
        // otherwise look like a perfectly healthy one.
        if (lastUptime !== null && health.uptimeSeconds < lastUptime) bridgeReboots++;
        lastUptime = health.uptimeSeconds;

        const elapsed = now() - startedAt;
        if (elapsed >= warmupMs && heapFreeStart === null) {
          heapFreeStart = health.heapFreeBytes;
          largestBlockStart = health.largestBlockBytes;
        }
        if (elapsed >= warmupMs) {
          heapFreeEnd = health.heapFreeBytes;
          largestBlockEnd = health.largestBlockBytes;
        }
      }
    }

    const sample: SoakSample = {
      atMs: now() - startedAt,
      framesSent,
      framesReceived,
      framesLost,
      heapFreeBytes: heapFreeEnd,
      largestBlockBytes: largestBlockEnd,
      hostHeapUsedBytes: process.memoryUsage().heapUsed,
    };
    samples.push(sample);
    // Emitted as it happens: a 24-hour run that loses its results to a crash at
    // hour 23 has told us nothing.
    onSample?.(sample);
  }

  // Give whatever is still in flight its full timeout rather than counting it
  // against a deadline that arrived mid-round-trip.
  const drainUntil = now() + timeoutMs;
  while (inFlight.size > 0 && now() < drainUntil) {
    await sleep(Math.min(intervalMs, 100));
    expire(now());
  }
  expire(now() + timeoutMs + 1);

  const sortedLatencies = [...latencies].sort((a, b) => a - b);
  const elapsedMs = now() - startedAt;

  // Denominator is frames that were resolved, not frames ever sent.
  const resolved = framesReceived + framesLost;

  return {
    durationHours: elapsedMs / 3_600_000,
    framesSent,
    framesReceived,
    framesLost,
    framesLate,
    framesOutOfOrder,
    lossPercent: resolved === 0 ? 0 : (100 * framesLost) / resolved,
    latencyP50Ms: percentile(sortedLatencies, 0.5),
    latencyP99Ms: percentile(sortedLatencies, 0.99),
    latencyMaxMs: sortedLatencies.at(-1) ?? null,
    bridgeReboots,
    heapFreeStart,
    heapFreeEnd,
    heapDriftBytes:
      heapFreeStart === null || heapFreeEnd === null ? null : heapFreeEnd - heapFreeStart,
    largestBlockStart,
    largestBlockEnd,
    largestBlockDriftBytes:
      largestBlockStart === null || largestBlockEnd === null
        ? null
        : largestBlockEnd - largestBlockStart,
    hostHeapDriftBytes: process.memoryUsage().heapUsed - hostHeapStart,
    samples,
  };
}

/** Renders a report as LH_METRIC lines for the collector and the gate. */
export function formatSoakMetrics(report: SoakReport, label = 'soak'): string {
  const lines: string[] = [];
  const metric = (name: string, value: number | null, unit: string, budget?: string): void => {
    const rendered = value === null ? 'SKIPPED' : Number(value.toFixed(3));
    lines.push(
      `LH_METRIC ${label}.${name} value=${rendered} unit=${unit}` +
        (budget === undefined ? '' : ` budget=${budget}`),
    );
  };

  metric('duration_h', report.durationHours, 'h');
  metric('frames_sent', report.framesSent, 'count');
  metric('frames_lost', report.framesLost, 'count');
  metric('loss_pct', report.lossPercent, 'pct', '0.1');
  metric('frames_late', report.framesLate, 'count');
  metric('frames_out_of_order', report.framesOutOfOrder, 'count');
  metric('bridge_reboots', report.bridgeReboots, 'count', '0');
  metric('latency.p50_ms', report.latencyP50Ms, 'ms');
  metric('latency.p99_ms', report.latencyP99Ms, 'ms');
  metric('heap_free.start', report.heapFreeStart, 'B');
  metric('heap_free.end', report.heapFreeEnd, 'B');

  // The one that blocks a release. Absolute value, because losing memory and
  // mysteriously gaining it are both reasons not to tag.
  const drift = report.heapDriftBytes;
  metric('heap_drift_abs', drift === null ? null : Math.abs(drift), 'B', '1024');
  metric(
    'largest_block_drift_abs',
    report.largestBlockDriftBytes === null ? null : Math.abs(report.largestBlockDriftBytes),
    'B',
    '1024',
  );
  metric('host_heap_drift_mb', report.hostHeapDriftBytes / (1024 * 1024), 'MB', '1');

  return lines.join('\n');
}
