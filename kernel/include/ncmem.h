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

#if !defined(PLATFORM_X86_64)
/*
 * Clean and invalidate a block by VA at boot (#1196).
 *
 * On Jetson, CPU 0 is not reset across kexec or the UEFI hand-off, so
 * its caches may still hold Write-Back lines for the NC block from the
 * previous owner. SLM-OS maps the block Normal Non-cacheable; a dirty
 * line left behind would later be evicted over SLM-OS data (Arm ARM
 * B2.11, "values reverted ... as a result of cache Write-Back").
 *
 * ncmem_clean_inval_range() runs `op` (DC CIVAC in production) on every
 * line of [base, base + size), one 4 KB page at a time, and compares
 * each page with a copy read just before. A line whose contents change
 * held a dirty line that was written back; it is counted. In the first
 * `preserve_bytes` the copy is written back over such a line, so that
 * prefix keeps the contents it had in DRAM (the Linux -> SLM-OS handoff
 * pages, which are only ever written through non-cacheable mappings or
 * by DMA). `base` and `size` must be 4 KB aligned and `preserve_bytes`
 * a multiple of the cache line. Boot-time and tests only: one static
 * page buffer, no locking. Returns 0, or -1 for bad arguments.
 */
typedef void (*ncmem_line_op_t)(uintptr_t line_addr);

typedef struct {
    uintptr_t base;             /* range maintained */
    size_t    size;
    size_t    preserve_bytes;   /* prefix whose DRAM contents are kept */
    uint32_t  line;             /* CTR_EL0.DminLine in bytes */
    uint32_t  lines;            /* lines `op` was applied to */
    uint32_t  changed;          /* lines whose contents changed */
    uint32_t  restored;         /* of those, lines put back (prefix) */
    uint64_t  preserve_hash_before;  /* FNV-1a over the prefix */
    uint64_t  preserve_hash_after;
    uint64_t  ticks;            /* CNTPCT ticks spent */
} ncmem_maint_stats_t;

int ncmem_clean_inval_range(uintptr_t base, size_t size,
                            size_t preserve_bytes, ncmem_line_op_t op,
                            ncmem_maint_stats_t *st);

/* DC CIVAC on one line (no barrier; the caller issues DSB SY). */
void ncmem_dc_civac_line(uintptr_t line_addr);

/* What ncmem_init's clean+invalidate of the NC block did (#1196). All
 * zero on platforms without the NC block. */
void ncmem_boot_maint_stats(ncmem_maint_stats_t *out);
#endif /* !PLATFORM_X86_64 */

#endif /* NCMEM_H */
