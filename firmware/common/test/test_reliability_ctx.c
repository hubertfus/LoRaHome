/*
 * Compile-only unit for the reliability RAM budget (T2.4).
 *
 * reliability.h carries a _Static_assert on sizeof(lh_reliability_ctx_t), and
 * an assertion in a header nobody compiles is a comment. This translation unit
 * exists so tools/compile-targets.mjs reads it on all three ABIs — the budget
 * has to hold on xtensa and riscv32, where alignment rules differ from the
 * host's, not merely on the machine that happened to run the tests.
 *
 * The instance is real rather than a sizeof() in a void: an alignment that
 * cannot be satisfied is a link-time problem, not a compile-time one.
 */
#include "lorahome/reliability.h"

static lh_reliability_ctx_t g_rel;

unsigned long lh_reliability_ctx_size(void);

unsigned long lh_reliability_ctx_size(void) {
  /* Touch the instance so it cannot be optimised out of existence before its
   * alignment has been decided. */
  g_rel.dedup.stat_accepted += 0;
  return (unsigned long)sizeof(lh_reliability_ctx_t);
}
