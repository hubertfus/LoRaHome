/*
 * Host-native ARQ harness for cross-language verification (T2.4).
 *
 * Reads a script, one event per line, and prints the resulting decision:
 *
 *   R <value>          push <value> onto the jitter queue
 *   S <seq> <len> <us> send a frame of <len> synthetic bytes
 *   A <seq> <us>       an ACK arrives
 *   T <us>             tick the timer
 *   G <0|1>            duty cycle guard: 0 refuses, 1 allows
 *
 * Output per line: "<result> <state> <retry_count>", where <result> is the
 * action code for T, 1/0 for S and A, and 0 for R and G.
 *
 * The jitter queue is what makes this comparable at all: both implementations
 * consume the same "random" numbers in the same order, so the retransmission
 * schedule is identical and any difference in the output is a difference in the
 * state machine rather than in two random number generators.
 */
#include <stdio.h>
#include <string.h>

#include "lorahome/arq.h"

#define RAND_QUEUE 4096

static uint32_t g_queue[RAND_QUEUE];
static int g_queue_len = 0;
static int g_queue_pos = 0;

/** Replays the scripted values, then repeats the last one. */
static uint32_t scripted_random(void *user) {
  (void)user;
  if (g_queue_len == 0) return 0;
  if (g_queue_pos >= g_queue_len) return g_queue[g_queue_len - 1];
  return g_queue[g_queue_pos++];
}

static bool g_allow_tx = true;

static bool guard(void *user, uint16_t len) {
  (void)user;
  (void)len;
  return g_allow_tx;
}

int main(void) {
  static char line[128];
  static uint8_t frame[LORAHOME_MAX_FRAME_SIZE];
  lh_arq_t arq;

  for (size_t i = 0; i < sizeof frame; i++) frame[i] = (uint8_t)(i * 7u + 3u);

  lh_arq_init(&arq, scripted_random, NULL);
  lh_arq_set_duty_cycle_guard(&arq, guard, NULL);

  while (fgets(line, (int)sizeof line, stdin) != NULL) {
    int result = 0;
    unsigned a = 0;
    unsigned b = 0;
    long long us = 0;

    switch (line[0]) {
      case 'R':
        if (sscanf(line + 1, "%u", &a) != 1) return 1;
        if (g_queue_len < RAND_QUEUE) g_queue[g_queue_len++] = a;
        break;
      case 'S':
        if (sscanf(line + 1, "%u %u %lld", &a, &b, &us) != 3) return 1;
        result = lh_arq_send(&arq, (uint8_t)a, frame, (uint16_t)b, (int64_t)us) ? 1 : 0;
        break;
      case 'A':
        if (sscanf(line + 1, "%u %lld", &a, &us) != 2) return 1;
        result = lh_arq_on_ack(&arq, (uint8_t)a, (int64_t)us) ? 1 : 0;
        break;
      case 'T':
        if (sscanf(line + 1, "%lld", &us) != 1) return 1;
        result = (int)lh_arq_tick(&arq, (int64_t)us, NULL, NULL);
        break;
      case 'G':
        if (sscanf(line + 1, "%u", &a) != 1) return 1;
        g_allow_tx = (a != 0);
        break;
      default:
        continue; /* blank line */
    }

    printf("%d %d %u\n", result, (int)arq.state, (unsigned)arq.retry_count);
  }

  return 0;
}
