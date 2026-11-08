/*
 * ncmem.h - Non-cacheable shared memory allocator
 *
 * Pi 5 and Jetson reserve one 2 MB block of DRAM that vmm.c maps with
 * MAIR index 2 (Normal Non-Cacheable, Inner Shareable) via an L2 table
 * entry. The physical address is platform-specific. Two separate uses
 * share it, with two macros:
 *
 *   PLATFORM_HAS_NC_REGION  The block exists, is mapped NC and reserved
 *                           from the PMM, and ncmem_alloc() hands out
 *                           pieces of it. Device DMA (xHCI rings, MACB,
 *                           CDC-ECM and USB bounce buffers, camera RCE)
 *                           and the fixed-address diagnostic slots use
 *                           it. Defined on Pi 5 and Jetson in every
 *                           build.
 *
 *   PLATFORM_HAS_NC_MEMORY  CPU-to-CPU shared state (run queues, task
 *                           table, current-task pointers, steal deques,
 *                           sched_diag_* counters, boot/init flags,
 *                           preemption state) lives in the block, and
 *                           the UART lock is IRQ-disable only. This is
 *                           the "NC axis" measured by #1193. Defined on
 *                           Pi 5 and Jetson unless the build passes
 *                           NC_AXIS=OFF (CMake defines NC_AXIS_OFF),
 *                           in which case every such user takes its
 *                           cacheable branch.
 *
 * The NC axis was introduced on the belief that the per-core L2 caches
 * are incoherent; the Cortex-A76 and Cortex-A78AE TRMs say they are
 * coherent in hardware and that the "SMPEN" bit SLM-OS checked does not
 * exist on these cores (docs/archive/coherency-gauntlet/c3-audit.md,
 * C0 facts Q1/Q4). NC_AXIS=OFF exists so that question can be measured.
 */

#ifndef NCMEM_H
#define NCMEM_H

#include <stdint.h>
#include <stddef.h>

#if defined(PLATFORM_RASPI5) || defined(PLATFORM_JETSON_ORIN_NANO)

/* The NC block and its allocator (see the header comment). */
#define PLATFORM_HAS_NC_REGION 1

/* CPU-to-CPU shared state in the NC block: use this macro in
 * scheduler/SMP code instead of platform-specific guards. */
#if !defined(NC_AXIS_OFF)
#define PLATFORM_HAS_NC_MEMORY 1
#endif

#if defined(PLATFORM_RASPI5)
#define NC_MEM_BASE     0xFFE00000UL    /* Last 2MB of 4GB RAM */
#elif defined(PLATFORM_JETSON_ORIN_NANO)
#define NC_MEM_BASE     0xBDE00000UL    /* Last 2MB of region 1 (before OP-TEE carveout) */
#endif
#define NC_MEM_SIZE     0x00200000UL    /* 2 MB */
#if defined(PLATFORM_JETSON_ORIN_NANO)
#define NC_MEM_RESERVED_BYTES 0x00005000UL
#else
#define NC_MEM_RESERVED_BYTES 0x00001000UL
#endif

/* Initialize NC memory allocator (call after vmm_init) */
void ncmem_init(void);

/* Allocate from NC region (bump allocator, cacheline-aligned) */
void *ncmem_alloc(size_t size, size_t align);

/* Return bytes allocated so far */
size_t ncmem_used(void);

#else /* No NC region (QEMU, x86-64) */

static inline void ncmem_init(void) {}
static inline void *ncmem_alloc(size_t size, size_t align)
{
    (void)size; (void)align;
    return NULL;
}
static inline size_t ncmem_used(void) { return 0; }

#endif

#endif /* NCMEM_H */
