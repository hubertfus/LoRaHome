# Test bench procedures

Everything in this file needs hardware. It exists because the alternative —
carrying these procedures in someone's head — is how a measurement gets taken
slightly differently the second time and the metric series becomes fiction.

Each section states what is being measured, how to set it up, and what number
would make the result invalid. Where a metric is currently reported `SKIPPED`,
this is the procedure that turns it into a number.

## Bill of materials

| Item | Notes |
|---|---|
| 2× ESP32 dev board with SX1262 | Pinout as wired in `firmware/bridge/src/main.cpp`: CS 18, IRQ 26, RST 23, GPIO 33 |
| 2× 868 MHz antenna | Never power a board without one — an unterminated PA can damage itself |
| 1× 30 dB SMA attenuator | Mandatory for bench tests, see below |
| USB cables capable of 921600 baud | Some charge-only cables enumerate but drop bytes |
| rtl-sdr or similar (optional) | For the airtime and frequency checks |

## R1.5 — the attenuator is not optional

**Two boards a metre apart, with antennas and no attenuator, will overload each
other's receive front end.** The symptom is the confusing one: CRC failures at
1 m that disappear at 50 m. Hours get spent reading the software.

Every bench test below uses a **30 dB attenuator** on at least one end. If a
result looks wrong, check the attenuator is fitted before changing any code.

## Procedures

### P1 — UART throughput and ring high-water mark (T1.2)

Fills in: `bench.uart.tput.sustained`, `mem.ring.hwm_pct`, `stack.hwm.uart_task`,
`test.flood_10MB.errors`.

1. Flash the bridge. Connect one board only; no radio traffic during this test.
2. From the host, stream 10 MB of known data at 921600 baud in blocks, each with
   a CRC, and verify every block on return.
3. Read `SerialLink::overrunCount()` and `ringHighWaterPct()` at the end.

Valid only if `overrunCount() == 0`. A non-zero overrun means the consumer fell
behind and the throughput figure describes a lossy link, not a working one.
`ringHighWaterPct()` above 60% is a warning even when nothing was lost — the
first NVS write or flash erase will cost data.

`stack.hwm.uart_task` comes from `uxTaskGetStackHighWaterMark()` on the reader
task. Budget 2048 B against a 3072 B stack.

### P2 — Airtime, measured against computed (T1.3)

Fills in: `bench.lora.airtime.230B.sf9.measured`.

The computed value is **1147.9 ms** for a 230 B frame at SF9/BW125/CR4-5,
preamble 8, explicit header. C and TypeScript agree on this across a 336-point
grid (`pnpm check:airtime`), but agreeing with each other is not the same as
agreeing with the radio.

1. Transmit a full 230 B frame in a loop with a known gap.
2. Capture with an SDR, or put a scope/logic analyser on DIO1 and measure from
   the start of transmission to `TxDone`.
3. Compare against 1147.9 ms.

**A disagreement over 5% is not a measurement problem, it is a compliance
problem.** The duty cycle tracker spends this number; if it is wrong, the
bridge's ETSI EN 300 220 accounting is wrong with it. Do not tune the number to
match — find out which side is wrong.

While the SDR is out, confirm the centre frequency is 868.1 MHz and the sync
word is the private 0x12. A mismatch on either produces silence, and silence
sends people to read software (R1.6).

### P3 — Radio init, RSSI and SNR (T1.3)

Fills in: `bench.radio.init_ms`, `metric.rssi_at_1m_30dbatt`,
`metric.snr_at_1m_30dbatt`, `heap.free.post_radio_init`.

1. Time `LoraTransport::begin()` from entry to return. Budget 200 ms — it is
   paid on every wake from deep sleep.
2. Place the boards 1 m apart **with the 30 dB attenuator fitted**.
3. Send 100 frames and read `lastRssiDbm()` / `lastSnrDb()` at the receiver.
4. Read `esp_get_free_heap_size()` after `begin()` returns. Budget ≥ 180 kB.

Record the attenuation in the metric name. An RSSI figure without it is
meaningless.

### P4 — End-to-end frame delivery (T1.4)

Fills in: `bench.e2e.latency.p50.ms`, `bench.e2e.latency.p99.ms`,
`metric.frame_loss_1000`, `heap.free.steady_state`, `heap.largest_block`.

1. Two bridges, attenuator fitted, each on its own host port.
2. Send 1000 frames of mixed length through host A and compare byte for byte at
   host B.
3. Send frames with deliberately corrupted CRCs interleaved. Confirm
   `rejected_crc` rises and `radio_frames_out` does not.

Budget: 0 losses under bench conditions.

> **Latency expectation.** Airtime alone is 1147.9 ms at this profile, so
> end-to-end p50 will be somewhere above that. The 402 ms target in the original
> planning notes is an SF7 figure and is not reachable at SF9. Record what you
> measure; re-targeting is an architect's decision, not a bench one.

### P5 — 24-hour soak (T1.6)

Fills in: the whole `soak.*` family, including the one that blocks a release.

```bash
pnpm soak --port COM4 --duration 24h --interval 5s --out soak-report.json
```

Before starting:

- Fit the attenuator.
- Use a dedicated machine. A laptop that sleeps produces a report with a hole in
  it, which is worse than no report.
- Confirm `pnpm soak:selfcheck` passes first. It drives the harness against its
  in-process loopback and checks the accounting — loss, late arrivals, heap
  drift, reboot detection — so a day is not spent discovering the harness is
  wrong.

Pass conditions:

| Metric | Budget |
|---|---|
| `soak.loss_pct` | < 0.1% |
| `soak.bridge_reboots` | 0 |
| `soak.heap_drift_abs` | ≤ 1024 B |
| `soak.largest_block_drift_abs` | ≤ 1024 B |
| `soak.host_heap_drift_mb` | < 1 MB |

> `heap_free.end - heap_free.start` is the most important number in the project.
> A non-zero drift means a leak, and a release with a leak does not get tagged.
> The largest-block drift matters just as much: 180 kB free in fragments that
> cannot hold a 4 kB buffer is a device that dies in week three.

**Currently blocked.** The Bridge does not yet report its free heap over the
link, so the soak has no health source on real hardware and the heap figures
come back `SKIPPED` rather than zero. A diagnostic frame carrying
`lh_mem_snapshot_t` (§0.4) closes this. Until then the 24-hour soak can measure
loss, latency and reboots, but not the number that decides whether to tag.
