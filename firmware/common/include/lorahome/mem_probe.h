/*
 * Canonical memory/timing probe. Roadmap §0.4.
 *
 * Every embedded test from this point on reports through this header, so that
 * "metrics in the commit message" means numbers a machine produced rather than
 * numbers a human retyped.
 *
 * heap_largest_block is not decoration. We can hold 180 kB free and still fail
 * to allocate a 4 kB contiguous buffer; that is the failure class that shows up
 * after three weeks in a field deployment, not on the bench.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t heap_free_internal; /* MALLOC_CAP_INTERNAL                */
  uint32_t heap_largest_block; /* fragmentation indicator            */
  uint32_t heap_min_free_ever; /* esp_get_minimum_free_heap_size()   */
  uint32_t stack_hwm;          /* current task high-water mark, bytes */
  int64_t t_us;                /* esp_timer_get_time()               */
} lh_mem_snapshot_t;

/** Fills *out with the current memory/timing state. Never allocates. */
void lh_mem_snapshot(lh_mem_snapshot_t *out);

/**
 * Prints the difference between two snapshots in the CI-parsable form:
 *
 *   LH_METRIC <name> heap_delta=<B> frag_ratio=<f> stack_hwm=<B> dt_us=<us>
 *
 * heap_delta is before-minus-after, so a positive number means memory was
 * consumed — the direction people expect when reading a leak report.
 */
void lh_mem_report(const char *name, const lh_mem_snapshot_t *before, const lh_mem_snapshot_t *after);

/**
 * The same line, rendered into a caller-supplied buffer instead of stdout.
 *
 * Split out from lh_mem_report so the formatting contract can be unit-tested on
 * any target without a serial port or a live heap: the report format is what CI
 * parses, so a silent change to it breaks the metrics pipeline. Returns the
 * number of characters written (excluding NUL), or a negative value on error,
 * following snprintf conventions. Does not allocate.
 */
int lh_mem_format_report(
    char *out,
    size_t cap,
    const char *name,
    const lh_mem_snapshot_t *before,
    const lh_mem_snapshot_t *after);

/**
 * Fragmentation ratio in [0,1]: largest_block / free_internal.
 *
 * 1.0 means the free heap is one contiguous run; as it falls, a large
 * allocation gets likelier to fail even while total free memory looks healthy.
 * Returns 0.0 when the heap is empty, so callers never divide by zero.
 */
float lh_mem_frag_ratio(const lh_mem_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
