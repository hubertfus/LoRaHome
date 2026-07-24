# 🛰️ LoRaHome

### No-Code IoT Automation over LoRa P2P — zero recompiles, ever.

> Change a sensor threshold. Change a rule's logic. Add a new node.
> **No compilation. No flashing. Over the air in 2 seconds.**

LoRaHome is a no-code platform for building ESP32-based sensor and automation networks over 868 MHz LoRa (P2P), where **all business logic lives in configuration, not in firmware**. Firmware, once flashed, remains a generic execution engine — rules, thresholds, hysteresis, action mappings: all of it is transmitted over radio as a binary config (CBOR), edited visually in the browser.

If ESPHome is "write YAML, compile, flash" — LoRaHome is "drag a node, send a packet, done."

---

## Vision

Field sensor networks (barns, greenhouses, warehouses, remote outbuildings) share one common problem: **any change in logic requires physical access to the device**, or at best a full compile-and-OTA cycle. That doesn't scale when you have 40 sensors scattered across hectares, out of Wi-Fi range.

LoRaHome flips this model:

- **Radio as the configuration bus**, not just telemetry.
- **Firmware = generic execution machine** (static rule table + static driver registry), never changing between logic deployments.
- **UI generated from manifests**, not hardcoded — adding a new sensor is a JSON file, not a line of C++.

---

## Killer Features

| # | Feature | Why it matters |
|---|---------|--------------------|
| 1 | ⚡ **Zero-recompile config** | Threshold/rule/action changes reach the node in ~2 seconds over LoRa, instead of 3 minutes of compiling + flashing over USB. |
| 2 | 🔍 **Live capability discovery** | On boot, a node scans its I2C bus and sends a capability report. The UI asks: *"Detected BME680 at 0x76 — add it?"* |
| 3 | 🧪 **In-browser rule graph simulator** | Dry-run rule graph paths (thresholds, hysteresis, debounce) without touching hardware. |
| 4 | 🔋 **Energy budget calculator** | Enter `interval_s`, and the engine estimates battery life from the component manifest's power profile and the radio's consumption profile. |
| 5 | 📡 **Duty cycle guard (ETSI)** | The host enforces the hard 1% duty cycle limit on 868 MHz and **blocks** sending any config that would exceed it. |
| 6 | ⏪ **Time-travel debug** | A time slider replays the history of sensor states and rule-engine decisions — see exactly why a relay flipped at 3:14am. |

---

## System architecture

Four layers, one data stream (CBOR both ways over radio, JSON only locally in the browser):

```
┌───────────────────────────────────────────────────────────────────────┐
│                              BROWSER                                  │
│   React + React Flow — rule graph editor, simulator, time-travel      │
│   Component Registry UI ← generated from JSON manifests                │
└───────────────────────────────┬───────────────────────────────────────┘
                                 │ HTTP/WebSocket (JSON)
                                 ▼
┌───────────────────────────────────────────────────────────────────────┐
│                          HOST (Node.js/Fastify)                       │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────┐  ┌───────────┐  │
│  │  Component   │  │   Graph→CBOR │  │  Global rule  │  │  SQLite   │  │
│  │  Registry    │  │   compiler   │  │  engine       │  │ (state,   │  │
│  │  (manifests) │  │              │  │ (cross-device)│  │ telemetry,│  │
│  └─────────────┘  └──────────────┘  └───────────────┘  │  logs)    │  │
│                                                          └───────────┘  │
│               Duty Cycle Guard · Energy Budget Calculator              │
└───────────────────────────────┬───────────────────────────────────────┘
                                 │ Serial (SLIP framing, 0xC0)
                                 ▼
┌───────────────────────────────────────────────────────────────────────┐
│                          BRIDGE (ESP32 + SX1262)                      │
│   Serial ⇄ LoRa P2P translator · Duty cycle tracker                    │
│   Retransmissions · Time windows (TDMA-lite) · Dedup (Seq)             │
└───────────────────────────────┬───────────────────────────────────────┘
                                 │ LoRa P2P 868 MHz (CBOR, ~230B MTU)
                                 ▼
┌───────────────────────────────────────────────────────────────────────┐
│                          NODE (ESP32 + SX1262)                        │
│  ┌────────────┐ ┌───────────┐ ┌────────────┐ ┌────────────────────┐   │
│  │ Config     │ │  CBOR     │ │  Static    │ │  Scheduler          │   │
│  │ Store      │ │  Parser   │ │  driver    │ │  (sensor warm-up)   │   │
│  │ (NVS A/B)  │ │           │ │  registry  │ │  + local rule       │   │
│  │            │ │           │ │  (BME680,  │ │  evaluator          │   │
│  │            │ │           │ │  GPIO...)  │ │  (hysteresis/       │   │
│  │            │ │           │ │            │ │   debounce)         │   │
│  └────────────┘ └───────────┘ └────────────┘ └────────────────────┘   │
└───────────────────────────────────────────────────────────────────────┘
```

Protocol details (frame header format, config fragmentation, A/B lifecycle) are covered in [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Tech stack

**Host / Web (TypeScript monorepo):**
- Node.js + [Fastify](https://fastify.dev/) — API
- React + [React Flow](https://reactflow.dev/) — visual rule graph editor
- SQLite — device state, telemetry, event logs
- [`cbor-x`](https://github.com/kriszyp/cbor-x) — CBOR encoding/decoding

**Firmware (ESP32, C/C++):**
- ESP-IDF or PlatformIO
- [RadioLib](https://github.com/jgromes/RadioLib) — SX1262 driver
- TinyCBOR / cbor-lite — CBOR parser with no dynamic allocation
- Static memory pools — **zero `malloc`** in the critical path

**Data over the air:** CBOR only, with integer keys (`{1: 0x0A}`, never `{"threshold": 10}`) — LoRa's MTU (~230 B) leaves no room for verbose JSON.

---

## Why not ESPHome?

ESPHome is great — until you need to change the logic. Then the cycle starts: edit YAML → compile → flash (USB or OTA) → wait → check if it worked. Fine for a single device within Wi-Fi range. It doesn't scale for **a fleet of battery-powered LoRa sensors scattered across a field**.

| | ESPHome | LoRaHome |
|---|---|---|
| Changing a threshold/rule | Recompile + reflash (~3 min, requires access) | CBOR packet over radio (~2 s) |
| Transport | Mostly Wi-Fi/ESP-NOW | LoRa P2P 868 MHz, km range, low power |
| Config safety | In-place flash overwrite | NVS A/B slots — a new config never replaces the working one until validated |
| Hardware discovery | Static, declared in YAML | Live capability discovery — the device reports what it found on the bus |
| Rule model | YAML/Lambda automations (compiled) | Fixed-structure rule table, evaluated at runtime, no VM/bytecode |
| Energy budget | No built-in tool | Battery life calculator built into the UI |
| Historical debugging | None | Time-travel debug over state history |

LoRaHome isn't trying to be ESPHome for LoRa. It's a different mental model: **firmware as an execution machine, config as the single source of truth for logic.**

---

## 🗺️ Roadmap

LoRaHome is built in phases, not on a calendar. Each phase closes with a tagged,
stable release — we ship when the numbers say it's ready, not when a date says so.

### Phase 1 — Rock-Solid Foundations

The unglamorous work that everything else stands on: one shared definition of the
wire format across every language in the project, a dependable link between the
host and the radio, and delivery you can trust when packets are travelling
kilometres through walls, rain and interference.

- One source of truth for the on-air format, shared by host and firmware
- A stable host ⇄ radio link that survives noisy cables and long uptimes
- Reliable delivery: nothing silently lost, nothing silently duplicated
- Continuous memory and timing budgets enforced in CI from day one

**You'll be able to:** flash two boards, plug one into your laptop, and watch
messages flow both ways — reliably, for days.

### Phase 2 — Hardware That Introduces Itself

Sensors should not require a datasheet, a config file and a recompile. In this
phase nodes learn to look around, report what they found, and accept new
behaviour delivered over the air — with a safety net if that behaviour turns out
to be wrong.

- Live hardware discovery: the node tells the host what's attached
- New sensors added to the platform as data, never as UI code
- Behaviour changes delivered over the air in seconds
- Safe rollback: a bad update can never brick a node in the field

**You'll be able to:** plug a sensor into a node in a barn, and have it appear in
your browser without touching a keyboard on-site.

### Phase 3 — The Visual Node Editor

Where LoRaHome becomes a no-code platform. Drag, connect, publish. Logic lives
in the picture on your screen — and in the network minutes later.

- Visual rule graph editor: sensors, conditions, actions
- Side panels generated automatically from component definitions
- Local decisions on the node, coordinated decisions across the fleet
- One click from "graph on screen" to "logic running in the field"

**You'll be able to:** build a whole automation — "if soil moisture drops below
X for Y minutes, open valve Z" — without writing a line of code.

### Phase 4 — Superpowers

The features that separate a hobby project from something you'd trust with a
season's harvest: predicting behaviour before deployment, predicting battery life
before you seal the enclosure, and staying inside radio regulations automatically.

- In-browser simulator: dry-run your automations before they touch hardware
- Energy budget calculator: battery life estimated from your actual setup
- Automatic radio airtime compliance — the platform refuses to break the rules
- Time-travel debugging: replay exactly why a relay flipped at 3:14 a.m.

**You'll be able to:** answer "will this run all winter on two AA cells?" before
you climb the ladder.

### Beyond

Ideas on the table, unscheduled and open for discussion: multi-hop coverage for
awkward terrain, hardened long-term field deployments, richer visual logic
primitives, and integrations with the wider home-automation world.

> Roadmap items are direction, not promises. Priorities follow real deployments
> and community feedback — [open an issue](../../issues) and tell us what you'd
> build with it.

---

## Project status

🚧 Early stage (V1 in progress). The rule engine is deliberately **not** a virtual machine or bytecode — it's a flat table of records: `{src_sensor_id, op, threshold, hysteresis, debounce_ms, action_id, action_param}`. Simplicity beats flexibility until we prove simplicity isn't enough.

See [ARCHITECTURE.md](ARCHITECTURE.md) for technical details and [CONTRIBUTING.md](CONTRIBUTING.md) for contribution rules.

## License

MIT (see [LICENSE](LICENSE)).
