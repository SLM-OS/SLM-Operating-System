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
#ifdef PLATFORM_JETSON_ORIN_NANO
#include "../gpu/nvidia/ga10b_bringup.h"
#endif

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
 * GPU Inference (M7) — model-level entrypoints. Today only MNIST
 * is wired up; the dispatch path is the v5 multi-op pipeline that
 * scripts/gpu-kernel-mnist.c builds pre-kexec. On non-Jetson
 * platforms these are stubs returning -1.
 */

#ifdef PLATFORM_JETSON_ORIN_NANO
/* Persistent bringup state for repeat MNIST runs. First call walks
 * inherit + channel; subsequent calls reuse the channel-open state
 * and only re-dispatch the kernels.
 *
 * Concurrency: this file-scope static assumes single-threaded access
 * — today only the shell task reaches it (cmd_lua → Lua VM → these
 * FFIs, or `nvgpu` shell command via shell_sys.c). If a future
 * caller (telnetd's TCP shell, a parallel kernel task) calls these
 * concurrently, the lazy-init `ensure_mnist_bringup` below has a
 * TOCTOU window: both callers can pass the state check, both run
 * inherit+channel, and the second corrupts the first's bringup.
 * Add a spinlock around `ensure_mnist_bringup` if that ever happens.
 *
 * Note: the `nvgpu` shell command (kernel/src/shell_sys.c) keeps
 * its own function-local `struct ga10b_bringup b` that's separate
 * from this `g_mnist_bringup`. Both ultimately mutate the same
 * file-scope `g_handoff` in ga10b_bringup.c, so the underlying GPU
 * state stays consistent — but the user-visible state machines are
 * independent. A user who runs `nvgpu inherit; nvgpu channel; lua
 * print(slm.gpu_run_mnist())` triggers a redundant inherit+channel
 * walk on the FFI side. Idempotent, just wasteful. */
static struct ga10b_bringup g_mnist_bringup;

/* Walk inherit + channel if needed. Returns 0 on success, negative
 * rc if either phase fails. Callers must already be inside the
 * single-threaded assumption documented above. */
static int ensure_mnist_bringup(void)
{
    if (g_mnist_bringup.state == GA10B_BRINGUP_CHANNEL_OPEN ||
        g_mnist_bringup.state == GA10B_BRINGUP_METHOD_ACCEPTED) {
        return 0;
    }
    int rc = ga10b_bringup_inherit(&g_mnist_bringup);
    if (rc < 0) return rc;
    rc = ga10b_bringup_channel(&g_mnist_bringup);
    if (rc < 0) return rc;
    return 0;
}

int slm_gpu_run_mnist(void *logits_bytes_out)
{
    if (!logits_bytes_out) return -1;
    int rc = ensure_mnist_bringup();
    if (rc < 0) return rc;

    rc = ga10b_bringup_launch_kernel(&g_mnist_bringup);
    if (rc < 0) return rc;

    /* 4-byte * 10 = 40 bytes of fp32 bit patterns. */
    int n = ga10b_bringup_read_pipeline_output(&g_mnist_bringup,
                                                logits_bytes_out,
                                                40u);
    return n < 0 ? -1 : 0;
}
int slm_gpu_set_mnist_input(const void *bytes, size_t cap)
{
    /* NULL `bytes` falls through to ga10b_bringup_set_input, which
     * returns -3 — keeps the error code mapping one-to-one with the
     * bringup helper (-1 = no v6 handoff, -2 = cap too large, -3 =
     * bad arg). */
    int rc = ensure_mnist_bringup();
    if (rc < 0) return rc;
    int n = ga10b_bringup_set_input(&g_mnist_bringup, bytes, cap);
    return n < 0 ? n : 0;
}
int slm_gpu_set_mnist_input_fill(uint32_t value_bits, uint32_t n_floats)
{
    int rc = ensure_mnist_bringup();
    if (rc < 0) return rc;
    int n = ga10b_bringup_set_input_fill(&g_mnist_bringup, value_bits, n_floats);
    return n < 0 ? n : 0;
}
#else
int slm_gpu_run_mnist(void *logits_bytes_out)
{
    (void)logits_bytes_out;
    return -1;
}
int slm_gpu_set_mnist_input(const void *bytes, size_t cap)
{
    (void)bytes; (void)cap;
    return -1;
}
int slm_gpu_set_mnist_input_fill(uint32_t value_bits, uint32_t n_floats)
{
    (void)value_bits; (void)n_floats;
    return -1;
}
#endif

/*
 * FP-free argmax over fp32 bit patterns. The kernel target compiles
 * with -mgeneral-regs-only on AArch64, which forbids floating-point
 * comparisons in C. So we work on the fp32 bit patterns directly:
 *
 *   - sign bit is at bit 31 (1 = negative)
 *   - if both operands have the same sign:
 *       positive : larger magnitude → larger bits → use unsigned >
 *       negative : larger magnitude → larger bits → "more negative",
 *                   so larger value is the smaller-bits one
 *   - if signs differ, the positive one is larger
 *
 * This handles +0/-0 (both compare equal because IEEE 754 +0.0 has
 * bit pattern 0x00000000 and -0.0 has 0x80000000 — the algorithm
 * picks the one with positive sign, matching `> -0.0 == true` for
 * any positive value). NaN handling is undefined — none of the GPU
 * paths we wire here can produce NaN given non-NaN inputs.
 */
int slm_fp32_argmax(const void *logits_bytes, uint32_t n_logits)
{
    if (!logits_bytes || n_logits == 0u) return -1;
    const uint8_t *p = (const uint8_t *)logits_bytes;
    uint32_t best_bits;
    __builtin_memcpy(&best_bits, p, 4);
    int best_idx = 0;
    for (uint32_t i = 1u; i < n_logits; i++) {
        uint32_t cur_bits;
        __builtin_memcpy(&cur_bits, p + i * 4u, 4);

        uint32_t cur_neg  = (cur_bits  >> 31) & 1u;
        uint32_t best_neg = (best_bits >> 31) & 1u;

        int cur_is_larger;
        if (cur_neg != best_neg) {
            /* Different signs: positive wins. */
            cur_is_larger = !cur_neg;
        } else if (cur_neg) {
            /* Both negative: smaller bit pattern is less negative. */
            cur_is_larger = (cur_bits < best_bits);
        } else {
            /* Both non-negative: larger bit pattern is larger value. */
            cur_is_larger = (cur_bits > best_bits);
        }
        if (cur_is_larger) {
            best_bits = cur_bits;
            best_idx = (int)i;
        }
    }
    return best_idx;
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
