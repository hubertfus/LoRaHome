/*
 * Native harness for the CBOR codec and capability report (T3.5).
 *
 * The cross-language check in tools/check-capability-cross.mjs proves the two
 * implementations agree on well-formed reports. This proves the C decoder
 * survives ill-formed ones, which is the case that matters operationally: the
 * input is a radio frame, and a radio frame can arrive corrupted, truncated,
 * or from a device that is faulty in a way nobody designed for.
 *
 * The fuzz loop is the point of the file. A decoder that walks past its buffer
 * on a crafted length field is a remote memory disclosure on a device sitting
 * in somebody's wall, and no round-trip test will ever find it — the malformed
 * input never appears in a corpus built from valid reports. Run under ASAN by
 * tools/run-native.mjs, an out-of-bounds read is a failure rather than a value.
 *
 * Output is LH_METRIC lines per roadmap §0.4.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/capability.h"
#include "lorahome/cbor.h"

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

static uint32_t g_rng = 0x2f6e1b93u;
static uint32_t next_random(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

/* ------------------------------------------------------------------------- */
/* CBOR primitives                                                           */
/* ------------------------------------------------------------------------- */

/** Canonical widths at every seam. The cross-language byte check rests on this. */
static void test_canonical_widths(void) {
  const struct {
    uint64_t value;
    const char *hex;
    uint8_t len;
  } cases[] = {
      {0, "\x00", 1},
      {23, "\x17", 1},
      {24, "\x18\x18", 2},
      {255, "\x18\xFF", 2},
      {256, "\x19\x01\x00", 3},
      {65535, "\x19\xFF\xFF", 3},
      {65536, "\x1A\x00\x01\x00\x00", 5},
      {4294967295u, "\x1A\xFF\xFF\xFF\xFF", 5},
  };

  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    uint8_t buf[16];
    lh_cbor_writer_t writer;
    lh_cbor_writer_init(&writer, buf, sizeof buf);
    lh_cbor_write_uint(&writer, cases[i].value);

    const int written = lh_cbor_writer_finish(&writer);
    CHECK(written == cases[i].len, "uint %llu should be %u B, was %d",
          (unsigned long long)cases[i].value, (unsigned)cases[i].len, written);
    CHECK(written > 0 && memcmp(buf, cases[i].hex, (size_t)written) == 0,
          "uint %llu encoded to the wrong bytes", (unsigned long long)cases[i].value);
  }
}

/** Negative integers encode -1 - n, and INT64_MIN does not overflow on the way. */
static void test_negative_integers(void) {
  uint8_t buf[16];
  const int64_t values[] = {-1, -24, -25, -256, -65536, INT64_MIN};

  for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
    lh_cbor_writer_t writer;
    lh_cbor_writer_init(&writer, buf, sizeof buf);
    lh_cbor_write_int(&writer, values[i]);
    const int written = lh_cbor_writer_finish(&writer);
    CHECK(written > 0, "encoding %lld should succeed", (long long)values[i]);

    lh_cbor_reader_t reader;
    lh_cbor_reader_init(&reader, buf, (uint16_t)written);
    int64_t back = 0;
    CHECK(lh_cbor_read_int(&reader, &back), "decoding %lld should succeed",
          (long long)values[i]);
    CHECK(back == values[i], "%lld round-tripped to %lld", (long long)values[i], (long long)back);
  }
}

/** The writer records overflow rather than truncating into a plausible message. */
static void test_writer_overflow_is_sticky(void) {
  uint8_t buf[3];
  lh_cbor_writer_t writer;
  lh_cbor_writer_init(&writer, buf, sizeof buf);

  lh_cbor_write_uint(&writer, 1);
  lh_cbor_write_uint(&writer, 70000); /* needs 5 bytes; only 2 remain */
  lh_cbor_write_uint(&writer, 1);     /* would fit, but must not be emitted */

  CHECK(writer.overflow, "the writer should have recorded the overflow");
  CHECK(lh_cbor_writer_finish(&writer) < 0, "and finish must refuse to report a length");
}

/** Everything outside the subset is refused rather than guessed at. */
static void test_reader_refuses_the_rest_of_cbor(void) {
  const struct {
    const char *name;
    uint8_t byte;
  } refused[] = {
      {"indefinite bytes", 0x5F}, {"indefinite array", 0x9F}, {"double", 0xFB},
      {"tag", 0xC0},              {"reserved info 28", 0x1C},
  };

  for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
    lh_cbor_reader_t reader;
    lh_cbor_reader_init(&reader, &refused[i].byte, 1);
    uint64_t value = 0;
    CHECK(!lh_cbor_read_uint(&reader, &value) || reader.error, "%s should be refused",
          refused[i].name);
  }

  /* A declared length larger than the buffer cannot be honest — every item
   * costs at least a byte — and is refused at the header rather than driving a
   * caller's loop off the end. */
  const uint8_t huge_array[] = {0x9A, 0xFF, 0xFF, 0xFF, 0xFF};
  lh_cbor_reader_t reader;
  lh_cbor_reader_init(&reader, huge_array, sizeof huge_array);
  uint32_t items = 0;
  CHECK(!lh_cbor_read_array(&reader, &items), "an impossible array length should be refused");
}

/** Skip descends into containers, and refuses to descend for ever. */
static void test_skip(void) {
  uint8_t buf[64];
  lh_cbor_writer_t writer;
  lh_cbor_writer_init(&writer, buf, sizeof buf);
  lh_cbor_write_array(&writer, 2);
  lh_cbor_write_map(&writer, 1);
  lh_cbor_write_uint(&writer, 9);
  lh_cbor_write_array(&writer, 2);
  lh_cbor_write_uint(&writer, 1);
  lh_cbor_write_uint(&writer, 2);
  lh_cbor_write_uint(&writer, 77);
  const int written = lh_cbor_writer_finish(&writer);

  lh_cbor_reader_t reader;
  lh_cbor_reader_init(&reader, buf, (uint16_t)written);
  uint32_t items = 0;
  CHECK(lh_cbor_read_array(&reader, &items) && items == 2, "outer array");
  CHECK(lh_cbor_skip(&reader), "the nested map should be skipped whole");
  uint64_t tail = 0;
  CHECK(lh_cbor_read_uint(&reader, &tail) && tail == 77, "and the item after it read cleanly");

  /* Nesting past the cap is refused. A recursive skipper meeting this would
   * walk the stack off a task that has 3 kB of it. */
  uint8_t deep[32];
  for (size_t i = 0; i < sizeof deep - 1; i++) deep[i] = 0x81; /* array(1) */
  deep[sizeof deep - 1] = 0x00;

  lh_cbor_reader_t deep_reader;
  lh_cbor_reader_init(&deep_reader, deep, sizeof deep);
  CHECK(!lh_cbor_skip(&deep_reader), "nesting past the cap should be refused");
}

/* ------------------------------------------------------------------------- */
/* Capability reports                                                        */
/* ------------------------------------------------------------------------- */

static lh_cap_report_t sample_report(uint8_t count) {
  lh_cap_report_t report;
  memset(&report, 0, sizeof report);
  report.fw_version = 0x00040000u;
  report.free_heap_kb = 192;
  report.count = count;
  for (uint8_t i = 0; i < count; i++) {
    report.caps[i].driver_type_id = (uint16_t)(16 + (i % 2));
    report.caps[i].bus_addr = (uint8_t)(0x76 + (i % 2));
    report.caps[i].bus_type = (uint8_t)((i % 2) == 0 ? LH_BUS_I2C : LH_BUS_GPIO);
    report.caps[i].channel_count = (uint8_t)(4 - (i % 2) * 2);
    report.caps[i].flags = (uint8_t)(i % 2);
  }
  return report;
}

static uint16_t g_worst_case_bytes = 0;

static void test_report_round_trip(void) {
  for (uint8_t count = 0; count <= LH_CAP_MAX; count++) {
    const lh_cap_report_t report = sample_report(count);
    uint8_t buf[LH_CAP_WIRE_BUDGET * 2];

    const int written = lh_cap_encode(&report, buf, (uint16_t)sizeof buf);
    CHECK(written > 0, "encoding %u components should succeed", (unsigned)count);

    lh_cap_report_t decoded;
    CHECK(lh_cap_decode(buf, (uint16_t)written, &decoded), "decoding %u components", (unsigned)count);
    CHECK(lh_cap_equal(&report, &decoded), "%u components should survive the round trip",
          (unsigned)count);
  }
}

/**
 * The provable maximum, not a large example.
 *
 * Every field at the widest value its type allows: 12 B per component (1 array
 * header, 3 for a 16-bit type id, 2 each for four bytes that spill past the
 * inline form) plus 13 B of envelope, so 109 B.
 *
 * Written as 93 first, on the assumption that bus type, channel count and flags
 * would stay inline. They do in every report the system generates and they do
 * not in every report it can be sent — the decoder accepts a full byte in each
 * — and the bound that matters for a receive buffer is the one an adversary can
 * reach. The assertion below is what caught the arithmetic.
 */
static void test_wire_budget(void) {
  lh_cap_report_t worst;
  memset(&worst, 0, sizeof worst);
  worst.fw_version = 0xFFFFFFFFu;
  worst.free_heap_kb = 0xFFFFu;
  worst.count = LH_CAP_MAX;
  for (uint8_t i = 0; i < LH_CAP_MAX; i++) {
    worst.caps[i].driver_type_id = 0xFFFFu;
    worst.caps[i].bus_addr = 0xFFu;
    worst.caps[i].bus_type = 0xFFu;
    worst.caps[i].channel_count = 0xFFu;
    worst.caps[i].flags = 0xFFu;
  }

  uint8_t buf[LH_CAP_WIRE_BUDGET * 3];
  const int written = lh_cap_encode(&worst, buf, (uint16_t)sizeof buf);
  CHECK(written == (int)LH_CAP_WIRE_WORST_CASE, "the widest possible report should be %u B, was %d",
        (unsigned)LH_CAP_WIRE_WORST_CASE, written);
  g_worst_case_bytes = (uint16_t)written;

  lh_cap_report_t decoded;
  CHECK(lh_cap_decode(buf, (uint16_t)written, &decoded), "and decode");
  CHECK(lh_cap_equal(&worst, &decoded), "and match");

  /* A buffer one byte short must refuse rather than emit a truncated report
   * that decodes to something plausible. */
  uint8_t tight[LH_CAP_WIRE_WORST_CASE - 1];
  CHECK(lh_cap_encode(&worst, tight, (uint16_t)sizeof tight) < 0,
        "encoding one byte short should fail, not truncate");

  /*
   * The roadmap's 100 B budget, against the reports the system actually
   * produces: bus types from a five-value enum, channel counts in single
   * digits, one flag bit defined. This is the number the budget was written
   * for, and it is the one that constrains adding drivers.
   */
  lh_cap_report_t realistic;
  memset(&realistic, 0, sizeof realistic);
  realistic.fw_version = 0xFFFFFFFFu;
  realistic.free_heap_kb = 0xFFFFu;
  realistic.count = LH_CAP_MAX;
  for (uint8_t i = 0; i < LH_CAP_MAX; i++) {
    realistic.caps[i].driver_type_id = 0xFFFFu; /* the widest id ever allocatable */
    realistic.caps[i].bus_addr = 0x77u;
    realistic.caps[i].bus_type = LH_BUS_ADC; /* 4, the largest enum value */
    realistic.caps[i].channel_count = 8;
    realistic.caps[i].flags = LH_CAP_FLAG_CONFIGURED;
  }
  const int realistic_bytes = lh_cap_encode(&realistic, buf, (uint16_t)sizeof buf);
  CHECK(realistic_bytes > 0 && realistic_bytes <= (int)LH_CAP_WIRE_BUDGET,
        "a full realistic report is %d B against the %u B budget", realistic_bytes,
        (unsigned)LH_CAP_WIRE_BUDGET);
  printf("LH_METRIC size.cap_report.cbor.8comp value=%d unit=B budget=%u\n", realistic_bytes,
         (unsigned)LH_CAP_WIRE_BUDGET);
}

/** Unknown keys and extra component fields are stepped over, not refused. */
static void test_forward_compatibility(void) {
  uint8_t buf[128];
  lh_cbor_writer_t writer;
  lh_cbor_writer_init(&writer, buf, sizeof buf);

  lh_cbor_write_map(&writer, 4);
  lh_cbor_write_uint(&writer, LH_CAP_KEY_FW_VERSION);
  lh_cbor_write_uint(&writer, 0x00050000u);
  lh_cbor_write_uint(&writer, LH_CAP_KEY_FREE_HEAP_KB);
  lh_cbor_write_uint(&writer, 150);
  lh_cbor_write_uint(&writer, 99); /* a key from a newer protocol */
  lh_cbor_write_array(&writer, 2);
  lh_cbor_write_uint(&writer, 1);
  lh_cbor_write_uint(&writer, 2);
  lh_cbor_write_uint(&writer, LH_CAP_KEY_COMPONENTS);
  lh_cbor_write_array(&writer, 1);
  lh_cbor_write_array(&writer, 7); /* two fields this build does not know */
  lh_cbor_write_uint(&writer, 16);
  lh_cbor_write_uint(&writer, 0x76);
  lh_cbor_write_uint(&writer, 0);
  lh_cbor_write_uint(&writer, 4);
  lh_cbor_write_uint(&writer, 1);
  lh_cbor_write_uint(&writer, 123);
  lh_cbor_write_uint(&writer, 456);

  const int written = lh_cbor_writer_finish(&writer);
  CHECK(written > 0, "the synthetic message should build");

  lh_cap_report_t decoded;
  CHECK(lh_cap_decode(buf, (uint16_t)written, &decoded),
        "an older build must still decode a newer node's report");
  CHECK(decoded.fw_version == 0x00050000u, "fw version survived the unknown key");
  CHECK(decoded.count == 1 && decoded.caps[0].driver_type_id == 16,
        "and so did the component this build understands");
}

/** A report claiming more components than the node can hold is refused. */
static void test_over_capacity_is_refused(void) {
  uint8_t buf[256];
  lh_cbor_writer_t writer;
  lh_cbor_writer_init(&writer, buf, sizeof buf);
  lh_cbor_write_map(&writer, 1);
  lh_cbor_write_uint(&writer, LH_CAP_KEY_COMPONENTS);
  lh_cbor_write_array(&writer, LH_CAP_MAX + 1);
  for (uint8_t i = 0; i <= LH_CAP_MAX; i++) {
    lh_cbor_write_array(&writer, LH_CAP_FIELDS);
    for (uint8_t f = 0; f < LH_CAP_FIELDS; f++) lh_cbor_write_uint(&writer, 1);
  }
  const int written = lh_cbor_writer_finish(&writer);

  lh_cap_report_t decoded;
  CHECK(!lh_cap_decode(buf, (uint16_t)written, &decoded),
        "nine components must be refused, not truncated to eight");
}

/* ------------------------------------------------------------------------- */
/* Fuzz                                                                      */
/* ------------------------------------------------------------------------- */

#define FUZZ_ITERATIONS 200000

/**
 * Random bytes through the decoder. Under ASAN, an out-of-bounds read fails.
 *
 * Two corpora, because they find different things. Uniform noise mostly gets
 * rejected at the first byte and exercises the reject paths; mutated valid
 * reports get deep into the decoder with one field wrong, which is where a
 * length taken on trust actually bites.
 */
static uint32_t fuzz_decoder(void) {
  uint8_t buf[LH_CAP_WIRE_BUDGET * 2];
  lh_cap_report_t decoded;
  uint32_t accepted = 0;

  for (int i = 0; i < FUZZ_ITERATIONS / 2; i++) {
    const uint16_t len = (uint16_t)(next_random() % (sizeof buf + 1));
    for (uint16_t b = 0; b < len; b++) buf[b] = (uint8_t)(next_random() >> 8);
    if (lh_cap_decode(buf, len, &decoded)) accepted++;
  }

  const lh_cap_report_t valid = sample_report(LH_CAP_MAX);
  for (int i = 0; i < FUZZ_ITERATIONS / 2; i++) {
    const int written = lh_cap_encode(&valid, buf, (uint16_t)sizeof buf);
    if (written <= 0) continue;

    /* One to three bytes corrupted, or the message truncated. */
    uint16_t len = (uint16_t)written;
    const uint32_t mutations = 1 + next_random() % 3;
    for (uint32_t m = 0; m < mutations; m++) {
      buf[next_random() % (uint32_t)written] = (uint8_t)(next_random() >> 8);
    }
    if ((next_random() & 3u) == 0) len = (uint16_t)(next_random() % (uint32_t)(written + 1));

    if (lh_cap_decode(buf, len, &decoded)) accepted++;
  }

  return accepted;
}

/* ------------------------------------------------------------------------- */
/* Benchmarks                                                                */
/* ------------------------------------------------------------------------- */

#define ROUNDS 60
#define BATCH 20000

static volatile int32_t g_sink = 0;

static double bench_encode_us(void) {
  double samples[ROUNDS];
  const lh_cap_report_t report = sample_report(LH_CAP_MAX);
  uint8_t buf[LH_CAP_WIRE_BUDGET * 2];

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      g_sink += lh_cap_encode(&report, buf, (uint16_t)sizeof buf);
    }
    samples[round] = (now_seconds() - started) * 1e6 / BATCH;
  }
  qsort(samples, ROUNDS, sizeof(double), compare_double);
  return samples[ROUNDS / 2];
}

static double bench_decode_us(void) {
  double samples[ROUNDS];
  const lh_cap_report_t report = sample_report(LH_CAP_MAX);
  uint8_t buf[LH_CAP_WIRE_BUDGET * 2];
  const int written = lh_cap_encode(&report, buf, (uint16_t)sizeof buf);
  lh_cap_report_t decoded;

  for (int round = 0; round < ROUNDS; round++) {
    const double started = now_seconds();
    for (int i = 0; i < BATCH; i++) {
      g_sink += lh_cap_decode(buf, (uint16_t)written, &decoded) ? 1 : 0;
    }
    samples[round] = (now_seconds() - started) * 1e6 / BATCH;
  }
  qsort(samples, ROUNDS, sizeof(double), compare_double);
  return samples[ROUNDS / 2];
}

/* ------------------------------------------------------------------------- */

int main(void) {
#if defined(LH_SANITIZED)
  printf("LH_ENV capability.selftest.build=sanitized\n");
#else
  printf("LH_ENV capability.selftest.build=plain\n");
#endif

  test_canonical_widths();
  test_negative_integers();
  test_writer_overflow_is_sticky();
  test_reader_refuses_the_rest_of_cbor();
  test_skip();
  test_report_round_trip();
  test_wire_budget();
  test_forward_compatibility();
  test_over_capacity_is_refused();

  const uint32_t fuzz_accepted = fuzz_decoder();

  printf("LH_METRIC test.capability.checks value=%d unit=count\n", g_checks);
  printf("LH_METRIC test.capability.failures value=%d unit=count budget=0\n", g_failures);
  printf("LH_METRIC fuzz.capability.iterations value=%d unit=count budget=%d\n", FUZZ_ITERATIONS,
         FUZZ_ITERATIONS);
  /* Not a pass/fail number. Reported because "the fuzzer accepted nothing"
   * would mean it never got past the first byte and was testing the reject path
   * only — a fuzz run that proves less than it appears to. */
  printf("LH_METRIC fuzz.capability.accepted value=%lu unit=count\n",
         (unsigned long)fuzz_accepted);

  /* Budgeted against the frame payload, not against the 100 B target: the
   * requirement is that a discovery response never fragments, and 109 B leaves
   * more than half the payload spare. The 100 B figure belongs to the realistic
   * report emitted above, which is what constrains adding drivers. */
  printf("LH_METRIC size.cap_report.cbor.worst_case value=%u unit=B budget=220\n",
         (unsigned)g_worst_case_bytes);
  printf("LH_METRIC mem.capability.report value=%u unit=B\n", (unsigned)sizeof(lh_cap_report_t));
  printf("LH_METRIC mem.capability.entry value=%u unit=B budget=6\n",
         (unsigned)sizeof(lh_capability_t));

  printf("LH_METRIC bench.cap_report.encode.native.p50 value=%.4f unit=us\n", bench_encode_us());
  printf("LH_METRIC bench.cap_report.decode.native.p50 value=%.4f unit=us\n", bench_decode_us());
  printf("LH_METRIC bench.cap_report.encode.esp32 value=SKIPPED unit=us budget=1500"
         " (no target hardware attached)\n");

  if (g_sink == 0x7FFFFFFF) printf("unreachable %ld\n", (long)g_sink);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
