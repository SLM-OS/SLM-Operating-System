/*
 * slm_ffi.c - FFI implementation for Rust runtime
 *
 * Simple wrappers around kernel functions for FFI safety.
 */

#include "slm_ffi.h"
#include "pmm.h"
#include "vmm.h"
#include "uart.h"
#include "timer.h"
#include "task.h"
#include "sched.h"
#include "ipc.h"
#include "spinlock.h"
#include "../gpu/gpu.h"

/*
 * Memory Management
 */

void *slm_alloc_pages(size_t count)
{
    return pmm_alloc_pages(count);
}

void slm_free_pages(void *addr, size_t count)
{
    pmm_free_pages(addr, count);
}

int slm_map_region(uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags)
{
    int ret = vmm_map_region(virt, phys, size, flags);
    return ret == 0 ? SLM_OK : SLM_ERR_INVALID;
}

int slm_unmap_region(uint64_t virt, uint64_t size)
{
    /* Unmap each 2MB block in the region */
    uint64_t block_size = 2 * 1024 * 1024;  /* 2MB */
    uint64_t end = virt + size;
    
    for (uint64_t addr = virt; addr < end; addr += block_size) {
        int ret = vmm_unmap_block(addr);
        if (ret != 0) {
            return SLM_ERR_INVALID;
        }
    }
    
    return SLM_OK;
}

/*
 * Debug Output
 */

void slm_print(const char *s)
{
    uart_puts(s);
}

/*
 * Timing
 */

/*
 * Convert a tick count to nanoseconds given a timer frequency in Hz.
 *
 * The naive `ticks * 1e9 / freq` overflows on x86-64: TSC at
 * ~3.4 GHz reaches UINT64_MAX / 1e9 ≈ 1.84e10 ticks after only
 * ~5.4 seconds of uptime, wrapping the multiply. Fix #171 by
 * splitting the computation along the integer division:
 *
 *   secs       = ticks / freq           (seconds of uptime)
 *   frac_ticks = ticks % freq           (0 .. freq-1)
 *   ns         = secs * 1e9 + (frac_ticks * 1e9) / freq
 *
 * Both multiplies are bounded:
 *   - `secs * 1e9`: u64 seconds × 1e9 overflows only past
 *     ~585 years of uptime.
 *   - `frac_ticks * 1e9`: `frac_ticks < freq`. For a 3.4 GHz TSC
 *     that is < 3.4e9, so the product is < 3.4e18 — well under
 *     UINT64_MAX (≈1.84e19). ARM64 platforms (1-62.5 MHz) have
 *     even more headroom.
 *
 * Fast path retained for freqs that divide 1e9 evenly (QEMU virt
 * 62.5 MHz → 16 ns/tick): same correctness, avoids two divisions.
 *
 * Exposed (not static) so `test_scheduler.c` can exercise the
 * overflow boundary with synthetic inputs — `slm_get_time_ns`
 * itself reads the real timer and cannot be driven to post-5 s
 * values in unit-test time.
 */
uint64_t slm_time_ticks_to_ns(uint64_t ticks, uint64_t freq)
{
    if (freq == 0) {
        return 0;
    }

    uint64_t ns_per_tick = 1000000000ULL / freq;
    uint64_t remainder = 1000000000ULL % freq;

    if (remainder == 0) {
        return ticks * ns_per_tick;
    }

    uint64_t secs = ticks / freq;
    uint64_t frac_ticks = ticks % freq;
    return secs * 1000000000ULL + (frac_ticks * 1000000000ULL) / freq;
}

uint64_t slm_get_time_ns(void)
{
    return slm_time_ticks_to_ns(timer_get_count(), timer_get_frequency());
}

/*
 * Sleep the current task for the given number of milliseconds.
 */
void slm_sleep_ms(uint32_t ms)
{
    sleep_ms(ms);
}

/*
 * GPU Cache Coherency
 */

void slm_gpu_sync_for_device(void *addr, size_t size)
{
    if (!gpu_available()) return;
    gpu_buffer_t buf = {
        .cpu_addr = addr,
        .gpu_addr = (uint64_t)(uintptr_t)addr,
        .size = size,
        .flags = 0,
    };
    gpu_sync_for_gpu(&buf);
}

void slm_gpu_sync_for_cpu(void *addr, size_t size)
{
    if (!gpu_available()) return;
    gpu_buffer_t buf = {
        .cpu_addr = addr,
        .gpu_addr = (uint64_t)(uintptr_t)addr,
        .size = size,
        .flags = 0,
    };
    gpu_sync_for_cpu(&buf);
}

/*
 * Task Management
 */

uint32_t slm_task_create(const char *name, slm_task_entry_t entry, void *arg)
{
    struct task *task = task_create(name, (task_entry_t)entry, arg);
    if (!task) {
        return 0;  /* ID 0 is reserved, indicates failure */
    }

    /* Add to scheduler */
    scheduler_add_task(task);

    return task->id;
}

int slm_task_set_priority(uint32_t task_id, uint8_t priority)
{
    struct task *task = task_get(task_id);
    if (!task) {
        return SLM_ERR_INVALID;
    }

    task_set_priority(task, priority);
    return SLM_OK;
}

int slm_task_set_deadline(uint32_t task_id, uint64_t deadline_ns)
{
    struct task *task = task_get(task_id);
    if (!task) {
        return SLM_ERR_INVALID;
    }

    task_set_deadline(task, deadline_ns);
    return SLM_OK;
}

uint32_t slm_task_current(void)
{
    struct task *task = task_current();
    return task ? task->id : 0;
}

/*
 * IPC - Message Queues
 */

int slm_msg_send(uint32_t queue_id, const void *msg, size_t msg_size, int timeout_ms)
{
    struct msg_queue *queue = msg_queue_lookup(queue_id);
    if (!queue) {
        return SLM_ERR_INVALID;
    }

    /* Verify message size matches queue's message size */
    if (msg_size != queue->msg_size) {
        return SLM_ERR_INVALID;
    }

    int ret = msg_send(queue, msg, timeout_ms);

    /* Translate IPC error codes to SLM error codes */
    switch (ret) {
        case IPC_OK:        return SLM_OK;
        case IPC_ERR_FULL:  return SLM_ERR_BUSY;
        case IPC_ERR_TIMEOUT: return SLM_ERR_TIMEOUT;
        default:            return SLM_ERR_INVALID;
    }
}

int slm_msg_recv(uint32_t queue_id, void *msg, size_t msg_size, int timeout_ms)
{
    struct msg_queue *queue = msg_queue_lookup(queue_id);
    if (!queue) {
        return SLM_ERR_INVALID;
    }

    /* Verify buffer size matches queue's message size */
    if (msg_size != queue->msg_size) {
        return SLM_ERR_INVALID;
    }

    int ret = msg_recv(queue, msg, timeout_ms);

    /* Translate IPC error codes to SLM error codes */
    switch (ret) {
        case IPC_OK:        return SLM_OK;
        case IPC_ERR_EMPTY: return SLM_ERR_BUSY;
        case IPC_ERR_TIMEOUT: return SLM_ERR_TIMEOUT;
        default:            return SLM_ERR_INVALID;
    }
}

/*
 * Test Support
 */

/*
 * GPU Compute (Phase 5, M3)
 */

int slm_gpu_available(void)
{
    return gpu_available() ? 1 : 0;
}

int slm_gpu_get_info(RustGpuInfo *info)
{
    if (!info) return -1;

    /* Zero the struct first */
    for (size_t i = 0; i < sizeof(RustGpuInfo); i++)
        ((uint8_t *)info)[i] = 0;

    if (!gpu_available()) {
        /* No GPU — fill with defaults */
        const char *name = "none";
        for (int i = 0; name[i] && i < 31; i++)
            info->name[i] = (uint8_t)name[i];
        return 0;
    }

    gpu_info_t gi;
    int ret = gpu_get_info(&gi);
    if (ret != 0) return -1;

    /* Copy strings */
    if (gi.name) {
        for (int i = 0; gi.name[i] && i < 31; i++)
            info->name[i] = (uint8_t)gi.name[i];
    }
    if (gi.device) {
        for (int i = 0; gi.device[i] && i < 63; i++)
            info->device[i] = (uint8_t)gi.device[i];
    }

    info->capabilities = gi.capabilities;
    info->cuda_cores = gi.cuda_cores;
    info->tensor_cores = gi.tensor_cores;
    info->memory_size = gi.memory_size;
    info->unified_memory = gi.unified_memory ? 1 : 0;

    /* Check if compute is actually ready (submit function implemented) */
    /* The gpu driver struct is internal; detect by checking capabilities */
    info->compute_ready = 0;  /* Currently no driver has submit/wait */

    return 0;
}

/*
 * Test Support
 */

/* Static test queue for FFI tests */
static struct msg_queue *ffi_test_queue = (void *)0;

uint32_t slm_ffi_get_test_queue(void)
{
    /* Create test queue on first call */
    if (!ffi_test_queue) {
        ffi_test_queue = msg_queue_create(8, 64);  /* 8 slots, 64 bytes each */
        if (!ffi_test_queue) {
            return 0;
        }
    }
    return ffi_test_queue->id;
}

/*
 * IRQ Control
 */

uint64_t slm_irq_save(void)
{
    return (uint64_t)irq_save();
}

void slm_irq_restore(uint64_t flags)
{
    irq_restore((irq_flags_t)flags);
}
