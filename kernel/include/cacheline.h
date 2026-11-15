/*
 * cacheline.h - The cache-line size, and per-CPU data that owns its line
 *
 * CACHE_LINE_SIZE is the one cache-line constant of the kernel (cache.h
 * takes it from here). 64 bytes is the L1 data line of every CPU SLM-OS
 * runs on. Cortex-A76 (Pi 5) and Cortex-A78AE (Jetson) both read
 * CTR_EL0 = 0x9444c004 on every core (shell: `coherency regs`):
 * DminLine = 4 (16 words, the smallest data line), CWG = 4 (cache
 * writeback granule) and ERG = 4 (exclusives reservation granule), all
 * 64 bytes; CCSIDR_EL1 gives 64-byte lines at L1D, L2 and L3. x86-64
 * uses 64-byte lines. The ERG is the unit that matters for locks: a store
 * to any byte of the granule clears another CPU's LDAXR reservation on it.
 *
 * Why every element, not just the array (#1263): an array of small
 * per-CPU words that is only aligned as a whole keeps all its elements in
 * one line. Every store by one CPU then takes the line away from the
 * others (false sharing), and where the element is a spinlock the
 * LDAXR/STXR pair of a waiting CPU can lose its reservation to stores to
 * the neighbouring locks for milliseconds, on a lock that is free
 * (measured on Jetson CPUs 4 and 5).
 *
 * Rule: a per-CPU array on the scheduler, steal, wake or tick path whose
 * elements are written by more than one CPU (each its own element
 * counts), or written by one CPU and polled by others, uses a padded
 * element: one of the cell types below (the value is the member `v`), a
 * struct marked CACHE_LINE_ALIGNED, or SPINLOCK_ARRAY (spinlock.h).
 * Assembly that indexes such an array shifts by CACHE_LINE_SHIFT.
 * kernel/tests/test_percpu_layout.c checks the stride and alignment of
 * every such array at run time; docs/scheduler.md lists them.
 */

#ifndef CACHELINE_H
#define CACHELINE_H

#define CACHE_LINE_SHIFT    6
#define CACHE_LINE_SIZE     64

#ifndef __ASSEMBLER__

#include <stdint.h>

_Static_assert(CACHE_LINE_SIZE == (1 << CACHE_LINE_SHIFT),
               "CACHE_LINE_SIZE and CACHE_LINE_SHIFT disagree");

#define CACHE_LINE_ALIGNED  __attribute__((aligned(CACHE_LINE_SIZE)))

/* Compile-time check that a type fills whole lines and starts on one. */
#define CACHE_LINE_ASSERT_PADDED(type)                                      \
    _Static_assert(sizeof(type) % CACHE_LINE_SIZE == 0 &&                   \
                   _Alignof(type) == CACHE_LINE_SIZE,                       \
                   #type " must be cache-line aligned and padded")

/*
 * A per-CPU cell: one value alone in its cache line. Declare the array as
 *     percpu_u32_t counter[MAX_CPUS];
 * and use counter[cpu].v. The struct wrapper makes a stale declaration
 * with the bare type a compile error wherever the real one is visible;
 * declare each array in one header only.
 */
#define PERCPU_CELL_TYPE(name, type)                                        \
    typedef struct { type v; } CACHE_LINE_ALIGNED name;                     \
    _Static_assert(sizeof(name) == CACHE_LINE_SIZE &&                       \
                   _Alignof(name) == CACHE_LINE_SIZE,                       \
                   #name " must be exactly one cache line")

PERCPU_CELL_TYPE(percpu_int_t, volatile int);
PERCPU_CELL_TYPE(percpu_u8_t,  volatile uint8_t);
PERCPU_CELL_TYPE(percpu_u32_t, volatile uint32_t);
PERCPU_CELL_TYPE(percpu_u64_t, volatile uint64_t);

#endif /* !__ASSEMBLER__ */

#endif /* CACHELINE_H */
