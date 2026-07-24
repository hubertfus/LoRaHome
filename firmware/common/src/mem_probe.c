/*
 * Memory/timing probe implementation. Roadmap §0.4.
 *
 * Two builds share this file:
 *
 *   ESP_PLATFORM  — real ESP-IDF probes (heap caps, FreeRTOS stack HWM, esp_timer)
 *   otherwise     — a host stub, so the formatting contract can be compiled and
 *                   tested off-target
 *
 * The stub exists for the format string, not to fake measurements: it reports
 * zeroes and a monotonic clock, and no metric gathered from a host build is
 * ever reported as an on-target number. Everything CI parses comes from the
 * line rendered by lh_mem_format_report, which is identical in both builds.
 */
#include "lorahome/mem_probe.h"

#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <time.h>
#endif

void lh_mem_snapshot(lh_mem_snapshot_t *out) {
  if (out == NULL) return;

#ifdef ESP_PLATFORM
  out->heap_free_internal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  out->heap_largest_block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  out->heap_min_free_ever = (uint32_t)esp_get_minimum_free_heap_size();
  /* uxTaskGetStackHighWaterMark returns words remaining, not bytes used. */
  out->stack_hwm = (uint32_t)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
  out->t_us = esp_timer_get_time();
#else
  out->heap_free_internal = 0u;
  out->heap_largest_block = 0u;
  out->heap_min_free_ever = 0u;
  out->stack_hwm = 0u;

  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    out->t_us = (int64_t)ts.tv_sec * 1000000 + (int64_t)ts.tv_nsec / 1000;
  } else {
    out->t_us = 0;
  }
#endif
}

float lh_mem_frag_ratio(const lh_mem_snapshot_t *snapshot) {
  if (snapshot == NULL || snapshot->heap_free_internal == 0u) return 0.0f;
  return (float)snapshot->heap_largest_block / (float)snapshot->heap_free_internal;
}

int lh_mem_format_report(
    char *out,
    size_t cap,
    const char *name,
    const lh_mem_snapshot_t *before,
    const lh_mem_snapshot_t *after) {
  if (out == NULL || name == NULL || before == NULL || after == NULL) return -1;

  /*
   * Signed: heap_free is unsigned, and computing after-minus-before in unsigned
   * arithmetic turns "freed 200 bytes" into 4294967096. Cast first, subtract
   * second. before-minus-after so that consumed memory reads positive.
   */
  const int64_t heap_delta = (int64_t)before->heap_free_internal - (int64_t)after->heap_free_internal;
  const int64_t dt_us = after->t_us - before->t_us;

  return snprintf(
      out,
      cap,
      "LH_METRIC %s heap_delta=%lld frag_ratio=%.4f stack_hwm=%lu dt_us=%lld",
      name,
      (long long)heap_delta,
      (double)lh_mem_frag_ratio(after),
      (unsigned long)after->stack_hwm,
      (long long)dt_us);
}

void lh_mem_report(const char *name, const lh_mem_snapshot_t *before, const lh_mem_snapshot_t *after) {
  char line[160];
  if (lh_mem_format_report(line, sizeof line, name, before, after) < 0) return;
  printf("%s\n", line);
}
