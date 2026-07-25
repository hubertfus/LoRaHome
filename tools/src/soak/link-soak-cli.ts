/**
 * CLI for the link soak. Roadmap T1.6.
 *
 *   node tools/dist/src/soak/link-soak-cli.js --port COM4 --duration 24h
 *   node tools/dist/src/soak/link-soak-cli.js --loopback --duration 60s
 *
 * The nightly run against real hardware is the first form. The second drives
 * the in-process loopback and exists so the harness itself is exercised in CI —
 * a 24-hour run is a bad place to discover that the loss accounting is wrong.
 *
 * Samples are appended to the report file as they are taken rather than written
 * at the end, because a day-long run that loses everything to a crash at hour
 * 23 has told us nothing.
 */
import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import {
  formatSoakMetrics,
  runSoak,
  type BridgeHealth,
  type SoakSample,
  type SoakTransport,
} from './link-soak.js';
import { LoopbackLink } from './loopback.js';

interface Args {
  loopback: boolean;
  port: string | null;
  baud: number;
  durationMs: number;
  intervalMs: number;
  timeoutMs: number;
  warmupMs: number;
  payloadBytes: number;
  out: string | null;
  label: string;
  lossRate: number;
  latencyMs: number;
  leakBytesPerFrame: number;
}

/** Accepts `500ms`, `30s`, `10m`, `24h`, or a bare number of milliseconds. */
export function parseDuration(text: string): number {
  const match = /^(\d+(?:\.\d+)?)(ms|s|m|h)?$/.exec(text.trim());
  if (match === null) throw new Error(`cannot parse duration: ${text}`);
  const value = Number(match[1]);
  switch (match[2]) {
    case 'h':
      return value * 3_600_000;
    case 'm':
      return value * 60_000;
    case 's':
      return value * 1000;
    case 'ms':
    case undefined:
    default:
      return value;
  }
}

function parseArgs(argv: string[]): Args {
  const get = (name: string): string | null => {
    const index = argv.indexOf(`--${name}`);
    return index === -1 ? null : (argv[index + 1] ?? null);
  };
  const has = (name: string): boolean => argv.includes(`--${name}`);

  const duration = get('duration') ?? '24h';
  const interval = get('interval') ?? '5s';

  return {
    loopback: has('loopback'),
    port: get('port'),
    baud: Number(get('baud') ?? 921600),
    durationMs: parseDuration(duration),
    intervalMs: parseDuration(interval),
    // Default well clear of a round trip: at SF9 a full frame is 1147.9 ms each
    // way, so anything under ~3 s would report healthy traffic as loss.
    timeoutMs: parseDuration(get('timeout') ?? '5s'),
    warmupMs: parseDuration(get('warmup') ?? '60s'),
    payloadBytes: Number(get('payload') ?? 230),
    out: get('out'),
    label: get('label') ?? 'soak',
    lossRate: Number(get('loss') ?? 0),
    latencyMs: parseDuration(get('latency') ?? '0'),
    leakBytesPerFrame: Number(get('leak') ?? 0),
  };
}

async function main(): Promise<void> {
  const args = parseArgs(process.argv.slice(2));

  let transport: SoakTransport;
  let readBridgeHealth: (() => Promise<BridgeHealth | null>) | undefined;

  if (args.loopback) {
    const link = new LoopbackLink({
      lossRate: args.lossRate,
      latencyMs: args.latencyMs,
      leakBytesPerFrame: args.leakBytesPerFrame,
    });
    transport = link;
    readBridgeHealth = link.readHealth;
    console.log(
      `soak: loopback, ${args.durationMs / 1000}s at ${args.intervalMs}ms` +
        ` (loss ${args.lossRate}, latency ${args.latencyMs}ms, leak ${args.leakBytesPerFrame} B/frame)`,
    );
  } else {
    if (args.port === null) {
      console.error('soak: --port is required, or pass --loopback to run without hardware');
      process.exit(2);
    }
    // Imported lazily so a loopback run needs no native serial bindings.
    const { openSerialTransport } = await import('@lorahome/host');
    transport = await openSerialTransport({ path: args.port, baudRate: args.baud });
    console.log(`soak: ${args.port} @ ${args.baud}, ${args.durationMs / 3_600_000}h`);
    // The Bridge does not yet report its heap. Until it does, heap drift — the
    // number that decides whether a release ships — is unavailable on hardware,
    // and the report will say so rather than showing a zero.
    readBridgeHealth = undefined;
  }

  const progress: SoakSample[] = [];

  const report = await runSoak({
    transport,
    durationMs: args.durationMs,
    intervalMs: args.intervalMs,
    timeoutMs: args.timeoutMs,
    warmupMs: args.warmupMs,
    payloadBytes: args.payloadBytes,
    ...(readBridgeHealth === undefined ? {} : { readBridgeHealth }),
    onSample: (sample) => {
      if (args.out === null) return;
      progress.push(sample);
      // Rewritten whole on every sample. At one sample per 5 s over 24 h that
      // is 17280 rewrites of a small file — cheap next to losing the run to a
      // crash in hour 23.
      writeFileSync(args.out, JSON.stringify({ inProgress: true, samples: progress }, null, 2));
    },
  });

  if (args.out !== null) writeFileSync(args.out, JSON.stringify(report, null, 2));

  console.log(formatSoakMetrics(report, args.label));

  const heapDrift = report.heapDriftBytes;
  const failures: string[] = [];
  if (report.lossPercent > 0.1) failures.push(`loss ${report.lossPercent.toFixed(3)}% (budget 0.1%)`);
  if (report.bridgeReboots > 0) failures.push(`${report.bridgeReboots} bridge reboot(s) (budget 0)`);
  if (heapDrift !== null && Math.abs(heapDrift) > 1024) {
    failures.push(`heap drift ${heapDrift} B (budget +/-1024 B) — DO NOT TAG A RELEASE`);
  }

  for (const failure of failures) console.error(`SOAK FAIL: ${failure}`);
  process.exit(failures.length > 0 ? 1 : 0);
}

// Only when run directly. `parseDuration` is imported by the tests, and a
// top-level `await main()` would start a 24-hour soak the moment they load it.
if (process.argv[1] !== undefined && fileURLToPath(import.meta.url) === process.argv[1]) {
  await main();
}
