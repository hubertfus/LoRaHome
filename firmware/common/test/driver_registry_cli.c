/*
 * Prints the image's driver registry, one line per entry. Roadmap T3.6.
 *
 *   name<TAB>type_id<TAB>channel_count<TAB>warmup_ms<TAB>min_interval_ms
 *
 * The point of running this rather than parsing driver_registry.c is that a
 * parser checks what the source appears to say, and this checks what the linked
 * image actually declares. Those differ exactly when it matters: a driver
 * behind an #ifdef, a vtable initialised through a macro, a build that excluded
 * a translation unit. Risk R3.4 is a manifest and a firmware disagreeing about
 * what a type id means, and a check that reads the same text a human already
 * read is not much of a check.
 */
#include <stdio.h>

#include "lorahome/driver.h"

int main(void) {
  for (uint8_t i = 0; i < LH_DRIVER_COUNT; i++) {
    const lh_driver_vtable_t *vt = LH_DRIVERS[i];
    if (vt == NULL || vt->name == NULL) {
      fprintf(stderr, "registry slot %u is empty\n", (unsigned)i);
      return 1;
    }
    printf("%s\t%u\t%u\t%u\t%u\n", vt->name, (unsigned)vt->type_id,
           (unsigned)vt->channel_count, (unsigned)vt->warmup_ms,
           (unsigned)vt->min_interval_ms);
  }
  return 0;
}
