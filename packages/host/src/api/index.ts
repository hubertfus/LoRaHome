import type { FastifyInstance } from 'fastify';
import type { DatabaseSync } from 'node:sqlite';

export interface ApiDeps {
  db: DatabaseSync;
}

export async function registerRoutes(app: FastifyInstance, { db }: ApiDeps): Promise<void> {
  app.get('/health', async () => ({ status: 'ok' }));

  app.get('/devices', async () => {
    const rows = db.prepare('SELECT id, name, last_seen_ms, config_version FROM devices').all();
    return { devices: rows };
  });
}
