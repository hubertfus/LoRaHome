/*
 * Host-native capability harness for cross-language verification (T3.5).
 *
 *   capability_cli encode   each line "<fw_hex> <heap_hex> <type:addr:bus:ch:flags>,..." ->
 *                           one line of encoded CBOR in hex
 *   capability_cli decode   each line "<cbor_hex>" ->
 *                           one line "<fw_hex> <heap_hex> <type:addr:bus:ch:flags>,..."
 *                           or "ERR"
 *
 * One line in, one line out, so the TypeScript side can compare positionally.
 * Deliberately dumb, like frag_cli.c: the corpus lives on the TypeScript side
 * and the only thing under test here is the codec.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/capability.h"

#define MAX_LINE 1024

static char g_line[MAX_LINE + 2];
static uint8_t g_buf[LH_CAP_WIRE_BUDGET * 4];

static int hex_value(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int unhex(const char *text, size_t len, uint8_t *out, size_t out_cap) {
  if (len % 2 != 0 || len / 2 > out_cap) return -1;
  size_t count = 0;
  for (size_t i = 0; i < len; i += 2) {
    const int hi = hex_value((unsigned char)text[i]);
    const int lo = hex_value((unsigned char)text[i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[count++] = (uint8_t)((hi << 4) | lo);
  }
  return (int)count;
}

static void print_hex(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) printf("%02X", data[i]);
}

/** Reads one line into g_line with the newline stripped. */
static size_t read_line(void) {
  if (fgets(g_line, (int)sizeof g_line, stdin) == NULL) return 0;
  size_t len = strlen(g_line);
  while (len > 0 && (g_line[len - 1] == '\n' || g_line[len - 1] == '\r')) g_line[--len] = '\0';
  return len;
}

/** Parses "type:addr:bus:channels:flags,..." into a report's component list. */
static bool parse_components(char *text, lh_cap_report_t *report) {
  report->count = 0;
  if (text == NULL || *text == '\0') return true;

  char *save = text;
  while (save != NULL && *save != '\0') {
    char *comma = strchr(save, ',');
    if (comma != NULL) *comma = '\0';

    if (report->count >= LH_CAP_MAX) return false;
    lh_capability_t *entry = &report->caps[report->count];

    unsigned type_id = 0;
    unsigned addr = 0;
    unsigned bus = 0;
    unsigned channels = 0;
    unsigned flags = 0;
    if (sscanf(save, "%u:%u:%u:%u:%u", &type_id, &addr, &bus, &channels, &flags) != 5) {
      return false;
    }
    entry->driver_type_id = (uint16_t)type_id;
    entry->bus_addr = (uint8_t)addr;
    entry->bus_type = (uint8_t)bus;
    entry->channel_count = (uint8_t)channels;
    entry->flags = (uint8_t)flags;
    report->count++;

    save = (comma == NULL) ? NULL : comma + 1;
  }
  return true;
}

static void print_report(const lh_cap_report_t *report) {
  printf("%X %X ", (unsigned)report->fw_version, (unsigned)report->free_heap_kb);
  for (uint8_t i = 0; i < report->count; i++) {
    if (i > 0) printf(",");
    printf("%u:%u:%u:%u:%u", (unsigned)report->caps[i].driver_type_id,
           (unsigned)report->caps[i].bus_addr, (unsigned)report->caps[i].bus_type,
           (unsigned)report->caps[i].channel_count, (unsigned)report->caps[i].flags);
  }
  printf("\n");
}

static int run_encode(void) {
  size_t len;
  while ((len = read_line()) > 0) {
    lh_cap_report_t report;
    memset(&report, 0, sizeof report);

    char *first = strchr(g_line, ' ');
    if (first == NULL) {
      printf("ERR\n");
      continue;
    }
    *first = '\0';
    char *second = strchr(first + 1, ' ');
    if (second == NULL) {
      printf("ERR\n");
      continue;
    }
    *second = '\0';

    report.fw_version = (uint32_t)strtoul(g_line, NULL, 16);
    report.free_heap_kb = (uint16_t)strtoul(first + 1, NULL, 16);
    if (!parse_components(second + 1, &report)) {
      printf("ERR\n");
      continue;
    }

    const int written = lh_cap_encode(&report, g_buf, (uint16_t)sizeof g_buf);
    if (written < 0) {
      printf("ERR\n");
      continue;
    }
    print_hex(g_buf, (size_t)written);
    printf("\n");
  }
  return 0;
}

static int run_decode(void) {
  size_t len;
  while ((len = read_line()) > 0) {
    const int bytes = unhex(g_line, len, g_buf, sizeof g_buf);
    if (bytes < 0) {
      printf("ERR\n");
      continue;
    }

    lh_cap_report_t report;
    if (!lh_cap_decode(g_buf, (uint16_t)bytes, &report)) {
      printf("ERR\n");
      continue;
    }
    print_report(&report);
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: capability_cli encode|decode\n");
    return 2;
  }
  if (strcmp(argv[1], "encode") == 0) return run_encode();
  if (strcmp(argv[1], "decode") == 0) return run_decode();
  fprintf(stderr, "unknown mode: %s\n", argv[1]);
  return 2;
}
