/*
 * Compiles the vendored Belt engine with calloc/free redirected to the
 * SDRAM bump allocator (versio_alloc.c, carried over from the Smack ports;
 * the name is the file's provenance, not the platform).
 *
 * belt_create() does five calloc()s totalling ~130 KB (input, dry and
 * accumulator rings, the decimated YIN ring, and belt_t itself). That would
 * fit in AXI SRAM, but Daisy has no heap to speak of and the allocator is
 * already the family's answer; the pool is sized in versio_alloc.h. Doing
 * the redirect here rather than as a global -D keeps it scoped to this
 * translation unit -- libDaisy, the Alchemy SDK and everything else keep the
 * real calloc/free. stdlib.h is included FIRST so the macros rewrite call
 * sites only, never the library's own declarations.
 *
 * Build this file; do NOT add vendor/belt_core.c to the source list too, or
 * you get duplicate symbols.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "versio_alloc.h"

#define calloc versio_calloc
#define free   versio_free

#include "vendor/belt_core.c"
