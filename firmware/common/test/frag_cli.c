/*
 * Host-native fragmentation harness for cross-language verification (T2.3).
 *
 *   frag_cli split        each line "<cfg_id_hex> <config_hex>" ->
 *                         one line of comma-separated fragment payloads
 *   frag_cli reassemble   each line "<frag_hex>,<frag_hex>,..." (feed order) ->
 *                         one line "<result_code> <assembled_hex>"
 *
 * One line in, one line out, so the TypeScript side can compare positionally.
 * Deliberately dumb, like slip_cli.c and dedup_cli.c: the corpus lives on the
 * TypeScript side, and the only thing under test here is the codec.
 */
#include <stdio.h>
#include <string.h>

#include "lorahome/frag.h"

#define MAX_LINE (4 * (LH_FRAG_CONFIG_MAX + LH_FRAG_MAX_FRAGMENTS * 16) + 64)
#define FRAG_BUF (LH_FRAG_HDR_SIZE + LH_FRAG_PAYLOAD_MAX)

static int hex_value(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/** Returns the byte count, or -1 on a malformed field. */
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

static char g_line[MAX_LINE + 2];
static uint8_t g_config[LH_FRAG_CONFIG_MAX];
static uint8_t g_fragment[FRAG_BUF];
static lh_reassembler_t g_reasm;

static int run_split(void) {
  while (fgets(g_line, (int)sizeof g_line, stdin) != NULL) {
    size_t len = strlen(g_line);
    while (len > 0 && (g_line[len - 1] == '\n' || g_line[len - 1] == '\r')) g_line[--len] = '\0';
    if (len == 0) continue;

    char *space = strchr(g_line, ' ');
    if (space == NULL) {
      fprintf(stderr, "split: expected '<cfg_id> <config_hex>'\n");
      return 1;
    }
    *space = '\0';

    unsigned cfg_id = 0;
    if (sscanf(g_line, "%x", &cfg_id) != 1) {
      fprintf(stderr, "split: bad cfg_id\n");
      return 1;
    }

    const char *config_hex = space + 1;
    const int config_len = unhex(config_hex, strlen(config_hex), g_config, sizeof g_config);
    if (config_len < 0) {
      fprintf(stderr, "split: bad config hex\n");
      return 1;
    }

    const uint8_t total = lh_frag_count((uint16_t)config_len);
    for (uint8_t index = 0; index < total; index++) {
      const int written = lh_frag_build((uint16_t)cfg_id, index, g_config, (uint16_t)config_len,
                                        g_fragment, sizeof g_fragment);
      if (written < 0) {
        fprintf(stderr, "split: builder refused fragment %u\n", (unsigned)index);
        return 1;
      }
      if (index > 0) printf(",");
      print_hex(g_fragment, (size_t)written);
    }
    printf("\n");
  }
  return 0;
}

static int run_reassemble(void) {
  while (fgets(g_line, (int)sizeof g_line, stdin) != NULL) {
    size_t len = strlen(g_line);
    while (len > 0 && (g_line[len - 1] == '\n' || g_line[len - 1] == '\r')) g_line[--len] = '\0';
    if (len == 0) continue;

    lh_frag_reset(&g_reasm);
    lh_frag_result_t result = LH_FRAG_NEED_MORE;

    char *cursor = g_line;
    while (cursor != NULL && *cursor != '\0') {
      char *comma = strchr(cursor, ',');
      if (comma != NULL) *comma = '\0';

      const int frag_len = unhex(cursor, strlen(cursor), g_fragment, sizeof g_fragment);
      if (frag_len < 0) {
        fprintf(stderr, "reassemble: bad fragment hex\n");
        return 1;
      }
      /* A fixed timestamp: these vectors test geometry, not timeouts. */
      result = lh_frag_feed(&g_reasm, g_fragment, (uint16_t)frag_len, 1000);

      cursor = (comma == NULL) ? NULL : comma + 1;
    }

    printf("%d ", (int)result);
    if (result == LH_FRAG_COMPLETE) print_hex(g_reasm.buf, g_reasm.total_len);
    printf("\n");
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: frag_cli split|reassemble\n");
    return 2;
  }
  if (strcmp(argv[1], "split") == 0) return run_split();
  if (strcmp(argv[1], "reassemble") == 0) return run_reassemble();

  fprintf(stderr, "usage: frag_cli split|reassemble\n");
  return 2;
}
