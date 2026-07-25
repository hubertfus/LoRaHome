/*
 * Host-native SLIP harness for cross-language verification (T1.5).
 *
 *   slip_cli encode   reads hex payloads, writes hex SLIP frames
 *   slip_cli decode   reads hex SLIP frames, writes hex payloads
 *
 * One buffer per line, in and out. A line that decodes to no frame produces an
 * empty line, so input and output stay aligned one-to-one.
 *
 * Deliberately dumb for the same reason crc16_cli.c is: the TypeScript side
 * owns the corpus and pipes it in, so the only thing under test is the codec.
 * If the two languages disagree, the harness is not a plausible suspect.
 */
#include <stdio.h>
#include <string.h>

#include "lorahome/slip.h"

#define MAX_PAYLOAD 512
#define MAX_ENCODED (2 * MAX_PAYLOAD + 2)
#define MAX_LINE (2 * MAX_ENCODED + 4)

static int hex_value(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/** Returns the byte count, or -1 on a malformed line. */
static int unhex(const char *line, size_t len, uint8_t *out, size_t out_cap) {
  if (len % 2 != 0 || len / 2 > out_cap) return -1;

  size_t count = 0;
  for (size_t i = 0; i < len; i += 2) {
    const int hi = hex_value((unsigned char)line[i]);
    const int lo = hex_value((unsigned char)line[i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[count++] = (uint8_t)((hi << 4) | lo);
  }
  return (int)count;
}

static void print_hex(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) printf("%02X", data[i]);
  printf("\n");
}

int main(int argc, char **argv) {
  if (argc != 2 || (strcmp(argv[1], "encode") != 0 && strcmp(argv[1], "decode") != 0)) {
    fprintf(stderr, "usage: slip_cli encode|decode\n");
    return 2;
  }
  const int encoding = strcmp(argv[1], "encode") == 0;

  static char line[MAX_LINE + 2];
  static uint8_t input[MAX_ENCODED];
  static uint8_t encoded[MAX_ENCODED];
  static uint8_t decode_buf[MAX_PAYLOAD];

  lh_slip_decoder_t dec;

  while (fgets(line, (int)sizeof line, stdin) != NULL) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

    const int count = unhex(line, len, input, sizeof input);
    if (count < 0) {
      fprintf(stderr, "bad hex line of length %zu\n", len);
      return 1;
    }

    if (encoding) {
      const uint16_t written =
          lh_slip_encode(input, (uint16_t)count, encoded, (uint16_t)sizeof encoded);
      print_hex(encoded, written);
      continue;
    }

    /* A fresh decoder per line: each input line is one self-contained frame,
     * and carrying state between them would make a failure in one line show up
     * as a failure in the next. */
    lh_slip_init(&dec, decode_buf, (uint16_t)sizeof decode_buf);
    size_t produced = 0;
    for (int i = 0; i < count; i++) {
      if (lh_slip_feed(&dec, input[i]) == LH_SLIP_FRAME_READY) produced = dec.len;
    }
    print_hex(dec.buf, produced);
  }

  return 0;
}
