/*
 * Host-native dedup harness for cross-language verification (T2.1).
 *
 *   dedup_cli   reads "<src_id> <seq> <now_us>" lines, writes "1" or "0"
 *               per line, then a final "STATS <accepted> <dupes> <too_old>
 *               <evicted>" line.
 *
 * One window for the whole stream — unlike slip_cli, where each line is an
 * independent frame. Dedup is nothing but accumulated state, so resetting
 * between lines would test only the first-frame path and would agree with any
 * implementation at all.
 *
 * Deliberately dumb, for the same reason crc16_cli.c is: the TypeScript side
 * owns the corpus and pipes it in, so the only thing under test is the window.
 */
#include <stdio.h>

#include "lorahome/dedup.h"

int main(void) {
  static char line[128];
  lh_dedup_t dedup;
  lh_dedup_init(&dedup);

  while (fgets(line, (int)sizeof line, stdin) != NULL) {
    unsigned src = 0;
    unsigned seq = 0;
    long long now_us = 0;

    if (sscanf(line, "%u %u %lld", &src, &seq, &now_us) != 3) {
      fprintf(stderr, "bad event line: %s", line);
      return 1;
    }

    const bool accepted =
        lh_dedup_check_and_mark(&dedup, (uint16_t)src, (uint8_t)seq, (int64_t)now_us);
    printf("%d\n", accepted ? 1 : 0);
  }

  printf("STATS %lu %lu %lu %lu\n", (unsigned long)dedup.stat_accepted,
         (unsigned long)dedup.stat_dupes_dropped, (unsigned long)dedup.stat_too_old,
         (unsigned long)dedup.stat_peer_evicted);
  return 0;
}
