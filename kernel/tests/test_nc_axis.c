/*
 * test_nc_axis.c - Where CPU-shared scheduler state lives (#1193)
 *
 * Run queues, the task table, the current-task pointers, the
 * sched_diag_* counters and the SECONDARY_PREEMPT state live in
 * cacheable Normal Write-Back Inner Shareable memory on every platform;
 * the "NC axis" that once put them in the 2 MB Normal Non-Cacheable
 * block on Pi 5 and Jetson is gone (docs/adr/0007-cross-cpu-coherency.md).
 * These tests walk the live translation tables and check the MAIR
 * attribute index, shareability and physical address of each structure,
 * so a change that moves one back into the NC block fails here. They
 * also check that the NC block itself stays mapped NC for device DMA on
 * Pi 5 and Jetson.
 */

#include "unity.h"
#include "test_harness.h"
#include "../include/ncmem.h"
#include "../include/sched.h"
#include "../include/task.h"
#include "../include/vmm.h"
#include "../include/spinlock.h"
#include "../include/config.h"
#include "../include/uart.h"
#if defined(SECONDARY_PREEMPT)
#include "../include/preempt.h"
#endif
#include <stdint.h>
#include <stdbool.h>

/* The NC axis and its Pi 5 spinlock DC CIVAC path stay removed. */
#if defined(PLATFORM_HAS_NC_MEMORY) || defined(NC_AXIS_OFF) || \
    defined(SPINLOCK_DRAM_FLUSH)
#error "the CPU-sharing NC axis was removed (#1193); keep shared state cacheable"
#endif
#if (defined(PLATFORM_RASPI5) || defined(PLATFORM_JETSON_ORIN_NANO)) && \
    !defined(PLATFORM_HAS_NC_REGION)
#error "Pi 5 and Jetson always have the NC region (device DMA)"
#endif

/* sched_diag.h declares the counters as arrays (no NC pointer form). */
_Static_assert(sizeof(sched_diag_tick) == MAX_CPUS * sizeof(uint32_t),
               "sched_diag_tick must be a MAX_CPUS array");

#define DESC_VALID      0x1UL
#define DESC_TABLE      0x2UL           /* bit 1 at L1/L2: table, at L3: page */
#define DESC_OA_MASK    0x0000FFFFFFFFF000UL
#define DESC_ATTR(d)    (((d) >> 2) & 0x7UL)
#define DESC_SH(d)      ((d) & (3UL << 8))

struct leaf {
    uint64_t desc;      /* 0 if unmapped */
    uint64_t pa;
};

/*
 * Walk the kernel tables (TTBR1, which shares its L1 with the identity
 * map in TTBR0) for `va` with the 4 KB granule, 39-bit VA layout from
 * vmm.h. Tables are identity-mapped, so their PAs are readable as VAs.
 */
static struct leaf walk(uint64_t va)
{
    struct leaf r = { 0, 0 };
    const uint64_t *table = (const uint64_t *)(uintptr_t)(vmm_get_ttbr1() & DESC_OA_MASK);
    static const unsigned shift[3] = { 30, 21, 12 };

    for (int level = 0; level < 3; level++) {
        uint64_t d = table[(va >> shift[level]) & 0x1FFUL];
        if (!(d & DESC_VALID)) {
            return r;
        }
        bool last = (level == 2) || !(d & DESC_TABLE);
        if (last) {
            uint64_t span = 1UL << shift[level];
            r.desc = d;
            r.pa = (d & DESC_OA_MASK & ~(span - 1)) | (va & (span - 1));
            return r;
        }
        table = (const uint64_t *)(uintptr_t)(d & DESC_OA_MASK);
    }
    return r;
}

static bool pa_in_nc_block(uint64_t pa)
{
#if defined(PLATFORM_HAS_NC_REGION)
    return pa >= NC_MEM_BASE && pa < NC_MEM_BASE + NC_MEM_SIZE;
#else
    (void)pa;
    return false;
#endif
}

/*
 * Assert cacheable WB placement (MAIR index 3), Inner Shareable, outside
 * the NC block.
 */
static void assert_cpu_shared_placement(const volatile void *p, const char *what)
{
    struct leaf l = walk((uint64_t)(uintptr_t)p);

    TEST_ASSERT_MESSAGE(l.desc != 0, what);
    TEST_ASSERT_MESSAGE(DESC_ATTR(l.desc) == MAIR_IDX_NORMAL_WB, what);
    TEST_ASSERT_MESSAGE(DESC_SH(l.desc) == PTE_SH_INNER, what);
    TEST_ASSERT_FALSE_MESSAGE(pa_in_nc_block(l.pa), what);
}

/* The walker itself: a known cacheable kernel symbol and the NC block. */
static void test_walker_finds_kernel_and_nc_block(void)
{
    static volatile uint32_t probe;
    struct leaf l = walk((uint64_t)(uintptr_t)&probe);

    TEST_ASSERT_NOT_EQUAL(0, l.desc);
    TEST_ASSERT_EQUAL_UINT64(MAIR_IDX_NORMAL_WB, DESC_ATTR(l.desc));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(uintptr_t)&probe, l.pa);   /* identity map */
    TEST_ASSERT_FALSE(pa_in_nc_block(l.pa));

#if defined(PLATFORM_HAS_NC_REGION)
    struct leaf nc = walk(NC_MEM_BASE);
    TEST_ASSERT_NOT_EQUAL(0, nc.desc);
    TEST_ASSERT_EQUAL_UINT64(MAIR_IDX_NORMAL_NC, DESC_ATTR(nc.desc));
    TEST_ASSERT_EQUAL_UINT64(NC_MEM_BASE, nc.pa);
#endif
}

static void test_run_queues_placement(void)
{
    for (uint32_t cpu = 0; cpu < MAX_CPUS; cpu++) {
        assert_cpu_shared_placement(sched_cpu_rq(cpu), "run queue");
    }
}

static void test_task_table_placement(void)
{
    /* The running test task's struct lives in the task table. */
    struct task *self = task_current();
    TEST_ASSERT_NOT_NULL(self);
    assert_cpu_shared_placement(self, "task table");
}

static void test_current_task_slots_placement(void)
{
#if defined(ENABLE_BOOT_TESTS)
    /* task_current_slot_addr exists only in test kernels, so the main
     * kernel's task.c is unchanged. */
    for (uint32_t cpu = 0; cpu < MAX_CPUS; cpu++) {
        assert_cpu_shared_placement(task_current_slot_addr(cpu), "current_task slot");
    }
    TEST_ASSERT_NULL(task_current_slot_addr(MAX_CPUS));
#else
    TEST_IGNORE_MESSAGE("test kernel only");
#endif
}

static void test_sched_diag_placement(void)
{
    assert_cpu_shared_placement(&sched_diag_tick[0], "sched_diag_tick");
}

static void test_preempt_state_placement(void)
{
#if defined(SECONDARY_PREEMPT)
    assert_cpu_shared_placement(&reschedule_pending[0], "reschedule_pending");
    assert_cpu_shared_placement(&orig_elr[0], "orig_elr");
#else
    TEST_IGNORE_MESSAGE("SECONDARY_PREEMPT not built");
#endif
}

/* Device DMA keeps the NC allocator on Pi 5 and Jetson. */
static void test_nc_region_serves_dma(void)
{
#if defined(PLATFORM_HAS_NC_REGION)
    void *p = ncmem_alloc(64, 64);
    TEST_ASSERT_NOT_NULL(p);
    struct leaf l = walk((uint64_t)(uintptr_t)p);
    TEST_ASSERT_EQUAL_UINT64(MAIR_IDX_NORMAL_NC, DESC_ATTR(l.desc));
    TEST_ASSERT_TRUE(pa_in_nc_block(l.pa));
#else
    TEST_ASSERT_NULL(ncmem_alloc(64, 64));
#endif
}

int test_suite_nc_axis(void)
{
    UnityBegin("test_nc_axis.c");
    uart_puts("  [nc_axis] CPU-shared state in cacheable memory\n");
    RUN_TEST(test_walker_finds_kernel_and_nc_block);
    RUN_TEST(test_run_queues_placement);
    RUN_TEST(test_task_table_placement);
    RUN_TEST(test_current_task_slots_placement);
    RUN_TEST(test_sched_diag_placement);
    RUN_TEST(test_preempt_state_placement);
    RUN_TEST(test_nc_region_serves_dma);
    return UnityEnd();
}
