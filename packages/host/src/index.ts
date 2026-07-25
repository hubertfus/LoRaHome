import Fastify from 'fastify';
import { fileURLToPath } from 'node:url';
import { registerRoutes } from './api/index.js';
import { openDb } from './db/index.js';

// The link to the Bridge (T1.5). Re-exported so tools/soak and anything else
// outside this package reach one public surface rather than deep paths.
export {
  SerialTransport,
  openSerialTransport,
  type ByteStream,
  type OpenSerialOptions,
  type SerialTransportOptions,
  type SerialTransportStats,
} from './transport/serial.js';
export { SlipDecoder, slipEncode, slipEncodedMax, SlipState } from './transport/slip.js';
export {
  BRIDGE_STAT_SIZE,
  BRIDGE_STAT_VERSION,
  decodeBridgeStat,
  encodeBridgeStat,
  type BridgeStat,
} from './transport/bridge-stat.js';
export { BridgeHealthClient, type BridgeHealthClientOptions } from './transport/bridge-health.js';

export async function buildServer() {
  const app = Fastify({ logger: true });
  const db = openDb(process.env.LORAHOME_DB_PATH ?? 'lorahome.sqlite');
  await registerRoutes(app, { db });
  return app;
}

async function main() {
  const app = await buildServer();
  const port = Number(process.env.PORT ?? 3000);
  await app.listen({ port, host: '0.0.0.0' });
}

// Only auto-start when run directly (not when imported by tests).
if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  main().catch((err) => {
    console.error(err);
    process.exit(1);
  });
}
