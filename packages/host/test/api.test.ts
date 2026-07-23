import assert from 'node:assert/strict';
import { test } from 'node:test';
import Fastify from 'fastify';
import { registerRoutes } from '../src/api/index.js';
import { openDb } from '../src/db/index.js';

test('GET /health returns ok', async () => {
  const app = Fastify();
  const db = openDb(':memory:');
  await registerRoutes(app, { db });

  const res = await app.inject({ method: 'GET', url: '/health' });
  assert.equal(res.statusCode, 200);
  assert.deepEqual(res.json(), { status: 'ok' });
});

test('GET /devices returns rows inserted in sqlite', async () => {
  const app = Fastify();
  const db = openDb(':memory:');
  db.prepare('INSERT INTO devices (id, name, last_seen_ms, config_version) VALUES (?, ?, ?, ?)').run(
    1,
    'greenhouse-node-1',
    1234,
    2,
  );
  await registerRoutes(app, { db });

  const res = await app.inject({ method: 'GET', url: '/devices' });
  assert.equal(res.statusCode, 200);
  assert.deepEqual(res.json(), {
    devices: [{ id: 1, name: 'greenhouse-node-1', last_seen_ms: 1234, config_version: 2 }],
  });
});
