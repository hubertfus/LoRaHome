import { DatabaseSync } from 'node:sqlite';

const SCHEMA = `
CREATE TABLE IF NOT EXISTS devices (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  last_seen_ms INTEGER,
  config_version INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS telemetry (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id INTEGER NOT NULL REFERENCES devices(id),
  output_id TEXT NOT NULL,
  value REAL NOT NULL,
  at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id INTEGER NOT NULL REFERENCES devices(id),
  kind TEXT NOT NULL,
  detail TEXT,
  at_ms INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_telemetry_device_time ON telemetry(device_id, at_ms);
CREATE INDEX IF NOT EXISTS idx_events_device_time ON events(device_id, at_ms);
`;

export function openDb(path = 'lorahome.sqlite'): DatabaseSync {
  const db = new DatabaseSync(path);
  db.exec(SCHEMA);
  return db;
}
