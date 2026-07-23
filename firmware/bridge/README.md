# firmware/bridge

ESP32 + SX1262. Serial (SLIP, `0xC0`) ⇄ LoRa P2P translator. Responsible for the duty cycle tracker (ETSI 1%), retransmission of `ACK_REQ`-flagged frames, and time windows (TDMA-lite). Does not interpret the CBOR payload beyond the header — see [ARCHITECTURE.md](../../ARCHITECTURE.md) §7.

- `src/duty_cycle_tracker.h` — fixed-capacity rolling-window duty cycle enforcement, independent from the Host's own guard.
- `src/main.cpp` — wires `firmware/common`'s SLIP decoder (Serial side) to `LoraTransport` (radio side) and back.
