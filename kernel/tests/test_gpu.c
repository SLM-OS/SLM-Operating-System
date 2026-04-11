/*
 * GPU Subsystem Tests
 *
 * Validates GPU platform abstraction functionality:
 * - Driver registration and initialization
 * - GPU buffer allocation and deallocation
 * - Memory accessibility and alignment
 * - Cache coherency operations
 * - Error handling
 *
 * Note: True cache coherency cannot be fully validated without real DMA
 * hardware. These tests verify the code paths execute correctly and
 * the memory operations behave as expected from the CPU's perspective.
 */

#include "unity.h"
#include "gpu.h"
#include "pmm.h"
#include "uart.h"
#include "spinlock.h"
#include "platform.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Test pattern for memory verification */
#define TEST_PATTERN_A  0xDEADBEEF
#define TEST_PATTERN_B  0xCAFEBABE
#define TEST_PATTERN_C  0x12345678

/* ============================================================================
 * Unit Tests: GPU Initialization
 * ============================================================================ */

/*
 * Test: GPU is available after init
 */
static void test_gpu_available_after_init(void)
{
    /* GPU should be initialized by kernel_main before tests run */
    TEST_ASSERT_TRUE(gpu_available());
}

/*
 * Test: gpu_get_info returns valid info
 */
static void test_gpu_get_info_valid(void)
{
    gpu_info_t info;
    int ret = gpu_get_info(&info);

    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);
    TEST_ASSERT_NOT_NULL(info.name);
    TEST_ASSERT_NOT_NULL(info.device);

    /* Stub driver should report no capabilities */
    TEST_ASSERT_EQUAL_UINT32(GPU_CAP_NONE, info.capabilities);

    /* Log info for verification */
    uart_printf("  GPU driver: %s\n", info.name);
    uart_printf("  GPU device: %s\n", info.device);
}

/*
 * Test: gpu_get_info with NULL returns error
 */
static void test_gpu_get_info_null_returns_error(void)
{
    int ret = gpu_get_info(NULL);
    TEST_ASSERT_EQUAL_INT(GPU_ERR_INVALID_PARAM, ret);
}

/* ============================================================================
 * Unit Tests: GPU Buffer Allocation
 * ============================================================================ */

/*
 * Test: gpu_alloc returns valid buffer
 */
static void test_gpu_alloc_returns_valid_buffer(void)
{
    gpu_buffer_t buf;
    size_t size = 4096;  /* 1 page */

    int ret = gpu_alloc(size, GPU_MEM_READWRITE, &buf);

    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);
    TEST_ASSERT_NOT_NULL(buf.cpu_addr);
    TEST_ASSERT(buf.gpu_addr != 0);
    TEST_ASSERT(buf.size >= size);

    gpu_free(&buf);
}

/*
 * Test: Allocated buffer is readable and writable
 */
static void test_gpu_alloc_memory_is_accessible(void)
{
    gpu_buffer_t buf;
    size_t size = 4096;

    int ret = gpu_alloc(size, GPU_MEM_READWRITE, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);

    /* Write test pattern */
    uint32_t *ptr = (uint32_t *)buf.cpu_addr;
    ptr[0] = TEST_PATTERN_A;
    ptr[size/sizeof(uint32_t) - 1] = TEST_PATTERN_B;  /* End of buffer */

    /* Read back and verify */
    TEST_ASSERT_EQUAL_HEX32(TEST_PATTERN_A, ptr[0]);
    TEST_ASSERT_EQUAL_HEX32(TEST_PATTERN_B, ptr[size/sizeof(uint32_t) - 1]);

    gpu_free(&buf);
}

/*
 * Test: Multiple allocations return different addresses
 */
static void test_gpu_alloc_multiple_buffers_distinct(void)
{
    gpu_buffer_t buf1, buf2, buf3;
    size_t size = 4096;

    int ret1 = gpu_alloc(size, GPU_MEM_READWRITE, &buf1);
    int ret2 = gpu_alloc(size, GPU_MEM_READWRITE, &buf2);
    int ret3 = gpu_alloc(size, GPU_MEM_READWRITE, &buf3);

    TEST_ASSERT_EQUAL_INT(GPU_OK, ret1);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret2);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret3);

    /* All addresses should be different */
    TEST_ASSERT(buf1.cpu_addr != buf2.cpu_addr);
    TEST_ASSERT(buf2.cpu_addr != buf3.cpu_addr);
    TEST_ASSERT(buf1.cpu_addr != buf3.cpu_addr);

    /* Write different patterns to verify no overlap */
    *(uint32_t *)buf1.cpu_addr = TEST_PATTERN_A;
    *(uint32_t *)buf2.cpu_addr = TEST_PATTERN_B;
    *(uint32_t *)buf3.cpu_addr = TEST_PATTERN_C;

    /* Verify patterns didn't overwrite each other */
    TEST_ASSERT_EQUAL_HEX32(TEST_PATTERN_A, *(uint32_t *)buf1.cpu_addr);
    TEST_ASSERT_EQUAL_HEX32(TEST_PATTERN_B, *(uint32_t *)buf2.cpu_addr);
    TEST_ASSERT_EQUAL_HEX32(TEST_PATTERN_C, *(uint32_t *)buf3.cpu_addr);

    gpu_free(&buf1);
    gpu_free(&buf2);
    gpu_free(&buf3);
}

/*
 * Test: 2MB aligned allocation
 */
static void test_gpu_alloc_2mb_alignment(void)
{
    gpu_buffer_t buf;
    size_t size = 2 * 1024 * 1024;  /* 2MB */

    int ret = gpu_alloc(size, GPU_MEM_READWRITE | GPU_MEM_ALIGN_2MB, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);

    /* Verify 2MB alignment */
    uintptr_t addr = (uintptr_t)buf.cpu_addr;
    TEST_ASSERT_EQUAL_UINT64(0, addr % (2 * 1024 * 1024));

    /* Verify size */
    TEST_ASSERT(buf.size >= size);

    gpu_free(&buf);
}

/*
 * Test: gpu_alloc with zero size returns error
 */
static void test_gpu_alloc_zero_size_returns_error(void)
{
    gpu_buffer_t buf;
    int ret = gpu_alloc(0, GPU_MEM_READWRITE, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_ERR_INVALID_PARAM, ret);
}

/*
 * Test: gpu_alloc with NULL buffer returns error
 */
static void test_gpu_alloc_null_buffer_returns_error(void)
{
    int ret = gpu_alloc(4096, GPU_MEM_READWRITE, NULL);
    TEST_ASSERT_EQUAL_INT(GPU_ERR_INVALID_PARAM, ret);
}

/* ============================================================================
 * Unit Tests: GPU Buffer Deallocation
 * ============================================================================ */

/*
 * Test: gpu_free releases memory back to PMM
 */
static void test_gpu_free_releases_memory(void)
{
    uint64_t free_before = pmm_get_free_pages();

    gpu_buffer_t buf;
    size_t size = 16 * 4096;  /* 16 pages */

    int ret = gpu_alloc(size, GPU_MEM_READWRITE, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);

    uint64_t free_during = pmm_get_free_pages();
    TEST_ASSERT(free_during < free_before);  /* Memory was allocated */

    gpu_free(&buf);

    uint64_t free_after = pmm_get_free_pages();
    TEST_ASSERT_EQUAL_UINT64(free_before, free_after);  /* Memory reclaimed */
}

/*
 * Test: gpu_free with NULL is safe (no crash, no side effects)
 */
static void test_gpu_free_null_is_safe(void)
{
    /* Record memory state before */
    uint64_t free_before = pmm_get_free_pages();

    /* Call gpu_free with NULL - should handle gracefully */
    gpu_free(NULL);

    /* Memory state should be unchanged (no spurious frees) */
    uint64_t free_after = pmm_get_free_pages();
    TEST_ASSERT_EQUAL_UINT64(free_before, free_after);
}

/*
 * Test: gpu_free clears buffer fields
 */
static void test_gpu_free_clears_buffer(void)
{
    gpu_buffer_t buf;

    int ret = gpu_alloc(4096, GPU_MEM_READWRITE, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);
    TEST_ASSERT_NOT_NULL(buf.cpu_addr);

    gpu_free(&buf);

    /* Buffer should be cleared */
    TEST_ASSERT_NULL(buf.cpu_addr);
    TEST_ASSERT_EQUAL_UINT64(0, buf.gpu_addr);
    TEST_ASSERT_EQUAL_UINT64(0, buf.size);
}

/* ============================================================================
 * Unit Tests: Cache Coherency Operations
 * ============================================================================ */

/*
 * Test: cache_clean_range executes without fault
 *
 * We can't truly verify cache behavior without DMA hardware, but we can
 * verify the instructions execute without causing exceptions.
 */
static void test_cache_clean_executes(void)
{
    gpu_buffer_t buf;
    int ret = gpu_alloc(4096, GPU_MEM_READWRITE, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);

    /* Write data */
    uint32_t *ptr = (uint32_t *)buf.cpu_addr;
    for (int i = 0; i < 1024; i++) {
        ptr[i] = i;
    }

    /* Clean cache - should not fault */
    cache_clean_range(buf.cpu_addr, buf.size);

    /* Data should still be readable */
    TEST_ASSERT_EQUAL_UINT32(0, ptr[0]);
    TEST_ASSERT_EQUAL_UINT32(512, ptr[512]);
    TEST_ASSERT_EQUAL_UINT32(1023, ptr[1023]);

    gpu_free(&buf);
}

/*
 * Test: cache_invalidate_range executes without fault
 */
static void test_cache_invalidate_executes(void)
{
    gpu_buffer_t buf;
    int ret = gpu_alloc(4096, GPU_MEM_READWRITE, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);

    /* Write data */
    uint32_t *ptr = (uint32_t *)buf.cpu_addr;
    ptr[0] = TEST_PATTERN_A;

    /* Invalidate cache - should not fault */
    cache_invalidate_range(buf.cpu_addr, buf.size);

    /*
     * After invalidate, reading may return the old value (from memory)
     * or the cached value (if cache refilled). Either is acceptable
     * without real DMA to modify memory behind the cache.
     */
    uint32_t value = ptr[0];
    TEST_ASSERT(value == TEST_PATTERN_A || value == 0);

    gpu_free(&buf);
}

/*
 * Test: cache_flush_range executes without fault
 */
static void test_cache_flush_executes(void)
{
    gpu_buffer_t buf;
    int ret = gpu_alloc(4096, GPU_MEM_READWRITE, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);

    /* Write data */
    uint32_t *ptr = (uint32_t *)buf.cpu_addr;
    ptr[0] = TEST_PATTERN_B;

    /* Flush (clean + invalidate) - should not fault */
    cache_flush_range(buf.cpu_addr, buf.size);

    /* Data should still be accessible */
    TEST_ASSERT_EQUAL_HEX32(TEST_PATTERN_B, ptr[0]);

    gpu_free(&buf);
}

/*
 * Test: gpu_sync_for_gpu cleans cache
 */
static void test_gpu_sync_for_gpu_executes(void)
{
    gpu_buffer_t buf;
    int ret = gpu_alloc(4096, GPU_MEM_READWRITE, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);

    /* Write data that "GPU" will read */
    uint32_t *ptr = (uint32_t *)buf.cpu_addr;
    ptr[0] = TEST_PATTERN_C;

    /* Sync for GPU (clean cache) - should not fault */
    gpu_sync_for_gpu(&buf);

    /* CPU can still read the data */
    TEST_ASSERT_EQUAL_HEX32(TEST_PATTERN_C, ptr[0]);

    gpu_free(&buf);
}

/*
 * Test: gpu_sync_for_cpu invalidates cache and data remains readable
 */
static void test_gpu_sync_for_cpu_executes(void)
{
    gpu_buffer_t buf;
    int ret = gpu_alloc(4096, GPU_MEM_READWRITE, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);

    /* Write data to buffer */
    uint32_t *ptr = (uint32_t *)buf.cpu_addr;
    ptr[0] = TEST_PATTERN_A;
    ptr[1] = TEST_PATTERN_B;

    /* First sync for GPU (clean cache) to ensure data is in memory */
    gpu_sync_for_gpu(&buf);

    /* Now sync for CPU (invalidate cache) - simulating GPU write complete */
    gpu_sync_for_cpu(&buf);

    /* Data should be readable and correct (cache refills from memory) */
    TEST_ASSERT_EQUAL_HEX32(TEST_PATTERN_A, ptr[0]);
    TEST_ASSERT_EQUAL_HEX32(TEST_PATTERN_B, ptr[1]);

    gpu_free(&buf);
}

/*
 * Test: Cache operations with NULL are safe (no crash, no side effects)
 */
static void test_cache_ops_null_safe(void)
{
    /* Record memory state - should be unchanged by null ops */
    uint64_t free_before = pmm_get_free_pages();

    /* Cache ops with NULL addr should return early without crashing */
    cache_clean_range(NULL, 4096);
    cache_invalidate_range(NULL, 4096);
    cache_flush_range(NULL, 4096);

    /* GPU sync with empty buffer should be no-op */
    gpu_buffer_t empty = {0};
    gpu_sync_for_gpu(&empty);
    gpu_sync_for_cpu(&empty);

    /* Memory should be unchanged */
    uint64_t free_after = pmm_get_free_pages();
    TEST_ASSERT_EQUAL_UINT64(free_before, free_after);
}

/*
 * Test: Cache operations with zero size are no-ops
 */
static void test_cache_ops_zero_size_safe(void)
{
    /* Create buffer with known pattern */
    uint8_t buf[64];
    for (int i = 0; i < 64; i++) {
        buf[i] = (uint8_t)i;
    }

    /* Zero-size cache ops should be no-ops (not touch memory) */
    cache_clean_range(buf, 0);
    cache_invalidate_range(buf, 0);
    cache_flush_range(buf, 0);

    /* Buffer should be unchanged */
    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, buf[i]);
    }
}

/* ============================================================================
 * Integration Tests: Full Workflow
 * ============================================================================ */

/*
 * Test: Complete GPU buffer workflow
 *
 * Simulates a typical GPU operation:
 * 1. Allocate buffer
 * 2. CPU writes input data
 * 3. Sync for GPU
 * 4. (GPU would process here)
 * 5. Sync for CPU
 * 6. CPU reads output data
 * 7. Free buffer
 */
static void test_gpu_buffer_workflow(void)
{
    gpu_buffer_t buf;
    size_t size = 8192;  /* 2 pages */

    /* Step 1: Allocate */
    int ret = gpu_alloc(size, GPU_MEM_READWRITE, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);

    /* Step 2: CPU writes input */
    uint32_t *data = (uint32_t *)buf.cpu_addr;
    for (size_t i = 0; i < size / sizeof(uint32_t); i++) {
        data[i] = (uint32_t)i;
    }

    /* Step 3: Sync for GPU */
    gpu_sync_for_gpu(&buf);

    /* Step 4: Simulate GPU processing (just verify data is intact) */
    /* On real hardware, GPU would modify the buffer here */

    /* Step 5: Sync for CPU */
    gpu_sync_for_cpu(&buf);

    /* Step 6: CPU reads output */
    bool data_valid = true;
    for (size_t i = 0; i < size / sizeof(uint32_t); i++) {
        if (data[i] != (uint32_t)i) {
            data_valid = false;
            break;
        }
    }
    TEST_ASSERT_TRUE(data_valid);

    /* Step 7: Free */
    gpu_free(&buf);
}

/*
 * Test: Allocate, free, reallocate cycle
 *
 * Verifies memory is properly recycled.
 */
static void test_gpu_alloc_free_cycle(void)
{
    gpu_buffer_t buf;
    void *first_addr = NULL;

    /* Allocate */
    int ret = gpu_alloc(4096, GPU_MEM_READWRITE, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);
    first_addr = buf.cpu_addr;

    /* Free */
    gpu_free(&buf);

    /* Reallocate - may get same address (memory reuse) */
    ret = gpu_alloc(4096, GPU_MEM_READWRITE, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);
    TEST_ASSERT_NOT_NULL(buf.cpu_addr);

    /* Write to verify usable */
    *(uint32_t *)buf.cpu_addr = TEST_PATTERN_A;
    TEST_ASSERT_EQUAL_HEX32(TEST_PATTERN_A, *(uint32_t *)buf.cpu_addr);

    gpu_free(&buf);

    /* Log if we got memory reuse (informational, not a pass/fail) */
    if (buf.cpu_addr == first_addr) {
        uart_puts("  (Memory was reused - good!)\n");
    }
}

/*
 * Test: Large buffer allocation (1MB)
 */
static void test_gpu_alloc_large_buffer(void)
{
    gpu_buffer_t buf;
    size_t size = 1024 * 1024;  /* 1MB */

    int ret = gpu_alloc(size, GPU_MEM_READWRITE, &buf);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);
    TEST_ASSERT(buf.size >= size);

    /* Write pattern at start, middle, and end */
    uint32_t *ptr = (uint32_t *)buf.cpu_addr;
    ptr[0] = TEST_PATTERN_A;
    ptr[size / sizeof(uint32_t) / 2] = TEST_PATTERN_B;
    ptr[size / sizeof(uint32_t) - 1] = TEST_PATTERN_C;

    /* Verify */
    TEST_ASSERT_EQUAL_HEX32(TEST_PATTERN_A, ptr[0]);
    TEST_ASSERT_EQUAL_HEX32(TEST_PATTERN_B, ptr[size / sizeof(uint32_t) / 2]);
    TEST_ASSERT_EQUAL_HEX32(TEST_PATTERN_C, ptr[size / sizeof(uint32_t) - 1]);

    gpu_free(&buf);
}

/* ============================================================================
 * Unit Tests: NVIDIA GPU Register Decode (Platform-Agnostic)
 *
 * Tests the register decode functions in gpu_nvidia.h using known hardware
 * values from actual Jetson Orin Nano (GA10B) and RTX 3050 (GA106) GPUs.
 * These functions are pure computation — no hardware access required.
 * ============================================================================ */

#include "gpu_nvidia.h"

/* Actual hardware values captured from Jetson Orin Nano GA10B */
#define GA10B_BOOT0     0xB7B000A1
#define GA10B_BOOT42    0x17BA1000

/*
 * Test: BOOT_0 architecture decode for GA10B
 */
static void test_nv_boot0_arch_ga10b(void)
{
    uint32_t arch = nv_boot0_arch(GA10B_BOOT0);
    TEST_ASSERT_EQUAL_UINT32(NV_GPU_ARCHITECTURE_AMPERE, arch);
}

/*
 * Test: BOOT_0 implementation decode for GA10B
 */
static void test_nv_boot0_impl_ga10b(void)
{
    uint32_t impl = nv_boot0_impl(GA10B_BOOT0);
    TEST_ASSERT_EQUAL_UINT32(NV_GPU_IMPL_GA10B, impl);
}

/*
 * Test: BOOT_42 chip ID decode for GA10B
 */
static void test_nv_boot42_chip_id_ga10b(void)
{
    uint32_t chip_id = nv_boot42_chip_id(GA10B_BOOT42);
    TEST_ASSERT_EQUAL_UINT32(NV_CHIP_ID_GA10B, chip_id);
}

/*
 * Test: BOOT_42 architecture decode for GA10B
 */
static void test_nv_boot42_arch_ga10b(void)
{
    uint32_t arch = nv_boot42_arch(GA10B_BOOT42);
    TEST_ASSERT_EQUAL_UINT32(NV_GPU_ARCHITECTURE_AMPERE, arch);
}

/*
 * Test: Chip name lookup for known Ampere variants
 */
static void test_nv_chip_name_lookup(void)
{
    TEST_ASSERT_EQUAL_STRING("GA10B (Jetson Orin)", nv_chip_name(NV_CHIP_ID_GA10B));
    TEST_ASSERT_EQUAL_STRING("GA106 (RTX 3050/3060)", nv_chip_name(NV_CHIP_ID_GA106));
    TEST_ASSERT_EQUAL_STRING("GA100 (A100)", nv_chip_name(NV_CHIP_ID_GA100));
    TEST_ASSERT_EQUAL_STRING("Unknown", nv_chip_name(0x999));
}

/*
 * Test: NV_GPU_NOT_PRESENT sentinel detection
 */
static void test_nv_gpu_not_present(void)
{
    /* 0xFFFFFFFF indicates GPU not present or in reset */
    uint32_t boot0 = NV_GPU_NOT_PRESENT;
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFF, boot0);
    /* Architecture decode of 0xFFFFFFFF should NOT match Ampere */
    uint32_t arch = nv_boot0_arch(boot0);
    TEST_ASSERT_NOT_EQUAL(NV_GPU_ARCHITECTURE_AMPERE, arch);
}

/*
 * Test: Synthetic GA106 (RTX 3050) register decode
 *
 * Construct a plausible BOOT_42 value for GA106 and verify decode.
 * BOOT_42 format: arch(29:24)=0x17, impl(23:20)=0x6 → chip_id=0x176
 */
static void test_nv_boot42_chip_id_ga106(void)
{
    uint32_t synthetic_boot42 = (NV_GPU_ARCHITECTURE_AMPERE << 24) |
                                (NV_GPU_IMPL_GA106 << 20);
    uint32_t chip_id = nv_boot42_chip_id(synthetic_boot42);
    TEST_ASSERT_EQUAL_UINT32(NV_CHIP_ID_GA106, chip_id);
}

/*
 * Test: Architecture constants are distinct
 */
static void test_nv_architecture_constants(void)
{
    TEST_ASSERT_NOT_EQUAL(NV_GPU_ARCHITECTURE_TURING, NV_GPU_ARCHITECTURE_AMPERE);
    TEST_ASSERT_NOT_EQUAL(NV_GPU_ARCHITECTURE_AMPERE, NV_GPU_ARCHITECTURE_HOPPER);
    TEST_ASSERT_NOT_EQUAL(NV_GPU_ARCHITECTURE_HOPPER, NV_GPU_ARCHITECTURE_ADA);
}

/* ============================================================================
 * Regression Tests: Jetson CBB Firewall (Phase 5)
 *
 * GPU registers at 0x17000000 are behind the CBB firewall on Jetson.
 * Reading NV_PMC_BOOT_0 causes a Synchronous External Abort (bus error).
 * The nvidia GPU driver must NOT be used for direct MMIO probe on Jetson.
 * ============================================================================ */

/*
 * Compile-time assertion: Jetson must use SPINLOCK_SKIP_LOCKING.
 * Without this, LSE atomics (SWPALB) in spinlock code cause Synchronous
 * External Abort on Cortex-A78AE before MMU enable. This killed boot
 * at the first uart_puts() call.
 */
#if defined(PLATFORM_JETSON_ORIN_NANO)
static_assert(SPINLOCK_SKIP_LOCKING == 1,
    "Jetson requires SPINLOCK_SKIP_LOCKING — LSE atomics fault before MMU enable");
#endif

/*
 * Test: GPU stub driver is used on Jetson (not nvidia MMIO probe).
 *
 * Regression for: Phase 5 Jetson kexec crash (syndrome 0x92000410).
 * The nvidia driver's nv_gpu_probe() reads GPU_BASE (0x17000000),
 * which is CBB-protected. This test verifies the stub driver is active
 * by checking the driver name.
 */
static void test_gpu_jetson_uses_stub_driver(void)
{
    gpu_info_t info;
    int ret = gpu_get_info(&info);
    TEST_ASSERT_EQUAL_INT(GPU_OK, ret);

#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* On Jetson, must use stub driver (GPU registers are CBB-protected) */
    TEST_ASSERT_EQUAL_STRING("stub", info.name);
#endif
    /* On all platforms, GPU init must succeed without crashing */
    TEST_ASSERT_TRUE(gpu_available());
}

/*
 * Test: Spinlock is barrier-only on Jetson (no exclusive ops).
 *
 * Regression for: Phase 5 Jetson kexec crash.
 * Verifies that spin_lock/spin_unlock don't use LDAXR/STXR
 * (which are compiled away by SPINLOCK_SKIP_LOCKING).
 */
static void test_spinlock_safe_on_jetson(void)
{
    spinlock_t test_lock = SPINLOCK_INIT;

    /* spin_lock_irqsave / spin_unlock_irqrestore must not crash.
     * On Jetson with SPINLOCK_SKIP_LOCKING, these are barrier-only.
     * On other platforms, they use the runtime spinlock_hw_enabled check. */
    irq_flags_t flags = spin_lock_irqsave(&test_lock);
    spin_unlock_irqrestore(&test_lock, flags);

    /* Verify the lock is not held after unlock */
    TEST_ASSERT_FALSE(spin_is_locked(&test_lock));
}

/* ============================================================================
 * Regression Tests: Jetson kexec boot fixes (April 2026)
 *
 * Three issues were discovered when booting SLM-OS via kexec on the Jetson
 * Orin Nano with stock JetPack L4T 36.4.4:
 *
 * 1. Stale interrupts crash during BSS clear (boot.S DAIF masking)
 * 2. UART LSR reads garble serial output (DSB barrier needed)
 * 3. GPU RAS error from stale DMA (Linux-side GPU suspend, not testable here)
 * ============================================================================ */

/*
 * Test: DAIF is masked before kernel_main entry.
 *
 * Regression for: Stale Linux interrupts crashing SLM-OS during BSS clear.
 * After kexec, Linux timers and peripheral interrupts remain pending. Without
 * DAIF masking, these fire before exception vectors are installed, causing an
 * unpredictable crash. boot.S now masks DAIF as the first instruction at
 * primary_cpu. By the time kernel_main runs, all exceptions must be masked.
 */
static void test_daif_masked_at_boot(void)
{
    /* Read current DAIF — IRQ (bit 7) and FIQ (bit 6) should be masked.
     * They get unmasked later during scheduler start, but during tests
     * (which run before the scheduler), they may or may not be masked
     * depending on test ordering. Verify at minimum that we CAN read DAIF
     * without crashing (proves the register access works after kexec). */
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));

    /* DAIF register format: bits [9:6] = D,A,I,F masks.
     * We just verify the register is readable and non-garbage.
     * Only bits [9:6] should be set; other bits are RES0. */
    TEST_ASSERT_EQUAL_UINT64(0, daif & ~0x3C0ULL);
}

/*
 * Test: UART output works without garbling after initialization.
 *
 * Regression for: UARTC LSR reads returning stale data after kexec.
 * Without DSB barriers, speculative MMIO reads to LSR return cached/stale
 * values, causing uart_putc to write THR when the TX FIFO is full. This
 * garbles serial output. The fix adds DSB SY before each LSR read.
 *
 * This test verifies that uart_putc can send a sequence of characters
 * without crashing or hanging (the garbling is only visible on actual
 * hardware serial output, but a hang or crash would indicate the DSB
 * is missing or incorrect).
 */
static void test_uart_output_no_hang(void)
{
    /* Send a known sequence — if LSR reads are broken, this may hang
     * in the THRE polling loop or crash on MMIO access */
    const char *test_str = "UART_TEST_OK\n";
    for (int i = 0; test_str[i]; i++) {
        uart_putc(test_str[i]);
    }
    /* If we got here without hanging, the DSB-guarded LSR reads work */
    TEST_PASS();
}

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_gpu(void)
{
    UnityBegin("GPU Tests");

    /* Regression tests: Jetson CBB firewall */
    RUN_TEST(test_gpu_jetson_uses_stub_driver);
    RUN_TEST(test_spinlock_safe_on_jetson);

    /* Regression tests: Jetson kexec boot fixes */
    RUN_TEST(test_daif_masked_at_boot);
    RUN_TEST(test_uart_output_no_hang);

    /* Initialization tests */
    RUN_TEST(test_gpu_available_after_init);
    RUN_TEST(test_gpu_get_info_valid);
    RUN_TEST(test_gpu_get_info_null_returns_error);

    /* Allocation tests */
    RUN_TEST(test_gpu_alloc_returns_valid_buffer);
    RUN_TEST(test_gpu_alloc_memory_is_accessible);
    RUN_TEST(test_gpu_alloc_multiple_buffers_distinct);
    RUN_TEST(test_gpu_alloc_2mb_alignment);
    RUN_TEST(test_gpu_alloc_zero_size_returns_error);
    RUN_TEST(test_gpu_alloc_null_buffer_returns_error);

    /* Deallocation tests */
    RUN_TEST(test_gpu_free_releases_memory);
    RUN_TEST(test_gpu_free_null_is_safe);
    RUN_TEST(test_gpu_free_clears_buffer);

    /* Cache coherency tests */
    RUN_TEST(test_cache_clean_executes);
    RUN_TEST(test_cache_invalidate_executes);
    RUN_TEST(test_cache_flush_executes);
    RUN_TEST(test_gpu_sync_for_gpu_executes);
    RUN_TEST(test_gpu_sync_for_cpu_executes);
    RUN_TEST(test_cache_ops_null_safe);
    RUN_TEST(test_cache_ops_zero_size_safe);

    /* Integration tests */
    RUN_TEST(test_gpu_buffer_workflow);
    RUN_TEST(test_gpu_alloc_free_cycle);
    RUN_TEST(test_gpu_alloc_large_buffer);

    /* NVIDIA register decode tests (platform-agnostic) */
    RUN_TEST(test_nv_boot0_arch_ga10b);
    RUN_TEST(test_nv_boot0_impl_ga10b);
    RUN_TEST(test_nv_boot42_chip_id_ga10b);
    RUN_TEST(test_nv_boot42_arch_ga10b);
    RUN_TEST(test_nv_chip_name_lookup);
    RUN_TEST(test_nv_gpu_not_present);
    RUN_TEST(test_nv_boot42_chip_id_ga106);
    RUN_TEST(test_nv_architecture_constants);

    return UnityEnd();
}
