#pragma once

#include "lorahome/arq.h"
#include "lorahome/dedup.h"
#include "lorahome/frag.h"

#ifdef __cplusplus
extern "C" {
/* The Unity suites are C++ translation units and this assertion lives in a
 * header, so it has to be spelled in a way both languages accept. `static_assert`
 * is C++'s; `_Static_assert` is C11's. Getting this wrong does not weaken the
 * check — it stops the file compiling, which is how it was found. */
#define LH_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define LH_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/**
 * The whole of Etap 2's state, in one place, with one budget.
 *
 * Three components that are used together on every node — duplicate
 * suppression on the way in, reassembly for anything that arrived in pieces,
 * and the ARQ for anything going out that must arrive. Grouping them is not
 * tidiness: it is what makes the RAM budget a single number a person can hold
 * in their head, and what makes adding a field to any of the three a build
 * failure rather than a quiet withdrawal from some other component's account.
 *
 * One static instance per device, in .bss. Never on the heap, never per-peer,
 * never resized — the reason a device that has been up for three weeks behaves
 * exactly like one that booted this morning.
 */
typedef struct {
  lh_dedup_t dedup;       /*  152 B */
  lh_reassembler_t reasm; /* 1640 B */
  lh_arq_t arq;           /*  320 B */
} lh_reliability_ctx_t;

/**
 * Roadmap Etap 2: the reliability layer fits in 2304 B.
 *
 * The individual figures moved during implementation — dedup came in under its
 * estimate, the ARQ over it — and this is the number that was ever really the
 * contract. Checked on all three ABIs by tools/compile-targets.mjs, because a
 * struct that fits on an x86-64 laptop and not on xtensa is a struct that fits
 * nowhere that matters.
 */
LH_STATIC_ASSERT(sizeof(lh_reliability_ctx_t) <= 2304,
                 "reliability RAM budget breach — see internal roadmap, Etap 2");

#ifdef __cplusplus
}
#endif
