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

uint64_t slm_get_time_ns(void)
{
    uint64_t ticks = timer_get_count();
    uint64_t freq = timer_get_frequency();

    /*
     * Convert ticks to nanoseconds: ns = ticks * 1e9 / freq
     * To avoid overflow with large tick counts, we compute:
     * ns = ticks * (1e9 / freq) when freq divides 1e9 evenly,
     * otherwise use: ns = (ticks / freq) * 1e9 + ((ticks % freq) * 1e9) / freq
     *
     * For QEMU virt (62.5 MHz), 1e9/freq = 16, so each tick = 16ns.
     */
    if (freq == 0) {
        return 0;
    }

    uint64_t ns_per_tick = 1000000000ULL / freq;
    uint64_t remainder = 1000000000ULL % freq;

    /* Fast path for common frequencies that divide evenly */
    if (remainder == 0) {
        return ticks * ns_per_tick;
    }

    /* General case with remainder handling */
    return ticks * ns_per_tick + (ticks * remainder) / freq;
}

/*
 * Task Management
 */

void *slm_task_create(const char *name, slm_task_entry_t entry, void *arg)
{
    struct task *task = task_create(name, (task_entry_t)entry, arg);
    if (!task) {
        return (void *)0;
    }

    /* Add to scheduler */
    scheduler_add_task(task);

    return (void *)task;
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
