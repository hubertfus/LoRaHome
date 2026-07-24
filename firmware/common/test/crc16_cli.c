/*
 * Host-native CRC16 harness for cross-language verification.
 *
 * Reads one hex-encoded buffer per line on stdin, writes the CRC of each as
 * four uppercase hex digits on stdout. An empty line means an empty buffer,
 * whose CRC is the init value — an edge case worth being able to express.
 *
 * Deliberately dumb: no JSON parsing in C. The TypeScript test owns the vector
 * file and pipes the data in, so the C side under test is nothing but
 * lorahome_crc16() plus hex decoding. That keeps the thing being verified small
 * enough that the harness itself is not a plausible source of disagreement.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lorahome/crc16.h"

#define MAX_LINE 1024
#define MAX_BYTES (MAX_LINE / 2)

static int hex_value(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int main(void) {
  char line[MAX_LINE + 2];
  uint8_t bytes[MAX_BYTES];

  while (fgets(line, (int)sizeof line, stdin) != NULL) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }

    if (len % 2 != 0 || len > MAX_LINE) {
      fprintf(stderr, "bad hex line of length %zu\n", len);
      return 1;
    }

    size_t count = 0;
    for (size_t i = 0; i < len; i += 2) {
      const int hi = hex_value((unsigned char)line[i]);
      const int lo = hex_value((unsigned char)line[i + 1]);
      if (hi < 0 || lo < 0) {
        fprintf(stderr, "bad hex digit at offset %zu\n", i);
        return 1;
      }
      bytes[count++] = (uint8_t)((hi << 4) | lo);
    }

    printf("%04X\n", lorahome_crc16(bytes, count));
  }

  return 0;
}
