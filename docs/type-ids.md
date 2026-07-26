# Driver type ids

A `driver_type_id` is the only thing tying a manifest to a driver. The node
reports a number; the host looks it up and renders whatever the matching
manifest describes. Nothing at runtime notices when the two disagree — the
readings arrive intact, get attached to the wrong names, and every layer
involved believes it is working.

That is risk R3.4, and this file plus `tools/check-manifest-coherence.mjs` are
what keep it from happening.

## The two rules

**Ids are never changed.** Changing one silently repoints every stored
configuration and every rule that references the component. A node in the field
running older firmware will keep reporting the old number.

**Ids are never reused.** A retired id stays listed below as `RESERVED`. Reusing
one means a node that has not been reflashed reports a type the host now
believes is something else entirely — the exact failure the register exists to
prevent, arriving months later with no obvious cause.

Allocate the next free number. There is no shortage: the field is 16 bits.

## Register

| id | driver | manifest | since | status |
|---|---|---|---|---|
| 16 | `bme680` | `packages/components/manifests/bme680.json` | Etap 3 | active |
| 17 | `gpio_digital` | `packages/components/manifests/gpio_digital.json` | Etap 3 | active |

Ids 1–15 are unallocated and deliberately left so: starting at 16 keeps the
low numbers available if a future protocol revision needs to distinguish
built-in pseudo-components from real drivers.

## How the register is enforced

Three places, deliberately not sharing a source:

1. **`firmware/node/src/drivers/driver_registry.c`** — the image's list. This is
   what the node actually reports.
2. **`packages/components/manifests/*.json`** — what the host renders.
3. **`tools/check-manifest-coherence.mjs`** — compares them at build time, and
   fails the build on any disagreement in `driver_type_id`, channel count,
   `warmup_ms` or `min_interval_ms`.

The check reads the firmware's list by *compiling and running* it rather than by
parsing the source. Those differ exactly when it matters — a driver behind an
`#ifdef`, a vtable built through a macro, a translation unit somebody excluded —
and a check that re-reads the text a human already read is not much of a check.

There is also a plain assertion in `packages/components/test/registry.test.ts`
spelling out that `bme680` is 16 and `gpio_digital` is 17. It duplicates this
table on purpose: changing an id then has to be done deliberately in three
places, which for a value that must never change is the point.
