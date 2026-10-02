/*
 * versio_alloc — a bump allocator so the vendored engine needs no edits.
 *
 * belt_create() does five calloc()s (~130 KB: the input, dry and output
 * accumulator rings, the decimated YIN ring, belt_t) and belt_destroy() frees
 * them. Daisy has no meaningful heap, and rewriting belt_create() would fork
 * the engine.
 *
 * Instead the ARM build compiles smack_core.c with
 *     -Dcalloc=versio_calloc -Dfree=versio_free
 * pointing those two calls at an SDRAM pool. Allocation happens once at boot
 * and is never returned, so free() is a no-op -- correct here, because the
 * module creates exactly one engine and keeps it until power-off.
 *
 * The pool itself is declared in belt_alchemy.cpp (it needs DSY_SDRAM_BSS
 * from libDaisy); this file stays plain C so it can be unit-tested natively.
 */
#ifndef VERSIO_ALLOC_H
#define VERSIO_ALLOC_H

#include <stddef.h>

/*
 * Pool size, defined here rather than in belt_alchemy.cpp so the firmware and
 * test/test_tuner.c cannot disagree about it (smack-versio once let them
 * drift and the test kept passing on a pool the firmware no longer used).
 *
 * The engine wants ~130 KB; one megabyte is eight times that, and the
 * Seed2 DFM has 64 MiB of SDRAM, so there is no reason to be tighter. The
 * rings could live in AXI SRAM instead, which is a CPU experiment for later
 * if the seven voices turn out to need it.
 */
/* Belt's engine takes 154,376 B with HOLD's 16 KB freeze slice (it was
 * 137,552 B before; test/test_tuner.c prints it and keeps 16 KB spare); the
 * pool lives in D2 SRAM (belt_alchemy.lds .belt_pool, 0x30010000 up), so it
 * is sized to the engine with room, not to SDRAM. 176 KB ends at 0x3003C000,
 * inside RAM_D2 (which ends at 0x30048000). */
#define VERSIO_POOL_BYTES (176u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

void   versio_alloc_init(void *pool, size_t bytes);
void  *versio_calloc(size_t nmemb, size_t size);
void   versio_free(void *ptr);

/* For the boot-time log: how much of the pool the engine actually took. */
size_t versio_alloc_used(void);
size_t versio_alloc_capacity(void);

/* Set when an allocation did not fit. If this is true after smack_create(),
 * the module is broken and must say so rather than run half-initialised. */
int    versio_alloc_failed(void);

#ifdef __cplusplus
}
#endif

#endif /* VERSIO_ALLOC_H */
