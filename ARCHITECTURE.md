# Architecture

This document describes the technical internals of LoRaHome: the radio protocol format, the configuration lifecycle on a node, the relationship between JSON manifests and CBOR, and the role of the Bridge. If README.md answers "what and why," this document answers "exactly how."

---

## 1. Three physical layers

```
Browser  ⇄(HTTP/WS, JSON)⇄  Host  ⇄(Serial/SLIP)⇄  Bridge  ⇄(LoRa P2P, CBOR)⇄  Node
```

- **Browser**: never talks directly to the radio. Works entirely with JSON and the React Flow graph.
- **Host**: the only place where the rule graph is turned into binary CBOR. The only place where JSON and CBOR meet.
- **Bridge**: doesn't understand config or rule semantics. It's a pure frame translator: SLIP ⇄ LoRa, plus duty cycle and retransmissions.
- **Node**: never sees JSON. Its entire world is integer-keyed CBOR and static C structures.

This separation is deliberate: **the Node is meant to be the simplest, most deterministic link in the chain.**

---

## 2. Transport abstraction

Before anything else gets built, every implementation (Host and Node) must define its transport behind an interface:

```cpp
class ITransport {
public:
    virtual bool send(const uint8_t* data, size_t len, uint16_t dstId) = 0;
    virtual void onReceive(void (*callback)(const uint8_t* data, size_t len, uint16_t srcId)) = 0;
    virtual size_t getMTU() const = 0;
    virtual ~ITransport() = default;
};
```

```typescript
interface ITransport {
  send(data: Uint8Array, dstId: number): Promise<boolean>;
  onReceive(callback: (data: Uint8Array, srcId: number) => void): void;
  getMTU(): number;
}
```

Reasoning: LoRa P2P is today's choice (range, power efficiency, no infrastructure). ESP-NOW, or some other transport, may be tomorrow's choice (denser network, higher throughput). No layer above this (CBOR parser, rule engine, config store) **is allowed to know** that an SX1262 sits underneath. All radio-dependent logic (duty cycle, retransmissions, MTU-driven fragmentation) lives below this interface, never above it.

---

## 3. Application-layer protocol

### 3.1 Header (8 bytes, fixed size)

> **Single source of truth.** This table is documentation; the authority is
> `HEADER_LAYOUT` in [`packages/protocol/src/frame.ts`](packages/protocol/src/frame.ts).
> The TypeScript codec is built from it at module load, and
> `firmware/common/include/lorahome/protocol_generated.h` is emitted from it by
> `pnpm gen:c`. Nothing in either language may hardcode an offset. `pnpm gen:c --check`
> fails CI if the generated header drifts from the schema.

| Offset | Size | Field | Byte order | Description |
|--------|------|-------|------------|-------------|
| `0` | 1B | Magic/Ver | — | `0x4B` (`K`), identifies the KNI protocol and version |
| `1` | 1B | Frame type | — | see table below |
| `2-3` | 2B | Src ID | **big-endian** | sender address |
| `4-5` | 2B | Dst ID | **big-endian** | recipient address, `0xFFFF` = broadcast |
| `6` | 1B | Seq | — | sequence number — dedupe + ACK correlation |
| `7` | 1B | Flags | — | bitmask: `ACK_REQ`, `FRAG`, `LAST`, `ENCR` |

**Multi-byte fields are big-endian on the wire, and every device we ship on is
little-endian.** Reading `hdr->src_id` directly returns a byte-swapped value that
still looks like a plausible node ID — `0x0102` reads as `0x0201` — so the
generated header provides `lh_hdr_get_src_id()` / `lh_hdr_set_src_id()` and
direct field access is forbidden.

Sizes, all derived from the layout and asserted at compile time on every target:

| Constant | Value | Meaning |
|---|---|---|
| `LH_HEADER_SIZE` | 8 B | this table |
| `LH_CRC_SIZE` | 2 B | trailing CRC |
| `LH_LORA_MTU` | 230 B | largest frame we put on air at SF9 |
| `LH_MAX_PAYLOAD` | 220 B | `MTU - header - CRC` |
| `LH_MIN_FRAME_SIZE` | 10 B | `header + CRC`; anything shorter is junk |

After the header: a **CBOR payload** (variable length, depending on frame type), and at the very end of the frame a **CRC16** (2B, computed over the header + payload).

The CRC is **CRC-16/CCITT-FALSE**: poly `0x1021`, init `0xFFFF`, no input or
output reflection, no final XOR. Its catalogue check value is
`CRC("123456789") == 0x29B1`. (Do not confuse this with CRC-16/AUG-CCITT, whose
check value is `0xE5CC` — same polynomial, but init `0x1D0F`.) The C and
TypeScript implementations are pinned to each other by the 500 shared vectors in
`packages/protocol/test/fixtures/crc16-vectors.json`.

```
┌────────┬────────┬─────────┬─────────┬────────┬─────────┬──────────────┬────────┐
│ Magic  │  Type  │ Src ID  │ Dst ID  │  Seq   │  Flags  │  Payload     │  CRC16 │
│  1B    │  1B    │  2B     │  2B     │  1B    │  1B     │  (CBOR, var) │  2B    │
└────────┴────────┴─────────┴─────────┴────────┴─────────┴──────────────┴────────┘
   0        1        2-3       4-5        6        7        8..N          N+1..N+2
```

The header is deliberately **binary and positional** (not CBOR) — it has to be parseable with zero allocation, with a single struct cast, before we even decide whether to decode the payload.

### 3.2 Frame types

| Code | Name | Direction | Purpose |
|-----|-------|----------|---------|
| `0x01` | `BEACON` | Node/Bridge → all | Presence announcement, health check |
| `0x02` | `JOIN_REQ` | Node → Bridge | Request to join the network |
| `0x03` | `JOIN_ACK` | Bridge → Node | Join confirmation, address assignment |
| `0x10` | `CONFIG_BEGIN` | Host → Node | Announces an incoming config (size, version, fragment count) |
| `0x11` | `CONFIG_FRAG` | Host → Node | A CBOR config fragment (see §3.3) |
| `0x12` | `CONFIG_COMMIT` | Host → Node | "That's all the fragments, validate and apply" |
| `0x13` | `CONFIG_ACK` | Node → Host | Confirms the config was applied (or rejected) |
| `0x20` | `TELEMETRY` | Node → Host | Sensor readings |
| `0x21` | `EVENT` | Node → Host | Event triggered by a local rule |
| `0x30` | `CMD` | Host → Node | Immediate command (e.g. force actuator) |
| `0x31` | `CMD_ACK` | Node → Host | Command execution confirmation |
| `0x40` | `CAPABILITY_REQ` | Host → Node | "Scan your buses and report what you see" |
| `0x41` | `CAPABILITY_RSP` | Node → Host | Report of detected devices (I2C, GPIO...) |

### 3.3 Config fragmentation

LoRa's MTU (~230 B) rarely fits an entire config (component manifests + rule table + parameters). Large configs are therefore split into fragments carrying the `FRAG`/`LAST` flags:

1. **`CONFIG_BEGIN`** — the Host sends metadata: total payload size, fragment count, config version, whole-payload CRC.
2. **`CONFIG_FRAG`** × N — each fragment has the `FRAG` flag set; the last one also gets `LAST`. The Node buffers fragments in a preallocated buffer (and rejects the transfer if it would exceed the static pool limit — see §4).
3. **`CONFIG_COMMIT`** — the Host signals the end of the transfer. The Node assembles the fragments, checks the CRC, and **only then** moves on to validation (§4).
4. **`CONFIG_ACK`** — the Node replies with status: `OK` (config validated and activated) or an error code (CRC mismatch, rule/component limit exceeded, unknown `action_id`).

The `Seq` number in the header lets the Node discard duplicate fragments (Bridge retransmissions) without reprocessing them.

---

## 4. Config lifecycle on the Node — NVS A/B slots

The Node **never overwrites a working configuration in place**. NVS (Non-Volatile Storage) holds two slots:

```
NVS
├── slot_a  (config #1, status: ACTIVE)
└── slot_b  (config #2, status: STAGING)
```

Update sequence:

1. The Node receives a complete, CRC-validated config (after `CONFIG_COMMIT`).
2. The config is written to the **inactive** slot (if `slot_a` is `ACTIVE`, the new config goes to `slot_b`) — the `ACTIVE` slot is left untouched.
3. The Node runs structural validation: component count ≤ 8, local rule count ≤ 16, every `action_id` and `src_sensor_id` points to an existing, initialized component.
4. If validation passes: the new slot is marked `STAGING → PENDING_ACTIVATION`, the Node restarts its scheduler with the new configuration, and after one full stable operating cycle (no crash-loop, watchdog didn't intervene) marks it `ACTIVE`, and the old slot `INACTIVE` (to be overwritten on the next update).
5. If validation **fails**, or the Node resets (brownout, watchdog, panic) before the new config is confirmed stable: on the next boot the Node falls back to the last slot marked `ACTIVE` — **the old, working config is never lost.**

This mechanism mirrors the A/B partition scheme known from OTA on Android/embedded systems — applied here not to the firmware itself (which changes rarely), but to the **config**, which changes often and is the main vector for "bricking" a device in the field, where physical access is expensive.

---

## 5. JSON manifests vs. CBOR payload — two different worlds

This distinction is fundamental, and getting it wrong leads to a mess in the codebase:

### Component manifests (JSON, live **only** on the Host/in the browser)

A manifest describes a hardware component's **capabilities** — its parameters, outputs, power profile. The UI (node configuration form, energy calculator, rule editor) is generated **entirely** from the manifest, with zero sensor-specific code in the UI layer.

```json
{
  "id": "bme680",
  "bus": "i2c",
  "addresses": ["0x76", "0x77"],
  "params": [
    { "key": "oversampling_temp", "type": "enum", "options": [1, 2, 4, 8, 16], "default": 2 },
    { "key": "warmup_ms", "type": "uint16", "default": 300 }
  ],
  "outputs": [
    { "id": "temperature", "unit": "°C", "type": "float" },
    { "id": "humidity", "unit": "%RH", "type": "float" },
    { "id": "pressure", "unit": "hPa", "type": "float" },
    { "id": "gas_resistance", "unit": "Ω", "type": "uint32" }
  ],
  "power": {
    "active_ua": 3600,
    "sleep_ua": 0.15,
    "measurement_ms": 189
  }
}
```

The manifest is **never sent over the radio in this form**. It's a capability description, from which the Host composes an integer-keyed CBOR payload — the mapping `"temperature"` → `1`, `"humidity"` → `2`, etc., happens once, in the Host→CBOR compiler, and is defined in [`packages/protocol`](packages/protocol).

### CBOR payload (over radio, integer keys)

```
{1: 0x0A}                          // NOT: {"sensor_id": 10}
{1: 10, 2: "gt", 3: 25.5, 4: 0.5, 5: 5000, 6: 3, 7: 1}
// src_sensor_id, op, threshold, hysteresis, debounce_ms, action_id, action_param
```

Rule: **if a key in a structure sent over radio is a string, that's a design bug.** The MTU (~230 B) and the overhead of CBOR maps with string keys are irreconcilable with a realistic number of rules/components per packet.

---

## 6. Rule engine — a flat table, not a VM

A deliberate architectural decision for V1: **no bytecode, no virtual machine**. A rule is a fixed-structure record:

```c
typedef struct {
    uint16_t src_sensor_id;
    uint8_t  op;              // GT, LT, GTE, LTE, EQ, NEQ
    float    threshold;
    float    hysteresis;      // band around threshold, to avoid flapping
    uint32_t debounce_ms;     // time the condition must hold continuously
    uint16_t action_id;
    int32_t  action_param;
} rule_t;

static rule_t rules[MAX_RULES]; // MAX_RULES = 16, statically allocated
```

Hysteresis and debounce are **always present** (not optional) — even if the UI lets you set them to `0`, the fields exist in every record, because a threshold without hysteresis right at the noise floor of a measurement guarantees relay/action oscillation.

The global rule engine (cross-device, on the Host) runs the same logic but operates on the state of multiple devices at once (e.g. "if sensor A on node 1 crosses a threshold AND sensor B on node 2 is below a threshold → trigger an action on node 3"). The local evaluator on the Node only handles rules within a single device — because it must work **without connectivity to the Host**, fully autonomously.

---

## 7. Role of the Bridge — SLIP framing

The Bridge is the only component physically connected (USB/UART) to the Host. Its jobs:

1. **Frame translation**: the Host sends raw protocol frames (§3.1) over Serial, wrapped in **SLIP** (Serial Line Internet Protocol, [RFC 1055](https://www.rfc-editor.org/rfc/rfc1055)). The Bridge unwraps SLIP and sends the frame over LoRa. In the other direction: it receives from LoRa, wraps it in SLIP, and sends it over Serial to the Host.

### 7.1. SLIP on the wire

The canonical implementation is `firmware/common/src/slip.c`; this section describes what it produces, and the two must not disagree.

A frame is `END`, the escaped payload, `END`:

| Byte | Value | Meaning |
|---|---|---|
| `END` | `0xC0` | Frame delimiter. Emitted both before and after every frame. |
| `ESC` | `0xDB` | Escape prefix. |
| `ESC_END` | `0xDC` | Second byte of an escaped `0xC0`. |
| `ESC_ESC` | `0xDD` | Second byte of an escaped `0xDB`. |

Inside a frame, a payload `0xC0` is sent as `ESC ESC_END` and a payload `0xDB` as `ESC ESC_ESC`. No other byte is altered, which is what makes the framing binary-transparent — unlike newline- or timeout-delimited framing, no payload value is forbidden.

The **leading** `END` is not redundant. RFC 1055 recommends it so that noise accumulated on an idle line is terminated as an empty frame rather than being prepended to the next real one. An empty frame (`END END`) carries nothing and is therefore not delivered to the caller — it is a delimiter pair, not a zero-length message.

Consequences the rest of the system has to respect:

- **Worst-case expansion is exactly 2×.** A payload of nothing but `0xC0`/`0xDB` doubles, so an encoded frame is at most `2n + 2` bytes. Every serial TX buffer is sized from `LH_SLIP_ENCODED_MAX()` and the relation is held by `_Static_assert`, because a buffer sized for the average frame passes every test and overruns in the field (risk R1.1). `lh_slip_encode()` refuses a buffer that could not hold the worst case, so the failure is deterministic rather than payload-dependent.
- **Decoding is streaming, one byte at a time.** A UART delivers whatever was in the FIFO, so frames routinely straddle reads.
- **Structural errors cost one frame, not the session.** An illegal escape pair or a frame larger than the receive buffer is counted, discarded, and the decoder discards further bytes until the next `END`. Arbitrary garbage *within* a frame is not detectable at this layer — a random data byte is indistinguishable from payload — and is caught one layer up by the frame CRC (§3.1).
- **No allocation.** The frame buffer belongs to the caller and is passed in at init; the decoder never resizes, copies or frees it.

### 7.2. The serial receive path

Between the wire and the SLIP decoder sits a static single-producer/single-consumer ring (`firmware/common/src/ring.c`, 2 kB), drained by the Bridge's `SerialLink`. The split exists because of one number: at 921600 baud a byte arrives every **10.9 µs**. Anything doing protocol work on the receive path — SLIP state, CRC, a log line — either loses bytes or trips the watchdog.

```
UART FIFO → driver ISR → reader task → lh_ring_t → main loop → SLIP → CRC → LoRa
            └──── moves bytes only ────┘          └──── all protocol work ────┘
```

Three properties are load-bearing:

- **Free-running indices.** `head` and `tail` count bytes and are masked only when indexing storage, so occupancy is `head - tail` in modular arithmetic, "full" and "empty" are distinguishable, and each index has exactly one writer. This is why the ring size must both be a power of two *and* divide the 65536-byte `uint16_t` range — otherwise the counter rollover shifts the masked index and corrupts a bufferful roughly every 0.7 s at full rate. Both conditions are held by `_Static_assert`.
- **Drop-newest on overrun.** Losing the incoming byte damages only the frame currently arriving, and SLIP resynchronises at the next delimiter. Dropping the oldest would additionally corrupt a frame that had already arrived intact.
- **`stat_hwm` is a warning light, not a statistic.** Peak occupancy above ~60% means the consumer is already too slow, and the first stall — an NVS write, a 390 ms LoRa transmit — will cost data. It is readable before that happens.

Two buffers sit in series (the IDF driver's and ours) and that is deliberate: the driver's absorbs interrupt-to-schedule latency, ours absorbs the far longer consumer-side stalls.

Allocation after `setup()` is zero. The reader task uses a static stack and TCB via `xTaskCreateStatic`; the ring is a member. `uart_driver_install()` allocates once during init, which §0.6 permits — the budget is zero `malloc` *after* `app_main`.

### 7.3. The radio profile, and what it costs

`LoraProfile` (`firmware/common/include/lorahome/lora_transport.h`) is the single definition of how the radio is configured. Frequency, bandwidth and sync word must match bit-for-bit on both ends or there is simply silence on the air, which is the most expensive failure mode to debug — so they are named in one place rather than spread across call sites.

| Setting | Value | Why |
|---|---|---|
| Frequency | 868.1 MHz | First of the three 868 MHz channels ETSI leaves at 1% duty cycle |
| Spreading factor | SF9 | Range/airtime compromise for Etap 1 |
| Bandwidth | 125 kHz | |
| Coding rate | 4/5 | |
| Preamble | 8 symbols | |
| Sync word | 0x12 (private) | Keeps us off LoRaWAN networks and them off us |
| TX power | 14 dBm | ETSI ceiling for this band |

**Time on air at this profile is 1147.9 ms for a full 230 B frame** (Semtech AN1200.22; C and TypeScript agree across a 336-point parameter grid). Under the ETSI 1% limit that means **one full frame every 114.8 s** per channel.

> This corrects the ~390 ms figure carried in earlier planning notes, which is an SF7 number, not SF9. The difference is roughly 3×, and it propagates: anything sized against a 390 ms frame — transmit intervals, end-to-end latency targets, how many config fragments fit in an hour — needs re-deriving from 1147.9 ms. The calculation itself was never wrong; both implementations have always followed AN1200.22. Only the number quoted in prose was.
>
> Still owed: verification against a real transmission (SDR or logic analyser). Two implementations agreeing proves they share a formula, not that the formula matches the radio.

Transmission is non-blocking (`startTransmit()` + DIO1 interrupt), and so is reception. At over a second per frame, a blocking `transmit()` would leave the Bridge deaf to the UART for that whole time and overflow its receive ring — risk R1.3. The interrupt handler sets one flag; `poll()` does the work.

While transmitting, the radio cannot hear. A frame arriving in that window is lost, and Etap 1 accepts that; retransmission and jitter are Etap 2's problem (risk R1.4). It is a recorded debt, not an oversight.

### 7.4. The forwarding path

All of the Bridge's decisions live in `firmware/common/src/bridge_core.c` as plain C, with the UART, the radio and the clock reaching it through function pointers. `firmware/bridge/src/main.cpp` is wiring. The forwarding logic that ships is therefore the same code the end-to-end test drives a thousand frames through, rather than a second implementation that resembles it.

```
Host →SLIP→ [ decode → validate → duty-cycle → TX ] →air→ [ RX → validate → SLIP ] → Host
```

**Validation before transmission is mandatory, in both directions.** A malformed frame costs the same 1147.9 ms of airtime as a good one, out of a budget that permits about one full frame every two minutes — spending that on a frame already known to be broken is the most expensive mistake this component can make. In the radio→Host direction, validating stops one bad RF moment from becoming a puzzle further up the stack.

Rejections are counted by cause, not lumped together, because in the field the three mean different things:

| Counter | Meaning | What it points at |
|---|---|---|
| `rejected_len` | Shorter than header+CRC, or over the MTU | A misconfigured sender |
| `rejected_magic` | Not one of our frames | Someone else's traffic on our sync word |
| `rejected_crc` | Ours, but corrupted | A marginal RF link |
| `rejected_duty_cycle` | Well-formed, but out of budget | Working as designed |

Checks run in that order, so a two-byte fragment is reported as a length error rather than a CRC error even though its CRC would also fail.

Buffers are static and sized by derivation. `LH_BRIDGE_TX_SERIAL_BUF` is `LH_SLIP_ENCODED_MAX(256)` = **514 bytes, not 512** — the two-byte shortfall in the original sketch is exactly enough to fail on a maximum-length frame whose every byte needs escaping (R1.1). There is no separate radio transmit buffer: a decoded frame goes to the radio as a pointer into the SLIP receive buffer, per the zero-copy rule. The whole context is 920 B against a 2560 B budget.

### 7.5. Bridge diagnostics

The Bridge is the only component in the system whose memory nobody can see. No display, no network stack, and a serial port carrying frames rather than a console — so a slow leak in it is invisible until the device stops answering, some weeks in. That is unacceptable for the component the roadmap makes a release depend on:

> `heap_free.end - heap_free.start` is the most important number in the whole project. If the drift is non-zero we have a leak and the release must not be tagged.

So the Bridge answers questions about itself. A frame of type `BRIDGE_STAT_REQ` addressed to `BRIDGE_ID` (`0x0000`, reserved and never a valid node id) is **answered locally and never transmitted**; the reply is a `BRIDGE_STAT_RSP` carrying an 82-byte fixed-layout payload. The same frame type addressed to any other destination is ordinary traffic and goes on air normally.

The payload carries three groups: the §0.4 memory probe (`heap_free_internal`, `heap_largest_block`, `heap_min_free_ever`, uptime), the forwarding counters from §7.4, and the SLIP and ring health from §7.1–7.2. `heap_largest_block` is there for the same reason §0.4 insists on it — 180 kB free in fragments too small for a 4 kB buffer is a device that dies in week three, and the free total alone cannot show that.

Design points, each with a reason:

- **Not CBOR.** The Bridge does not interpret CBOR anywhere else, and a diagnostic frame is not the place to introduce a decoder to it. Fixed offsets, big-endian, same byte order as the frame header.
- **Interception happens after validation, before the duty-cycle guard.** After, so a corrupt frame cannot trigger a reply; before, because a local answer spends no airtime.
- **A Bridge with no health source still answers**, with the counters filled in and zeroes for the heap. A Host reading zero free heap will notice; a Host receiving nothing cannot distinguish a silent bridge from an unconfigured one.
- **A malformed reply is dropped, not parsed.** The Host lets the request time out instead, because a gap in the series is visible and a heap figure read from the wrong offset is fiction that looks like data.

Two implementations exist (C encoder, TypeScript decoder) and are held together by `tools/check-bridge-stat.mjs`: 200 rows including all-zero, all-ones, and one row per field set in isolation, so two fields swapped in one language cannot cancel out.
2. **Duty cycle tracker**: ETSI EN 300 220 mandates a hard 1% transmit-time limit on 868 MHz (per channel, within a 1-hour window). The Bridge tracks actual airtime (based on frame size and SF/BW) and **refuses to transmit** if it would exceed this limit — regardless of whether the Host (Duty Cycle Guard) already checked it earlier. Two independent layers of defense.
3. **Retransmissions**: for frames with the `ACK_REQ` flag, the Bridge manages the timeout and resend (up to a configured retry limit) before reporting an error to the Host.
4. **Time windows**: a simple TDMA-lite scheme — the Bridge assigns time slots to nodes to minimize collisions in denser P2P networks, without needing full mesh routing.

The Bridge **does not interpret** the CBOR payload beyond the header (needed for Src/Dst routing and `Seq`-based dedup). This is a deliberate scope limitation — the Bridge is a transport layer, not a logic layer.

---

## 8. Capability Discovery

On boot (or on `CAPABILITY_REQ`), the Node scans its declared buses (currently: I2C) and returns a `CAPABILITY_RSP` listing detected addresses. The Host correlates addresses against the manifest database (`packages/components`) and presents in the UI: *"Detected a device at 0x76, matching the `bme680` manifest — add it as a component?"*

This inverts the typical ESPHome flow (declare a sensor in YAML, hoping it's wired correctly) into: **plug in the hardware, let the system tell you what it sees.**
