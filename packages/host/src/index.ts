import Fastify from 'fastify';
import { fileURLToPath } from 'node:url';
import { registerRoutes } from './api/index.js';
import { openDb } from './db/index.js';

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
