# @lorahome/host

Node.js + Fastify. Responsible for:

- `api/` — REST/WebSocket API for `packages/web`.
- `compiler/` — React Flow graph → CBOR compiler (the only place where JSON and CBOR meet).
- `rule-engine/` — global cross-device rule engine.
- `duty-cycle-guard/` — enforces the ETSI 1% duty cycle limit on 868 MHz, blocks sending overly "chatty" configs.
- `db/` — SQLite: device state, telemetry, event logs.

Talks to the Bridge over Serial (SLIP framing) — see [ARCHITECTURE.md](../../ARCHITECTURE.md) §7.
