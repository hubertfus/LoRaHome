# firmware/common

C/C++ code shared between the Bridge and the Node — a PlatformIO library (`library.json`), consumed via `lib_deps = symlink://../common`.

- `include/lorahome/transport.h` — the `ITransport` interface (ARCHITECTURE.md §2).
- `include/lorahome/protocol.h` + `src/protocol.c` — 8-byte frame header, frame types, CRC16 encode/decode. Mirrors `packages/protocol/src/frame.ts` field-for-field; see `packages/protocol/test/fixtures` for the shared cross-language test vectors.
- `include/lorahome/field_map.h` — CBOR integer-key constants, mirrors `packages/protocol/src/field-map.ts`.
- `include/lorahome/rule_evaluator.h` + `src/rule_evaluator.c` — the hysteresis/debounce state machine, mirrors `packages/protocol/src/rule-evaluator.ts`.
- `include/lorahome/airtime.h` + `src/airtime.c` — LoRa time-on-air estimate, mirrors `packages/host/src/duty-cycle-guard/airtime.ts`.
- `include/lorahome/slip.h` + `src/slip.c` — SLIP framing (RFC 1055) for the Bridge's Serial link.
- `include/lorahome/lora_transport.h` — `ITransport` over RadioLib's SX1262, shared by Bridge and Node.

Never hand-edit the mirrored TS/C pairs separately — see [CONTRIBUTING.md](../../CONTRIBUTING.md) §3/§1.4.
