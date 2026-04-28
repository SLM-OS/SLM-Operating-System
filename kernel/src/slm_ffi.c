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
#include "gpu_consumer.h"
#include <stdatomic.h>
#include <stdbool.h>
#ifdef PLATFORM_JETSON_ORIN_NANO
#include "../gpu/nvidia/ga10b_bringup.h"
#include "../gpu/nvidia/ga10b_channel_handoff.h"  /* GA10B_PIPELINE_KIND_* */
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

int slm_gpu_inference_enabled(void)
{
    /* Mirror the master `gpu use inference` flag from gpu_consumer.c.
     * Read by the Rust engine's `mnist_gpu_fastpath_eligible` ahead
     * of any device probe to short-circuit the CPU/GPU choice
     * cleanly. */
    return gpu_consumer_enabled(GPU_CONSUMER_INFERENCE) ? 1 : 0;
}

extern int rust_model_gpu_dispatch_enabled(uint32_t index);

int slm_model_gpu_dispatch_enabled(uint32_t model_index)
{
    /* Thin wrapper around the Rust registry getter. The master
     * inference toggle is checked separately via
     * slm_gpu_inference_enabled — both must be true for the engine
     * to dispatch on GPU. */
    return rust_model_gpu_dispatch_enabled(model_index);
}

bool eviction_active_policy_has_gpu_backend(void)
{
    /* Cross the Rust/C boundary once and surface the boolean. The
     * Rust side scans both eviction pools (weight + workspace) and
     * returns 1 if either pool's installed policy declares
     * `EvictionPolicy::has_gpu_backend() == true`. Used by
     * `gpu_consumer.c` to decide whether `gpu use eviction on`
     * accepts cleanly or with a "scaffold only" warning. */
    return rust_eviction_active_policy_has_gpu_backend() != 0;
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

    /* Compute readiness: true on platforms that expose a working
     * model-level GPU dispatch entrypoint via this FFI. Today only
     * Jetson (GA10B inherit-from-Linux path) qualifies — see
     * slm_gpu_run_mnist below. The Rust runtime treats this as a
     * hint; actual dispatch failures fall back to the CPU path. */
#ifdef PLATFORM_JETSON_ORIN_NANO
    info->compute_ready = 1;
#else
    info->compute_ready = 0;
#endif

    return 0;
}

/*
 * GPU Inference (M7) — model-level entrypoints. Today only MNIST
 * is wired up; the dispatch path is the v5 multi-op pipeline that
 * scripts/gpu-kernel-mnist.c builds pre-kexec. On non-Jetson
 * platforms these are stubs returning -1.
 */

#ifdef PLATFORM_JETSON_ORIN_NANO
/* Persistent bringup state for repeat MNIST + sched MLP runs. First
 * call walks inherit + channel; subsequent calls reuse the
 * channel-open state and only re-dispatch the kernels.
 *
 * Concurrency: PR-3 of gpu-policy-models.md widened the caller
 * surface for the sched path to `ai_mlp_assign_cpu`, which the
 * scheduler invokes from any CPU during `scheduler_add_task` (no
 * scheduler-wide lock). Both `slm_gpu_run_mnist` and
 * `slm_gpu_run_sched_inference` mutate the file-scope `g_handoff`
 * (in ga10b_bringup.c) and their per-kind bringup globals. Without
 * a lock, two CPUs concurrently in this code can:
 *   1. Both pass the `ensure_*_bringup` state check, both walk
 *      inherit + channel_kind, stomping each other's writes.
 *   2. Even after first-time bringup, both write GP_PUT, ring the
 *      doorbell, and race the semaphore poll inside
 *      `ga10b_bringup_launch_kernel`.
 * `g_gpu_dispatch_lock` serialises the entire dispatch — first-time
 * setup AND steady-state launch — so only one CPU is in the
 * critical section at a time. The lock spans both kinds because
 * `g_handoff` is shared between them; the cost is that an MNIST run
 * on one CPU briefly blocks a sched run on another, which is fine
 * (the GPU has one channel either way).
 *
 * IRQ-off duration. `spin_lock_irqsave` disables local-CPU IRQs for
 * the lock's hold time. Empirical durations on jetson-nano-2:
 *   ~5 ms steady-state per dispatch (8-op QMD chain + per-op poll),
 *   ~300 ms one-time on first call (inherit + channel scan +
 *           embedded uart_printf calls busy-waiting the UART).
 * Other CPUs spin on the lock but their local IRQs stay enabled.
 * The toggle that flips this on (`gpu use sched on`) emits a
 * perf-note line so operators see the latency cliff up front; on
 * any GPU dispatch error the path falls back to CPU NEON which
 * doesn't take the lock.
 *
 * Note: the `nvgpu` shell command (kernel/src/shell_sys.c) keeps
 * its own function-local `struct ga10b_bringup b` that's separate
 * from these globals. Both ultimately mutate `g_handoff`, so a
 * `nvgpu channel` from the shell raced against an `slm_gpu_run_*`
 * call would still race. The shell command is operator-driven
 * single-shot diagnostic and not meant to interleave with FFI
 * dispatch; documenting rather than locking that path. */
static spinlock_t g_gpu_dispatch_lock = SPINLOCK_INIT;
static struct ga10b_bringup g_mnist_bringup;

/* Consecutive-dispatch-failure circuit breaker (#552 mitigation).
 *
 * `ga10b_submit_and_poll` busy-waits up to 2 s with the dispatch
 * lock held IRQ-off. Once the GPU enters a degraded state (e.g.
 * GR-engine drift or GPFIFO ring wrap aftermath: PBDMA accepts
 * the submit but the shader never runs to completion), every
 * subsequent dispatch eats the full 2 s before returning -1 and
 * falling back to CPU. With a tight inference loop, this stacks
 * up: 10 inferences/pass × 2 s = 20 s of IRQ-off per pass, which
 * starves net_pump on CPU 0 and makes the entire telnet+serial
 * shell appear wedged.
 *
 * The breaker tracks consecutive failures of either MNIST or
 * sched-MLP dispatch (they share the GPU channel and degrade
 * together). After K=3 back-to-back failures, both entry points
 * short-circuit to -1 *before* taking the lock, so the engine's
 * Rust fastpath falls back to CPU NEON immediately and the IRQ-
 * off poll is skipped entirely.
 *
 * Reset paths:
 *   1. Any successful dispatch zeroes the counter — useful if the
 *      GPU recovers on its own (channel-inherit doesn't, today,
 *      but the path is here for when we add a recovery routine).
 *   2. `gpu use inference on` calls `slm_gpu_dispatch_breaker_reset`
 *      so the operator can manually retry after a reboot/kexec.
 *
 * The architectural fix in #552 (move the poll outside the IRQ-
 * off lock) makes this breaker unnecessary, but it's a small,
 * standalone net positive while that work is in flight. */
#define GPU_DISPATCH_FAILURE_THRESHOLD 3

static atomic_int g_gpu_consecutive_failures = 0;

static inline bool gpu_dispatch_breaker_tripped(void)
{
    return atomic_load_explicit(&g_gpu_consecutive_failures,
                                memory_order_relaxed)
        >= GPU_DISPATCH_FAILURE_THRESHOLD;
}

/* Update the failure counter after a dispatch attempt. Emits a
 * one-shot WARN exactly when the threshold is crossed (not on
 * every subsequent failure), so the serial log isn't flooded once
 * the breaker is tripped. */
static void gpu_dispatch_record_result(int rc)
{
    if (rc >= 0) {
        atomic_store_explicit(&g_gpu_consecutive_failures, 0,
                              memory_order_relaxed);
        return;
    }
    int prev = atomic_fetch_add_explicit(&g_gpu_consecutive_failures, 1,
                                          memory_order_relaxed);
    if (prev + 1 == GPU_DISPATCH_FAILURE_THRESHOLD) {
        uart_printf("[WARN] gpu-dispatch: %d consecutive failures — "
                    "circuit breaker tripped, fastpath disabled until "
                    "next successful dispatch or `gpu use inference on` "
                    "(see #552)\n",
                    GPU_DISPATCH_FAILURE_THRESHOLD);
    }
}

void slm_gpu_dispatch_breaker_reset(void)
{
    int prev = atomic_exchange_explicit(&g_gpu_consecutive_failures, 0,
                                         memory_order_relaxed);
    if (prev >= GPU_DISPATCH_FAILURE_THRESHOLD) {
        uart_printf("[INFO] gpu-dispatch: circuit breaker reset "
                    "(was tripped at %d consecutive failures)\n", prev);
    }
}

bool slm_gpu_dispatch_breaker_is_tripped(void)
{
    return gpu_dispatch_breaker_tripped();
}

/* Walk inherit + channel if needed. Returns 0 on success, negative
 * rc if either phase fails. Callers must already be inside the
 * single-threaded assumption documented above.
 *
 * PR-3 of gpu-policy-models.md: also detects when a different bringup
 * instance (e.g. g_sched_bringup) has overwritten the shared
 * g_handoff with a different pipeline_kind — in that case our state
 * is stale and we re-walk channel_kind to repopulate g_handoff with
 * MNIST data. */
static int ensure_mnist_bringup(void)
{
    bool channel_open = (g_mnist_bringup.state == GA10B_BRINGUP_CHANNEL_OPEN ||
                         g_mnist_bringup.state == GA10B_BRINGUP_METHOD_ACCEPTED);
    bool active_is_mnist = (ga10b_bringup_active_pipeline_kind() ==
                            GA10B_PIPELINE_KIND_MNIST);
    if (channel_open && active_is_mnist) return 0;

    int rc = ga10b_bringup_inherit(&g_mnist_bringup);
    if (rc < 0) return rc;
    rc = ga10b_bringup_channel_kind(&g_mnist_bringup,
                                     GA10B_PIPELINE_KIND_MNIST);
    if (rc < 0) return rc;
    return 0;
}

/* Sched MLP dispatch — parallel to MNIST. Separate per-instance
 * bringup state but shared g_handoff (kernel singleton); the
 * pipeline_kind check in ensure_*_bringup detects stale state from
 * cross-instance overwrites and re-runs channel_kind to repopulate
 * g_handoff with the right kind. */
static struct ga10b_bringup g_sched_bringup;

static int ensure_sched_bringup(void)
{
    bool channel_open = (g_sched_bringup.state == GA10B_BRINGUP_CHANNEL_OPEN ||
                         g_sched_bringup.state == GA10B_BRINGUP_METHOD_ACCEPTED);
    bool active_is_sched = (ga10b_bringup_active_pipeline_kind() ==
                            GA10B_PIPELINE_KIND_SCHED_MLP);
    if (channel_open && active_is_sched) return 0;

    int rc = ga10b_bringup_inherit(&g_sched_bringup);
    if (rc < 0) return rc;
    rc = ga10b_bringup_channel_kind(&g_sched_bringup,
                                     GA10B_PIPELINE_KIND_SCHED_MLP);
    if (rc < 0) return rc;
    return 0;
}

int slm_gpu_run_mnist(void *logits_bytes_out)
{
    if (!logits_bytes_out) return -1;
    /* Short-circuit before taking the IRQ-off lock if the breaker
     * has been tripped. Engine.rs handles -1 by falling back to
     * CPU NEON, so we just produce that rc. */
    if (gpu_dispatch_breaker_tripped()) return -1;

    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);
    int rc = ensure_mnist_bringup();
    if (rc < 0) goto out;
    rc = ga10b_bringup_launch_kernel(&g_mnist_bringup);
    if (rc < 0) goto out;
    /* 4-byte * 10 = 40 bytes of fp32 bit patterns. */
    int n = ga10b_bringup_read_pipeline_output(&g_mnist_bringup,
                                                logits_bytes_out,
                                                40u);
    rc = n < 0 ? -1 : 0;
out:
    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    gpu_dispatch_record_result(rc);
    return rc;
}
int slm_gpu_set_mnist_input(const void *bytes, size_t cap)
{
    /* NULL `bytes` falls through to ga10b_bringup_set_input, which
     * returns -3 — keeps the error code mapping one-to-one with the
     * bringup helper (-1 = no v6 handoff, -2 = cap too large, -3 =
     * bad arg). */
    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);
    int rc = ensure_mnist_bringup();
    if (rc < 0) goto out;
    int n = ga10b_bringup_set_input(&g_mnist_bringup, bytes, cap);
    rc = n < 0 ? n : 0;
out:
    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    return rc;
}
int slm_gpu_set_mnist_input_fill(uint32_t value_bits, uint32_t n_floats)
{
    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);
    int rc = ensure_mnist_bringup();
    if (rc < 0) goto out;
    int n = ga10b_bringup_set_input_fill(&g_mnist_bringup, value_bits, n_floats);
    rc = n < 0 ? n : 0;
out:
    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    return rc;
}

/* Sched MLP dispatch. Same shape as slm_gpu_run_mnist but reads
 * a 42-element fp32 logits vector (AI_SCHED_N_ACTIONS = 42 on
 * Jetson) instead of MNIST's 10-element output. The caller passes
 * the 108-element fp32 feature vector as `state_bytes`; the engine
 * does set_input + launch_kernel + read_output in one shot. */
int slm_gpu_run_sched_inference(const void *state_bytes,
                                 size_t state_bytes_len,
                                 void *logits_bytes_out)
{
    if (!state_bytes || !logits_bytes_out) return -1;
    /* Same circuit-breaker short-circuit as the MNIST path. The
     * counter is shared between MNIST + sched-MLP because they
     * use the same channel and degrade together. */
    if (gpu_dispatch_breaker_tripped()) return -1;

    /* Held across the whole set_input + launch_kernel + read_output
     * sequence so two CPUs concurrently in `ai_mlp_assign_cpu` can't
     * stomp the shared g_handoff or race the GPFIFO write + doorbell
     * + semaphore poll. See the lock-comment block above
     * g_gpu_dispatch_lock for full rationale. */
    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);
    int rc = ensure_sched_bringup();
    if (rc < 0) goto out;

    int n = ga10b_bringup_set_input(&g_sched_bringup, state_bytes,
                                     state_bytes_len);
    if (n < 0) { rc = n; goto out; }

    rc = ga10b_bringup_launch_kernel(&g_sched_bringup);
    if (rc < 0) goto out;

    /* 4-byte * 42 = 168 bytes of fp32 bit patterns. */
    n = ga10b_bringup_read_pipeline_output(&g_sched_bringup,
                                            logits_bytes_out,
                                            168u);
    rc = n < 0 ? -1 : 0;
out:
    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    gpu_dispatch_record_result(rc);
    return rc;
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
int slm_gpu_run_sched_inference(const void *state_bytes,
                                 size_t state_bytes_len,
                                 void *logits_bytes_out)
{
    (void)state_bytes; (void)state_bytes_len; (void)logits_bytes_out;
    return -1;
}
void slm_gpu_dispatch_breaker_reset(void) { }
bool slm_gpu_dispatch_breaker_is_tripped(void) { return false; }
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
