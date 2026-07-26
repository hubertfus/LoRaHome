/*
 * End-to-end forwarding tests for the bridge core (T1.4).
 *
 * Two bridge contexts are wired back to back through an in-process "radio":
 *
 *   host A -> SLIP -> bridge A -> [radio] -> bridge B -> SLIP -> host B
 *
 * That is the roadmap's end-to-end test with the RF and the UART taken out. It
 * cannot tell you anything about airtime, RSSI, or whether the antenna is
 * connected — but it does verify the part that a bench test with two boards
 * verifies badly: that a thousand frames arrive byte for byte, that a corrupted
 * frame is refused *before* it costs a second of airtime, and that each
 * rejection lands on the counter that will be read to diagnose it.
 *
 * The radio hop is deliberately lossless and instant. Loss and latency belong
 * to the real link; what is under test here is the forwarding decision.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/bridge_core.h"
#include "lorahome/crc16.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                          \
  do {                                            \
    g_checks++;                                   \
    if (!(cond)) {                                \
      g_failures++;                               \
      printf("FAIL %s:%d  ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
    }                                             \
  } while (0)

static uint32_t g_rng = 0x39B5C7D1u;

static uint32_t next_random(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

static double now_seconds(void) {
#if defined(_WIN32)
  static LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static int compare_double(const void *a, const void *b) {
  const double lhs = *(const double *)a;
  const double rhs = *(const double *)b;
  return (lhs > rhs) - (lhs < rhs);
}

/* ------------------------------------------------------------------------- */
/* The rig                                                                    */
/* ------------------------------------------------------------------------- */

static lh_bridge_ctx_t g_bridge_a; /* host side  */
static lh_bridge_ctx_t g_bridge_b; /* far side   */

/** What bridge B finally delivered upstream, SLIP-decoded again. */
static uint8_t g_delivered[LH_BRIDGE_RADIO_BUF];
static uint16_t g_delivered_len;
static uint32_t g_delivered_count;

static uint8_t g_host_b_buf[LH_BRIDGE_RADIO_BUF];
static lh_slip_decoder_t g_host_b_slip;

/** Radio hop: whatever bridge A transmits, bridge B receives, unchanged. */
static bool radio_link(void *user, const uint8_t *data, uint16_t len) {
  (void)user;
  lh_bridge_on_radio_frame(&g_bridge_b, data, len);
  return true;
}

/** Bridge B's serial port, with a host at the other end doing SLIP decoding. */
static bool host_b_serial(void *user, const uint8_t *data, uint16_t len) {
  (void)user;
  for (uint16_t i = 0; i < len; i++) {
    if (lh_slip_feed(&g_host_b_slip, data[i]) == LH_SLIP_FRAME_READY) {
      g_delivered_len = g_host_b_slip.len;
      memcpy(g_delivered, g_host_b_slip.buf, g_delivered_len);
      g_delivered_count++;
    }
  }
  return true;
}

/** Bridge A never emits upstream in this rig, and bridge B never transmits. */
static bool discard(void *user, const uint8_t *data, uint16_t len) {
  (void)user;
  (void)data;
  (void)len;
  return true;
}

static void rig_init(void) {
  lh_bridge_init(&g_bridge_a, radio_link, NULL, discard, NULL);
  lh_bridge_init(&g_bridge_b, discard, NULL, host_b_serial, NULL);
  lh_slip_init(&g_host_b_slip, g_host_b_buf, (uint16_t)sizeof g_host_b_buf);
  g_delivered_len = 0;
  g_delivered_count = 0;
}

/** Builds a valid frame with a random payload. Returns its total length. */
static uint16_t make_frame(uint8_t *out, uint16_t payload_len, uint8_t seq) {
  const lorahome_header_t header = {
      LORAHOME_FRAME_TELEMETRY, 0x0102, 0x0304, seq, LORAHOME_FLAG_NONE,
  };
  static uint8_t payload[LH_BRIDGE_RADIO_BUF];
  for (uint16_t i = 0; i < payload_len; i++) payload[i] = (uint8_t)next_random();

  const int len =
      lorahome_encode_frame(&header, payload, payload_len, out, LH_BRIDGE_RADIO_BUF);
  return len < 0 ? 0 : (uint16_t)len;
}

/** Wraps a frame in SLIP, as the Host would before putting it on the wire. */
static uint16_t wrap(const uint8_t *frame, uint16_t len, uint8_t *out, uint16_t cap) {
  return lh_slip_encode(frame, len, out, cap);
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                      */
/* ------------------------------------------------------------------------- */

static void test_validation_verdicts(void) {
  uint8_t frame[LH_BRIDGE_RADIO_BUF];
  const uint16_t len = make_frame(frame, 32, 1);

  CHECK(lh_bridge_validate(frame, len) == LH_BRIDGE_ACCEPT, "a well-formed frame must be accepted");

  /* Shorter than header+CRC. A CRC check would also fail here, but calling it a
   * CRC error would point the reader at an RF problem that does not exist. */
  CHECK(lh_bridge_validate(frame, 9) == LH_BRIDGE_REJECT_LEN, "a 9 byte frame is a length error");
  CHECK(lh_bridge_validate(frame, LH_BRIDGE_RADIO_BUF + 1) == LH_BRIDGE_REJECT_LEN,
        "an oversized frame is a length error");

  uint8_t alien[64];
  memcpy(alien, frame, len);
  alien[0] = 0x00;
  CHECK(lh_bridge_validate(alien, len) == LH_BRIDGE_REJECT_MAGIC,
        "somebody else's traffic is a magic error, not a CRC error");

  uint8_t corrupt[LH_BRIDGE_RADIO_BUF];
  memcpy(corrupt, frame, len);
  corrupt[12] ^= 0x01;
  CHECK(lh_bridge_validate(corrupt, len) == LH_BRIDGE_REJECT_CRC, "a flipped bit is a CRC error");

  /* T2.2: the ceiling is the MTU, not the receive buffer. One byte over is a
   * length error even though the buffer would have held it. */
  CHECK(lh_bridge_validate(frame, LORAHOME_MAX_FRAME_SIZE + 1) == LH_BRIDGE_REJECT_LEN,
        "a frame past the MTU is a length error");

  /* An unknown type is relayed. The Bridge is not the component that decides
   * which frame types exist — a relay that refuses tomorrow's types is a relay
   * that has to be reflashed before the network can be extended. */
  uint8_t future[LH_BRIDGE_RADIO_BUF];
  const lorahome_header_t future_header = {0x7F, 0x0102, 0x0304, 2, LORAHOME_FLAG_NONE};
  const int future_len =
      lorahome_encode_frame(&future_header, NULL, 0, future, sizeof future);
  CHECK(future_len > 0, "could not build a frame of an unknown type");
  CHECK(lh_bridge_validate(future, (uint16_t)future_len) == LH_BRIDGE_ACCEPT,
        "an unknown frame type must still be forwarded");
}

/**
 * The headline requirement: a frame with a broken CRC must be refused before it
 * reaches the air, and it must be counted.
 *
 * At this profile one full frame costs 1147.9 ms of a duty cycle that allows
 * about one such frame every two minutes. Transmitting a frame already known to
 * be broken is the single most expensive thing this component could do.
 */
static void test_corrupt_frame_never_reaches_the_air(void) {
  rig_init();

  uint8_t frame[LH_BRIDGE_RADIO_BUF];
  uint8_t wire[LH_BRIDGE_TX_SERIAL_BUF];

  const uint16_t len = make_frame(frame, 64, 7);
  frame[20] ^= 0x80; /* corrupt the payload, leave the CRC as it was */

  const uint16_t wire_len = wrap(frame, len, wire, (uint16_t)sizeof wire);
  lh_bridge_feed_serial(&g_bridge_a, wire, wire_len);

  CHECK(g_bridge_a.stats.serial_frames_in == 1, "the frame should have been seen");
  CHECK(g_bridge_a.stats.rejected_crc == 1, "it should be counted as a CRC rejection");
  CHECK(g_bridge_a.stats.radio_frames_out == 0, "IT MUST NOT HAVE BEEN TRANSMITTED");
  CHECK(g_delivered_count == 0, "and nothing should have reached the far host");
}

static void test_alien_and_short_frames_are_refused(void) {
  rig_init();

  uint8_t frame[LH_BRIDGE_RADIO_BUF];
  uint8_t wire[LH_BRIDGE_TX_SERIAL_BUF];

  const uint16_t len = make_frame(frame, 16, 3);
  frame[0] = 0x7E;
  uint16_t wire_len = wrap(frame, len, wire, (uint16_t)sizeof wire);
  lh_bridge_feed_serial(&g_bridge_a, wire, wire_len);
  CHECK(g_bridge_a.stats.rejected_magic == 1, "bad magic should be counted separately");

  const uint8_t runt[4] = {LORAHOME_MAGIC_VER, 0x20, 0x00, 0x01};
  wire_len = wrap(runt, sizeof runt, wire, (uint16_t)sizeof wire);
  lh_bridge_feed_serial(&g_bridge_a, wire, wire_len);
  CHECK(g_bridge_a.stats.rejected_len == 1, "a runt should be counted as a length rejection");

  CHECK(g_bridge_a.stats.radio_frames_out == 0, "neither should have been transmitted");
}

/** A duty-cycle veto stops a perfectly good frame, and says why. */
static bool always_refuse(void *user, uint16_t len) {
  (void)user;
  (void)len;
  return false;
}

static void test_duty_cycle_veto(void) {
  rig_init();
  lh_bridge_set_duty_cycle_guard(&g_bridge_a, always_refuse, NULL);

  uint8_t frame[LH_BRIDGE_RADIO_BUF];
  uint8_t wire[LH_BRIDGE_TX_SERIAL_BUF];
  const uint16_t len = make_frame(frame, 32, 9);
  const uint16_t wire_len = wrap(frame, len, wire, (uint16_t)sizeof wire);

  lh_bridge_feed_serial(&g_bridge_a, wire, wire_len);

  CHECK(g_bridge_a.stats.rejected_duty_cycle == 1, "the veto should be counted as its own reason");
  CHECK(g_bridge_a.stats.rejected_crc == 0, "a legal frame is not a corrupt one");
  CHECK(g_bridge_a.stats.radio_frames_out == 0, "and it must not have gone out");
}

/**
 * 1000 frames, host to host, compared byte for byte.
 *
 * Lengths span the whole range including the maximum, and payloads are random,
 * so the SLIP escape path is exercised by content rather than by a special case.
 */
static long test_end_to_end(int frames) {
  rig_init();

  uint8_t frame[LH_BRIDGE_RADIO_BUF];
  uint8_t wire[LH_BRIDGE_TX_SERIAL_BUF];
  long losses = 0;

  for (int i = 0; i < frames; i++) {
    /* Bounded by the MTU rather than by the receive buffer: as of T2.2 the
     * frame path enforces one length limit, and it is the one the radio profile
     * can actually carry. The buffer is 256 B and simply has slack. */
    const uint16_t payload_len = (uint16_t)(next_random() % (LORAHOME_MAX_PAYLOAD + 1));
    const uint16_t len = make_frame(frame, payload_len, (uint8_t)i);
    if (len == 0) {
      CHECK(0, "could not build a frame of payload %u", (unsigned)payload_len);
      continue;
    }

    const uint32_t before = g_delivered_count;
    const uint16_t wire_len = wrap(frame, len, wire, (uint16_t)sizeof wire);
    lh_bridge_feed_serial(&g_bridge_a, wire, wire_len);

    if (g_delivered_count != before + 1) {
      losses++;
      continue;
    }
    if (g_delivered_len != len || memcmp(g_delivered, frame, len) != 0) {
      losses++;
    }
  }

  CHECK(losses == 0, "%ld of %d frames were lost or altered end to end", losses, frames);
  CHECK((long)g_bridge_a.stats.radio_frames_out == (long)frames, "bridge A did not transmit all");
  CHECK((long)g_bridge_b.stats.serial_frames_out == (long)frames, "bridge B did not deliver all");
  return losses;
}

/**
 * A full escape-worst-case frame through the whole path.
 *
 * This is the one that would have found the 512-vs-514 byte transmit buffer:
 * every payload byte doubles, and two bytes of shortfall is exactly enough to
 * fail on a maximum-length frame (risk R1.1).
 */
static void test_worst_case_frame_survives_the_path(void) {
  rig_init();

  const uint16_t payload_len = LORAHOME_MAX_PAYLOAD;
  static uint8_t payload[LH_BRIDGE_RADIO_BUF];
  for (uint16_t i = 0; i < payload_len; i++) payload[i] = (i & 1) ? LH_SLIP_ESC : LH_SLIP_END;

  const lorahome_header_t header = {LORAHOME_FRAME_CONFIG_FRAG, 0xC0DB, 0xDBC0, 0xC0,
                                    LORAHOME_FLAG_FRAG};
  uint8_t frame[LH_BRIDGE_RADIO_BUF];
  const int len = lorahome_encode_frame(&header, payload, payload_len, frame, sizeof frame);
  CHECK(len > 0, "could not build the worst-case frame");
  if (len <= 0) return;

  uint8_t wire[LH_BRIDGE_TX_SERIAL_BUF];
  const uint16_t wire_len = wrap(frame, (uint16_t)len, wire, (uint16_t)sizeof wire);
  CHECK(wire_len > 0, "the host could not SLIP-encode the worst-case frame");

  lh_bridge_feed_serial(&g_bridge_a, wire, wire_len);

  CHECK(g_delivered_count == 1, "the worst-case frame did not survive the path");
  CHECK(g_delivered_len == (uint16_t)len && memcmp(g_delivered, frame, (size_t)len) == 0,
        "the worst-case frame arrived altered");
  CHECK(g_bridge_b.stats.serial_tx_errors == 0, "bridge B could not encode it for the host");
}

/* ------------------------------------------------------------------------- */
/* Bridge diagnostics (BRIDGE_STAT_REQ/RSP)                                   */
/* ------------------------------------------------------------------------- */

static void fake_health(void *user, lh_bridge_stat_t *out) {
  (void)user;
  out->uptime_ms = 123456u;
  out->heap_free_internal = 196108u;
  out->heap_largest_block = 172032u;
  out->heap_min_free_ever = 190000u;
  out->ring_overrun = 7u;
  out->ring_hwm = 640u;
  out->ring_capacity = 2048u;
}

/** Builds a BRIDGE_STAT_REQ addressed to the bridge itself. */
static uint16_t make_stat_request(uint8_t *out, uint8_t seq) {
  const lorahome_header_t header = {
      (uint8_t)LORAHOME_FRAME_BRIDGE_STAT_REQ, 0x0001, LORAHOME_BRIDGE_ID, seq,
      LORAHOME_FLAG_NONE,
  };
  const int len = lorahome_encode_frame(&header, NULL, 0, out, LH_BRIDGE_RADIO_BUF);
  return len < 0 ? 0 : (uint16_t)len;
}

/**
 * A request addressed to the Bridge is answered, not transmitted.
 *
 * This is what makes the 24-hour soak able to report heap drift at all: the
 * Bridge has no display, no network stack and a serial port carrying frames
 * rather than a console, so without this path its memory is unobservable and
 * the release-blocking number can never be measured.
 */
static void test_stat_request_is_answered_locally(void) {
  rig_init();
  lh_bridge_set_health_source(&g_bridge_a, fake_health, NULL);

  uint8_t frame[LH_BRIDGE_RADIO_BUF];
  uint8_t wire[LH_BRIDGE_TX_SERIAL_BUF];
  const uint16_t len = make_stat_request(frame, 0x2A);
  const uint16_t wire_len = wrap(frame, len, wire, (uint16_t)sizeof wire);

  /* Bridge A's to_host is the discard sink in this rig, so capture through the
   * far-side decoder instead: point A's host output at the same SLIP decoder. */
  lh_bridge_init(&g_bridge_a, radio_link, NULL, host_b_serial, NULL);
  lh_bridge_set_health_source(&g_bridge_a, fake_health, NULL);
  lh_bridge_feed_serial(&g_bridge_a, wire, wire_len);

  CHECK(g_bridge_a.stats.local_requests == 1, "the request should have been answered locally");
  CHECK(g_bridge_a.stats.radio_frames_out == 0, "A DIAGNOSTIC MUST NEVER GO ON AIR");
  CHECK(g_delivered_count == 1, "no reply reached the host");
  if (g_delivered_count != 1) return;

  lorahome_header_t reply;
  const uint8_t *payload = NULL;
  size_t payload_len = 0;
  CHECK(lorahome_decode_frame(g_delivered, g_delivered_len, &reply, &payload, &payload_len),
        "the reply is not a valid frame");
  CHECK(reply.type == (uint8_t)LORAHOME_FRAME_BRIDGE_STAT_RSP, "wrong reply type");
  CHECK(reply.seq == 0x2A, "the reply must echo the request's seq");
  CHECK(reply.dst_id == 0x0001, "the reply must go back to the requester");
  CHECK(payload_len == LH_BRIDGE_STAT_SIZE, "payload is %u B, expected %u",
        (unsigned)payload_len, (unsigned)LH_BRIDGE_STAT_SIZE);

  lh_bridge_stat_t stat;
  CHECK(lh_bridge_stat_decode(payload, (uint16_t)payload_len, &stat), "payload did not decode");
  CHECK(stat.heap_free_internal == 196108u, "heap_free did not survive the round trip");
  CHECK(stat.heap_largest_block == 172032u, "largest_block did not survive the round trip");
  CHECK(stat.ring_hwm == 640u, "ring hwm did not survive the round trip");
  CHECK(stat.serial_frames_in == 1u, "the core's own counters should be included");
}

/** Without a health source the counters still come back — zeroes, not silence. */
static void test_stat_request_answered_without_health_source(void) {
  rig_init();
  lh_bridge_init(&g_bridge_a, radio_link, NULL, host_b_serial, NULL);

  uint8_t frame[LH_BRIDGE_RADIO_BUF];
  uint8_t wire[LH_BRIDGE_TX_SERIAL_BUF];
  const uint16_t len = make_stat_request(frame, 1);
  lh_bridge_feed_serial(&g_bridge_a, wire, wrap(frame, len, wire, (uint16_t)sizeof wire));

  // A Host reading zero free heap will notice. A Host receiving nothing cannot
  // tell a silent bridge from an unconfigured one.
  CHECK(g_delivered_count == 1, "a bridge with no health source should still answer");
}

/** The same type addressed elsewhere is ordinary traffic and goes on air. */
static void test_stat_type_addressed_to_a_node_is_forwarded(void) {
  rig_init();

  const lorahome_header_t header = {
      (uint8_t)LORAHOME_FRAME_BRIDGE_STAT_REQ, 0x0001, 0x0042, 3, LORAHOME_FLAG_NONE,
  };
  uint8_t frame[LH_BRIDGE_RADIO_BUF];
  const int len = lorahome_encode_frame(&header, NULL, 0, frame, sizeof frame);
  CHECK(len > 0, "could not build the frame");
  if (len <= 0) return;

  uint8_t wire[LH_BRIDGE_TX_SERIAL_BUF];
  lh_bridge_feed_serial(&g_bridge_a, wire,
                        wrap(frame, (uint16_t)len, wire, (uint16_t)sizeof wire));

  CHECK(g_bridge_a.stats.local_requests == 0, "only the bridge's own address is intercepted");
  CHECK(g_bridge_a.stats.radio_frames_out == 1, "it should have been forwarded normally");
}

/** Every field survives encode/decode, including the extremes. */
static void test_stat_codec_round_trip(void) {
  lh_bridge_stat_t original;
  uint8_t *raw = (uint8_t *)&original;
  for (size_t i = 0; i < sizeof original; i++) raw[i] = 0;

  original.uptime_ms = 0xFFFFFFFFu;
  original.heap_free_internal = 0u;
  original.heap_largest_block = 0x7FFFFFFFu;
  original.heap_min_free_ever = 1u;
  original.serial_frames_in = 0x01020304u;
  original.serial_frames_out = 0x05060708u;
  original.radio_frames_in = 0x090A0B0Cu;
  original.radio_frames_out = 0x0D0E0F10u;
  original.rejected_crc = 0x11121314u;
  original.rejected_magic = 0x15161718u;
  original.rejected_len = 0x191A1B1Cu;
  original.rejected_duty_cycle = 0x1D1E1F20u;
  original.radio_tx_errors = 0x21222324u;
  original.serial_tx_errors = 0x25262728u;
  original.slip_frames_ok = 0x292A2B2Cu;
  original.slip_overflow = 0x2D2E2F30u;
  original.slip_bad_escape = 0x31323334u;
  original.slip_dropped = 0x35363738u;
  original.ring_overrun = 0x393A3B3Cu;
  original.ring_hwm = 0xFFFFu;
  original.ring_capacity = 2048u;

  uint8_t buf[LH_BRIDGE_STAT_SIZE];
  CHECK(lh_bridge_stat_encode(&original, buf, (uint16_t)sizeof buf) == LH_BRIDGE_STAT_SIZE,
        "encode did not write the declared size");

  /* Big-endian on the wire, like the header. Mixing byte orders inside one
   * protocol is how R0.2 gets in through a side door. */
  CHECK(buf[0] == 0x00 && buf[1] == 0x01, "version must be big-endian");

  lh_bridge_stat_t decoded;
  CHECK(lh_bridge_stat_decode(buf, (uint16_t)sizeof buf, &decoded), "decode failed");
  CHECK(decoded.uptime_ms == original.uptime_ms, "uptime drift");
  CHECK(decoded.heap_largest_block == original.heap_largest_block, "largest_block drift");
  CHECK(decoded.ring_hwm == original.ring_hwm, "ring_hwm drift");
  CHECK(decoded.slip_dropped == original.slip_dropped, "slip_dropped drift");
  CHECK(decoded.ring_capacity == original.ring_capacity, "ring_capacity drift");

  /* A short buffer must be refused rather than half-written. */
  CHECK(lh_bridge_stat_encode(&original, buf, LH_BRIDGE_STAT_SIZE - 1) == 0,
        "encode should refuse a short buffer");
  CHECK(!lh_bridge_stat_decode(buf, LH_BRIDGE_STAT_SIZE - 1, &decoded),
        "decode should refuse a short buffer");
}

/* ------------------------------------------------------------------------- */
/* Benchmark: the bridge's own overhead, with no airtime in it                */
/* ------------------------------------------------------------------------- */

#define ROUNDS 60
#define BATCH 2000

static volatile uint32_t g_sink = 0;

/** Microseconds to move one frame host-side-in to radio-out, p50 and p99. */
static void bench_overhead(double *p50, double *p99) {
  uint8_t frame[LH_BRIDGE_RADIO_BUF];
  uint8_t wire[LH_BRIDGE_TX_SERIAL_BUF];
  double samples[ROUNDS];

  rig_init();
  /* A full-MTU frame: the most expensive one the path can legally carry, which
   * is what an overhead budget should be measured against. */
  const uint16_t len = make_frame(frame, LORAHOME_MAX_PAYLOAD, 0);
  const uint16_t wire_len = wrap(frame, len, wire, (uint16_t)sizeof wire);

  for (int i = 0; i < BATCH; i++) lh_bridge_feed_serial(&g_bridge_a, wire, wire_len);

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) lh_bridge_feed_serial(&g_bridge_a, wire, wire_len);
    samples[round] = (now_seconds() - started) * 1e6 / BATCH;
    g_sink += g_delivered_len;
  }

  qsort(samples, ROUNDS, sizeof(double), compare_double);
  *p50 = samples[ROUNDS / 2];
  *p99 = samples[(int)(ROUNDS * 0.99) >= ROUNDS ? ROUNDS - 1 : (int)(ROUNDS * 0.99)];
}

/* ------------------------------------------------------------------------- */

int main(void) {
#if defined(LH_SANITIZED)
  printf("LH_ENV bridge.selftest.build=sanitized\n");
#else
  printf("LH_ENV bridge.selftest.build=plain\n");
#endif

  test_validation_verdicts();
  test_corrupt_frame_never_reaches_the_air();
  test_alien_and_short_frames_are_refused();
  test_duty_cycle_veto();
  test_worst_case_frame_survives_the_path();
  test_stat_codec_round_trip();
  test_stat_request_is_answered_locally();
  test_stat_request_answered_without_health_source();
  test_stat_type_addressed_to_a_node_is_forwarded();

  const int frames = 1000;
  const long losses = test_end_to_end(frames);

  double overhead_p50 = 0.0;
  double overhead_p99 = 0.0;
  bench_overhead(&overhead_p50, &overhead_p99);

  printf("LH_METRIC test.bridge.checks value=%d unit=count\n", g_checks);
  printf("LH_METRIC test.bridge.failures value=%d unit=count budget=0\n", g_failures);
  printf("LH_METRIC test.bridge.e2e_frames value=%d unit=count budget=%d\n", frames, frames);
  printf("LH_METRIC metric.frame_loss_1000.native value=%ld unit=count budget=0\n", losses);
  printf("LH_METRIC mem.bridge_ctx.struct value=%u unit=B budget=2560\n",
         (unsigned)sizeof(lh_bridge_ctx_t));

  /* The roadmap's 5 ms budget is for the bridge's processing excluding airtime,
   * on target. This is the same work on a host CPU, so the budget is not
   * attached to it — but it does establish that the logic is nowhere near
   * being the constraint. */
  printf("LH_METRIC bench.bridge.overhead.native.p50 value=%.3f unit=us\n", overhead_p50);
  printf("LH_METRIC bench.bridge.overhead.native.p99 value=%.3f unit=us\n", overhead_p99);

  /*
   * The on-hardware half of T1.4. End-to-end latency is dominated by airtime —
   * 1147.9 ms at this profile — so the roadmap's 402 ms p50 target is not
   * reachable at SF9; it is an SF7 figure. Left SKIPPED with no budget rather
   * than restated, because changing a target is the architect's call.
   */
  printf("LH_METRIC bench.bridge.overhead.ms value=SKIPPED unit=ms budget=5"
         " (no target hardware attached)\n");
  printf("LH_METRIC bench.e2e.latency.p50.ms value=SKIPPED unit=ms"
         " (needs two boards; airtime alone is 1147.9 ms at SF9)\n");
  printf("LH_METRIC bench.e2e.latency.p99.ms value=SKIPPED unit=ms (needs two boards)\n");
  printf("LH_METRIC metric.frame_loss_1000 value=SKIPPED unit=count budget=0"
         " (needs two boards and a 30 dB attenuator)\n");
  printf("LH_METRIC heap.free.steady_state value=SKIPPED unit=B budget=180000"
         " (no target hardware attached)\n");
  printf("LH_METRIC heap.largest_block value=SKIPPED unit=B budget=160000"
         " (no target hardware attached)\n");

  if (g_sink == 0xFFFFFFFFu) printf("unreachable %u\n", g_sink);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
