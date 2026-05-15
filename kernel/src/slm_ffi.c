/*
 * slm_ffi.c - FFI implementation for Rust runtime
 *
 * Simple wrappers around kernel functions for FFI safety.
 */

#include "slm_ffi.h"
#if !defined(PLATFORM_X86_64)
#include "kbuf.h"
#endif
/* Pull gpu_handoff.h into a translation unit so its _Static_assert
 * sizes are actually exercised on every kernel build (catches struct
 * drift the moment the C header is touched). */
#include "gpu_handoff.h"
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
#include <stdint.h>
#ifdef PLATFORM_JETSON_ORIN_NANO
#include "../gpu/nvidia/ga10b_bringup.h"
#include "../gpu/nvidia/ga10b_channel_handoff.h"  /* GA10B_PIPELINE_KIND_* */
#include "operator_dispatch.h"      /* struct operator_dispatch_args */
#include "oplib_dispatch.h"         /* slm_oplib_dispatch */
#include "oplib_pool.h"             /* OPLIB_POOL_* slot offsets */
#include "oplib_weights_pool.h"     /* W2 weight staging */
/* cache_clean_range / cache_invalidate_range come from gpu.h above
 * (already included on the non-Jetson side). The kernel/include/cache.h
 * variant has a stricter signature (const volatile void *) and would
 * conflict with gpu.h's declaration if pulled in here. */
#include <string.h>                 /* memcpy */
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
    /* The Rust `kernel_ffi::free_pages` shim calls this with the
     * exact pointer that was handed to it. Two producers feed those
     * pointers today:
     *   - `slm_alloc_pages` (above) → plain `pmm_alloc_pages`. The
     *     pointer lives in the identity map.
     *   - `slm xload` → `kbuf_alloc` (#789). The pointer may be a
     *     fast-path PMM block (identity-mapped) OR a chunked VMM
     *     mapping in the kbuf VA window. Either way kbuf knows how
     *     to release it.
     *
     * `kbuf_owns_va` looks up the pointer in kbuf's bookkeeping
     * table and is the authoritative signal — relying on a VA range
     * test alone would miss the fast-path PMM allocations kbuf
     * also tracks. */
#if !defined(PLATFORM_X86_64)
    if (addr != NULL && kbuf_owns_va(addr)) {
        kbuf_free(addr);
        return;
    }
#endif
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
    for (size_t i = 0; i < sizeof(RustGpuInfo); i++) {
        ((uint8_t *)info)[i] = 0;
    }

    if (!gpu_available()) {
        /* No GPU — fill with defaults */
        const char *name = "none";
        for (int i = 0; name[i] && i < 31; i++) {
            info->name[i] = (uint8_t)name[i];
        }
        return 0;
    }

    gpu_info_t gi;
    int ret = gpu_get_info(&gi);
    if (ret != 0) return -1;

    /* Copy strings */
    if (gi.name) {
        for (int i = 0; gi.name[i] && i < 31; i++) {
            info->name[i] = (uint8_t)gi.name[i];
        }
    }
    if (gi.device) {
        for (int i = 0; gi.device[i] && i < 63; i++) {
            info->device[i] = (uint8_t)gi.device[i];
        }
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

/* Sched-MLP dispatch rate-limit window (#651). Defined on every
 * platform so the pure-logic predicate below — and the test seam in
 * test_gpu_dispatch_breaker.c — compile cross-platform. The atomic
 * timestamp it gates (`g_sched_dispatch_last_ns`) and the call-site
 * that consults it both live inside the Jetson `#ifdef` block. */
#define GPU_SCHED_DISPATCH_RATE_LIMIT_NS  50000000ULL  /* 50 ms */

/* Pure-logic admission-control predicate. Returns true iff the
 * dispatch should be rejected because the previous dispatch
 * completed less than RATE_LIMIT_NS ago. Unsigned subtraction makes
 * the first call (last == 0) always pass, and is well-defined under
 * timer wraparound (any underflow lands far above the window
 * threshold, so a wrap simply re-opens the gate — safe-by-default,
 * since rejected dispatches fall back to CPU NEON anyway). Tested
 * directly via `slm_gpu_sched_dispatch_test_within_window`. */
static inline bool sched_dispatch_within_rate_limit(uint64_t now,
                                                     uint64_t last)
{
    return (now - last) < GPU_SCHED_DISPATCH_RATE_LIMIT_NS;
}

bool slm_gpu_sched_dispatch_test_within_window(uint64_t now, uint64_t last)
{
    return sched_dispatch_within_rate_limit(now, last);
}

#ifdef PLATFORM_JETSON_ORIN_NANO
/* Per-kind dispatch lock — serialises every `slm_gpu_*` entry
 * point (run / set_input / set_input_fill / run_with_input and
 * their kind-named shims) so only one dispatch is in flight at a
 * time, regardless of which CPU the caller runs on.
 *
 * Concurrency rationale: PR-3 of gpu-policy-models.md widened the
 * caller surface for the sched path to `ai_mlp_assign_cpu`, which
 * the scheduler invokes from any CPU during `scheduler_add_task`
 * (no scheduler-wide lock). All `slm_gpu_*` entry points mutate
 * the file-scope `g_handoff` (in ga10b_bringup.c) and the per-
 * kind `g_bringups[]` slot. Without a lock, two CPUs concurrently
 * in this code can:
 *   1. Both pass the `ensure_bringup` state check, both walk
 *      inherit + channel_kind, stomping each other's writes.
 *   2. Even after first-time bringup, both write GP_PUT, ring the
 *      doorbell, and race the semaphore poll inside
 *      `ga10b_bringup_launch_kernel`.
 * `g_gpu_dispatch_lock` serialises the entire dispatch — first-
 * time setup AND steady-state launch — so only one CPU is in the
 * critical section at a time. The lock spans all kinds because
 * `g_handoff` is shared between them; the cost is that a kind=A
 * run on one CPU briefly blocks a kind=B run on another, which is
 * fine (the GPU has one channel either way and PBDMA is sequential
 * on it).
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
 * `nvgpu channel` from the shell raced against an `slm_gpu_*`
 * call would still race. The shell command is operator-driven
 * single-shot diagnostic and not meant to interleave with FFI
 * dispatch; documenting rather than locking that path. */
static spinlock_t g_gpu_dispatch_lock = SPINLOCK_INIT;

/* Per-kind bringup state. One slot per `enum ga10b_pipeline_kind`
 * value, indexed directly by the kind. Each slot caches the GPU
 * channel inheritance + handoff lookup for that pipeline kind so
 * subsequent dispatches skip the inherit + scan. The slots share
 * `g_handoff` (a kernel singleton populated by
 * `ga10b_bringup_channel_kind`); when a different-kind dispatch
 * has overwritten g_handoff since this slot's last successful
 * call, `ensure_bringup` re-runs `ga10b_bringup_channel_kind` to
 * repopulate it. The state-machine fields in struct ga10b_bringup
 * (state, last_error_phase, etc.) stay private per slot.
 *
 * Bug-A warmup is also per-slot: each kind needs one throwaway
 * inference after a fresh inherit so the cold-start grid quirk
 * doesn't corrupt the user's first call. See Bug A comment in
 * `ensure_bringup` below for the full rationale. */
static struct ga10b_bringup g_bringups[GA10B_PIPELINE_KIND_COUNT];

/* Per-kind "first user dispatch after fresh inherit needs a throwaway"
 * flag. Set in `ensure_bringup` immediately after the in-bringup
 * warmup absorbs Bug A; cleared in the user-call path
 * (slm_gpu_run / slm_gpu_run_with_input) on the next dispatch.
 *
 * Why this is separate from the existing in-bringup warmup: the
 * warmup absorbs Bug A (very-first-dispatch-returns-all-zeros) but
 * does NOT reliably clear residual GPU L2 lines inherited from
 * Linux's nvgpu helper. PR #644 (gpu/l2-evict-sequence) does the
 * documented L2 evict on inherit, which closes that race for the
 * common path; this flag adds belt-and-suspenders for any L2 lines
 * that re-fill between inherit and the first user dispatch. The
 * effect is that the first user call after a fresh kexec runs
 * launch_kernel twice and returns the second result, so any stale
 * cache traffic during dispatch #1 is followed by a clean dispatch
 * #2 whose output is what the user sees. ~17% pre-fix iter-1 rate
 * tracked in #596. */
static bool g_post_inherit_double_dispatch_pending
    [GA10B_PIPELINE_KIND_COUNT] = { false };

/* Sched-MLP dispatch rate limiter (#651).
 *
 * `gpu use sched on` with `ai_mlp` invokes `slm_gpu_run_sched_inference`
 * from every `ai_mlp_assign_cpu` call — i.e. every task_create / steal
 * across all six CPUs. Each dispatch holds `g_gpu_dispatch_lock` IRQ-
 * off for ~5 ms; a task burst piles up dozens of waiters and starves
 * timer ticks / RX-stall watchdogs / shell scheduling enough to wedge
 * the box (telnet stops, serial stops, hard power-cycle required —
 * 2026-04-27 jetson-nano-2 reproducer).
 *
 * The limiter is system-wide rather than per-CPU: at most one GPU
 * sched dispatch may *complete* in any RATE_LIMIT_NS window. The
 * timestamp `g_sched_dispatch_last_ns` is updated *after* a successful
 * dispatch (see comment near the store), so the next allowed dispatch
 * starts at least RATE_LIMIT_NS after the previous one finishes —
 * about 55 ms start-to-start for a 5 ms dispatch with a 50 ms window.
 * Rejected dispatches return -1, and `forward_via_device` already has
 * a silent CPU NEON fallback for that case
 * (kernel/sched/ai/ai_inference.c:280-289).
 *
 * Effective worst-case IRQ-off load on the system: in steady state
 * only one CPU at a time wins the try-lock, so the dispatch path
 * burns ≤ 5 ms per ≥ 55 ms interval ≈ 9 % of one CPU. Other CPUs'
 * `assign_cpu` calls return -1 from the rate-limit check (or from
 * the try-lock) without ever spinning IRQ-off. */
static _Atomic uint64_t g_sched_dispatch_last_ns = 0;

/* Consecutive-dispatch-failure circuit breaker.
 *
 * `ga10b_submit_and_poll` busy-waits up to 2 s with the dispatch
 * lock held IRQ-off. Once the GPU enters a degraded state (GR-
 * engine drift, GPFIFO ring-wrap aftermath: PBDMA accepts the
 * submit but the shader never runs to completion), every
 * subsequent dispatch eats the full 2 s before returning -1 and
 * falling back to CPU. With a tight inference loop, this stacks
 * up: 10 inferences/pass × 2 s = 20 s of IRQ-off per pass, which
 * starves net_pump on CPU 0 and makes the entire telnet+serial
 * shell appear wedged.
 *
 * The breaker tracks consecutive failures of either MNIST or
 * sched-MLP dispatch. After K back-to-back failures, both entry
 * points short-circuit to -1 *before* taking the lock, so the
 * engine's Rust fastpath falls back to CPU NEON immediately and
 * the IRQ-off poll is skipped entirely.
 *
 * Reset paths:
 *   1. Any successful dispatch zeroes the counter — useful if the
 *      GPU recovers on its own (channel-inherit doesn't, today,
 *      but the path is here for when we add a recovery routine).
 *   2. `gpu use inference on` calls `slm_gpu_dispatch_breaker_reset`
 *      so the operator can manually retry after a reboot/kexec.
 *
 * Shared-counter assumption: the same atomic counter spans both
 * MNIST and sched-MLP because they share `g_gpu_dispatch_lock` and
 * the same GPU channel; if MNIST starts failing because the
 * channel has degraded, sched-MLP will too. The static_assert
 * below pins this invariant — if a future bringup splits these
 * paths onto separate channels, the breaker must split into
 * two counters at the same time or the healthy path will be
 * silently quenched. (#552 review S5.)
 *
 * Tracking issue for the architectural fix that supersedes this
 * mitigation is in MEMORY (and the issue tracker); the WARN
 * below intentionally describes only the observable behaviour
 * so the message doesn't bit-rot when the issue closes. */

/* Threshold is overridable from the command line via
 *   make kernel GPU_DISPATCH_BREAKER_THRESHOLD=N
 * (or `cmake -DGPU_DISPATCH_BREAKER_THRESHOLD=N`). Default 3 —
 * high enough that one transient timeout doesn't disable the
 * fastpath, low enough that a real wedge is contained within
 * ~6 s of IRQ-off time. */
#ifndef GPU_DISPATCH_BREAKER_THRESHOLD
#define GPU_DISPATCH_BREAKER_THRESHOLD 3
#endif

/* `memory_order_relaxed` everywhere the counter is touched: the
 * counter is a *hint*, not a synchroniser. A transient stale read
 * either causes one extra IRQ-off attempt (we miss a recent
 * trip) or one premature short-circuit (we see a trip that's
 * about to be reset) — both are already-tolerated outcomes since
 * the dispatch path falls back to CPU on either. Don't "fix" this
 * to seq_cst without understanding the trade-off. */
static atomic_int g_gpu_consecutive_failures = 0;

static inline bool gpu_dispatch_breaker_is_tripped_inner(void)
{
    return atomic_load_explicit(&g_gpu_consecutive_failures,
                                memory_order_relaxed)
        >= GPU_DISPATCH_BREAKER_THRESHOLD;
}

/* Update the failure counter after a dispatch attempt. Emits a
 * one-shot WARN exactly when the threshold is crossed (not on
 * every subsequent failure), so the serial log isn't flooded once
 * the breaker is tripped.
 *
 * Called from `slm_gpu_run_*` AFTER `spin_unlock_irqrestore` on
 * `g_gpu_dispatch_lock` — uart_printf takes its own UART lock,
 * and printing while holding the dispatch lock would create a
 * lock-order inversion with any path that already holds UART. */
static void gpu_dispatch_record_result(int rc)
{
    if (rc >= 0) {
        atomic_store_explicit(&g_gpu_consecutive_failures, 0,
                              memory_order_relaxed);
        return;
    }
    int prev = atomic_fetch_add_explicit(&g_gpu_consecutive_failures, 1,
                                          memory_order_relaxed);
    if (prev + 1 == GPU_DISPATCH_BREAKER_THRESHOLD) {
        uart_printf("[WARN] gpu-dispatch: %d consecutive failures — "
                    "circuit breaker tripped, fastpath disabled until "
                    "next successful dispatch or `gpu use inference on`\n",
                    GPU_DISPATCH_BREAKER_THRESHOLD);
    }
}

void slm_gpu_dispatch_breaker_reset(void)
{
    int prev = atomic_exchange_explicit(&g_gpu_consecutive_failures, 0,
                                         memory_order_relaxed);
    if (prev >= GPU_DISPATCH_BREAKER_THRESHOLD) {
        uart_printf("[INFO] gpu-dispatch: circuit breaker reset "
                    "(was tripped at %d consecutive failures)\n", prev);
    }
}

/* Public mirror of the static `_inner` predicate. Exposed for an
 * upcoming `gpu use status` shell extension that will surface the
 * breaker state alongside the consumer toggles; today no caller
 * exists outside the test seam below. The wrapper exists so the
 * external symbol matches the `slm_gpu_*` naming used by the rest
 * of the FFI surface, while the hot path inside this TU keeps the
 * unprefixed name. */
bool slm_gpu_dispatch_breaker_is_tripped(void)
{
    return gpu_dispatch_breaker_is_tripped_inner();
}

/* Set-input wrapper around gpu_dispatch_record_result that filters
 * out caller-side validation errors. `slm_gpu_set_mnist_input` /
 * `_fill` return -1 (no v6 handoff / GPU dispatch failure — counts
 * toward the breaker), -2 (cap too large) or -3 (bad arg) — both
 * caller-side bugs. Recording -2/-3 would let a misbehaving caller
 * (e.g. Rust passing NULL three times in a row) trip the breaker
 * spuriously and take the GPU offline for a reason unrelated to GPU
 * health. The breaker's contract is "consecutive GPU dispatch
 * failures", so keep -2/-3 out of the counter entirely — neither
 * increment nor reset. (PR #555 round-3 review.) */
static void gpu_dispatch_record_set_input_result(int rc)
{
    if (rc == 0 || rc == -1) {
        gpu_dispatch_record_result(rc);
    }
}

/* Test-only seam: drive the breaker state machine without going
 * through a real GPU dispatch. The unit tests in
 * `kernel/tests/test_gpu_dispatch_breaker.c` exercise the
 * threshold trip, single-success reset, manual reset clears, and
 * the one-shot WARN invariants. Wraps `gpu_dispatch_record_result`
 * which is otherwise only reachable from inside this TU. */
void slm_gpu_dispatch_breaker_test_record(int rc)
{
    gpu_dispatch_record_result(rc);
}

/* Test-only seam: read the raw counter so tests can assert exact
 * values. Atomically loaded for safety even though tests run
 * single-threaded. */
int slm_gpu_dispatch_breaker_test_count(void)
{
    return atomic_load_explicit(&g_gpu_consecutive_failures,
                                memory_order_relaxed);
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
/* Cache + warmup state for one pipeline_kind. Idempotent: returns
 * 0 immediately if `g_bringups[kind]` is already in a usable state
 * AND `g_handoff` currently holds that kind; otherwise re-runs
 * inherit + channel_kind to repopulate `g_handoff` and runs a
 * one-shot warmup dispatch.
 *
 * Caller MUST hold `g_gpu_dispatch_lock`. The body invokes
 * `ga10b_bringup_launch_kernel` (the warmup), which assumes
 * exclusive access to the channel + handoff state.
 *
 * Bug A workaround (see #596): the very first compute dispatch
 * after channel inheritance returns all-zero logits — the GPU's
 * cold-start grid silently doesn't write its output buffer.
 * Subsequent dispatches in the same channel produce correct
 * deterministic output. Running one throwaway inference here
 * absorbs the cold start so user-visible calls see warm state.
 * Cost: one extra pipeline dispatch per fresh inherit (i.e., once
 * per kexec session, or once after a different-kind bringup
 * overwrites `g_handoff`).
 *
 * Lock-hold-time note: callers run inside `g_gpu_dispatch_lock`
 * with IRQs disabled. This warmup combines with the post-inherit
 * throwaway dispatch (Option A for #596 — see
 * `g_post_inherit_double_dispatch_pending`) to triple the
 * worst-case lock-hold on first dispatch: warmup + throwaway +
 * user's real dispatch all run under the same lock acquisition
 * before the user gets their result. Typical cost is ~150-600 ms
 * (3 × one MNIST inference at ~50-200 ms each); pathological
 * (hung GPU on every op) is bounded by `ga10b_submit_and_poll`'s
 * 2 s per-op × 8-op MNIST pipeline × 3 dispatches ≈ 48 s of IRQ-
 * off in the worst case. A future async-warmup-from-kernel_main
 * move would relieve all three. */
static int ensure_bringup(uint32_t kind)
{
    if (kind >= GA10B_PIPELINE_KIND_COUNT) {
        /* Garbage kind from a buggy caller (off-by-one in an enum,
         * uninitialized variable, etc.). Surface it — silent -1
         * looks identical to a tripped breaker or missing handoff,
         * and the bug would be hard to spot in production logs. */
        uart_printf("[gpu] ensure_bringup: invalid kind=%lu "
                    "(max=%u) — caller bug\n",
                    (unsigned long)kind,
                    (unsigned)GA10B_PIPELINE_KIND_COUNT - 1u);
        return -1;
    }
    struct ga10b_bringup *b = &g_bringups[kind];

    bool channel_open = (b->state == GA10B_BRINGUP_CHANNEL_OPEN ||
                         b->state == GA10B_BRINGUP_METHOD_ACCEPTED);
    bool active_is_kind = (ga10b_bringup_active_pipeline_kind() == kind);
    if (channel_open && active_is_kind) return 0;

    int rc = ga10b_bringup_inherit(b);
    if (rc < 0) return rc;
    rc = ga10b_bringup_channel_kind(b, kind);
    if (rc < 0) return rc;

    int warmup_rc = ga10b_bringup_launch_kernel(b);
    if (warmup_rc < 0) {
        /* Non-fatal: the warmup failing doesn't itself prevent the
         * user's call. If a real dispatch problem persists, the
         * caller's launch_kernel will surface it. */
        uart_printf("[gpu-kind=%lu] warmup dispatch returned %d "
                    "(continuing — first user call may see Bug A)\n",
                    (unsigned long)kind, warmup_rc);
    }

    /* Arm the post-inherit double-dispatch for the next user call.
     * See g_post_inherit_double_dispatch_pending comment for why this
     * is a second safety net layered on top of the warmup + the L2
     * evict in ga10b_bringup_inherit. */
    g_post_inherit_double_dispatch_pending[kind] = true;
    return 0;
}

/* Consume the post-inherit double-dispatch flag if armed: run one
 * throwaway launch_kernel and clear the flag. Both user-call sites
 * (slm_gpu_run, slm_gpu_run_with_input) need the identical sequence,
 * so factoring it out keeps them in lockstep.
 *
 * Non-fatal: a discarded-dispatch failure logs but does not
 * propagate — the user dispatch that follows will surface any
 * persistent problem with its own rc. Caller must hold
 * g_gpu_dispatch_lock and have already verified `kind` is in range
 * (ensure_bringup does that). */
static void consume_post_inherit_double_dispatch(uint32_t kind,
                                                  struct ga10b_bringup *b)
{
    if (!g_post_inherit_double_dispatch_pending[kind]) return;
    g_post_inherit_double_dispatch_pending[kind] = false;
    int discard_rc = ga10b_bringup_launch_kernel(b);
    if (discard_rc < 0) {
        uart_printf("[gpu-kind=%lu] post-inherit throwaway dispatch "
                    "returned %d (continuing to user dispatch)\n",
                    (unsigned long)kind, discard_rc);
    }
}

/* ---- Generic kind-parameterized GPU dispatch FFI ----
 *
 * The functions below take `kind` as the first argument and route
 * through the per-kind cache in `g_bringups[]`. The kind-named
 * shims (slm_gpu_run_mnist, slm_gpu_run_sched_inference, etc.)
 * stay below as 1-line wrappers for backward compatibility with
 * existing Lua / Rust callers; new model integrations should
 * call the generic functions directly with the kind constant. */

int slm_gpu_run(uint32_t kind, void *output, size_t output_cap)
{
    if (!output) return -1;
    if (gpu_dispatch_breaker_is_tripped_inner()) return -1;

    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);
    int rc = ensure_bringup(kind);
    if (rc < 0) goto out;
    struct ga10b_bringup *b = &g_bringups[kind];

    /* Option A for #596 — one throwaway launch on the first user
     * call after a fresh inherit. */
    consume_post_inherit_double_dispatch(kind, b);

    rc = ga10b_bringup_launch_kernel(b);
    if (rc < 0) goto out;
    int n = ga10b_bringup_read_pipeline_output(b, output, output_cap);
    rc = n < 0 ? -1 : 0;
out:
    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    gpu_dispatch_record_result(rc);
    return rc;
}

int slm_gpu_set_input(uint32_t kind, const void *bytes, size_t cap)
{
    /* NULL `bytes` falls through to ga10b_bringup_set_input, which
     * returns -3 — preserves the error mapping (-1 no v6 handoff,
     * -2 cap too large, -3 bad arg) callers depend on. */
    if (gpu_dispatch_breaker_is_tripped_inner()) return -1;
    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);
    int rc = ensure_bringup(kind);
    if (rc < 0) goto out;
    int n = ga10b_bringup_set_input(&g_bringups[kind], bytes, cap);
    rc = n < 0 ? n : 0;
out:
    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    /* Filtered: only -1 / 0 reach the breaker counter. -2 (cap too
     * large) and -3 (bad arg) are caller-side validation errors. */
    gpu_dispatch_record_set_input_result(rc);
    return rc;
}

int slm_gpu_set_input_fill(uint32_t kind, uint32_t value_bits,
                           uint32_t n_floats)
{
    if (gpu_dispatch_breaker_is_tripped_inner()) return -1;
    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);
    int rc = ensure_bringup(kind);
    if (rc < 0) goto out;
    int n = ga10b_bringup_set_input_fill(&g_bringups[kind], value_bits,
                                          n_floats);
    rc = n < 0 ? n : 0;
out:
    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    gpu_dispatch_record_set_input_result(rc);
    return rc;
}

/* Inner dispatch sequence shared by the blocking + try-lock entry
 * points. Caller must already hold `g_gpu_dispatch_lock` IRQ-off.
 * Returns 0 on success, negative on failure. */
static int slm_gpu_dispatch_locked(uint32_t kind,
                                    const void *input, size_t input_cap,
                                    void *output, size_t output_cap)
{
    int rc = ensure_bringup(kind);
    if (rc < 0) return rc;

    struct ga10b_bringup *b = &g_bringups[kind];
    int n = ga10b_bringup_set_input(b, input, input_cap);
    if (n < 0) return n;

    /* Option A for #596. Throwaway dispatch is placed *after*
     * set_input so the discarded launch already sees the user's
     * fresh input — both dispatches read the same input buffer, the
     * user just sees the second result. */
    consume_post_inherit_double_dispatch(kind, b);

    rc = ga10b_bringup_launch_kernel(b);
    if (rc < 0) return rc;

    n = ga10b_bringup_read_pipeline_output(b, output, output_cap);
    return n < 0 ? -1 : 0;
}

/* All-in-one set_input + launch + read under a single lock
 * acquisition. Used by callers that produce input bytes
 * synchronously and want output in the same call (e.g.
 * sched-MLP's `assign_cpu` path, where holding the lock across
 * the whole sequence prevents a concurrent dispatcher from
 * overwriting g_handoff between set_input and launch). */
int slm_gpu_run_with_input(uint32_t kind,
                           const void *input, size_t input_cap,
                           void *output, size_t output_cap)
{
    if (!input || !output) return -1;
    if (gpu_dispatch_breaker_is_tripped_inner()) return -1;

    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);
    int rc = slm_gpu_dispatch_locked(kind, input, input_cap,
                                      output, output_cap);
    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    gpu_dispatch_record_result(rc);
    return rc;
}

/* Try-lock variant of slm_gpu_run_with_input (#651).
 *
 * Identical body to slm_gpu_run_with_input but uses spin_trylock
 * instead of spin_lock_irqsave: if g_gpu_dispatch_lock is held by
 * another CPU, return -1 immediately so the caller can fall through
 * to its own fallback (CPU NEON for the sched-MLP path) rather than
 * burn IRQ-off time spinning on a long-running dispatch.
 *
 * Designed for the sched-MLP hot path where any latency in
 * `assign_cpu` directly translates to scheduler stall. Not suitable
 * for callers that need the GPU result to actually drive the
 * decision (those want the blocking variant). Returns -1 on:
 *   - lock contention (try-lock failed),
 *   - any other error path that the blocking variant returns -1 for.
 * The caller cannot distinguish the two; both should silently fall
 * back to CPU. */
static int slm_gpu_try_run_with_input(uint32_t kind,
                                       const void *input, size_t input_cap,
                                       void *output, size_t output_cap)
{
    if (!input || !output) return -1;
    if (gpu_dispatch_breaker_is_tripped_inner()) return -1;

    irq_flags_t irq = irq_save();
    if (!spin_trylock(&g_gpu_dispatch_lock)) {
        irq_restore(irq);
        /* Lock contention is admission-control rejection, not a
         * dispatch failure — don't feed the consecutive-failure
         * breaker counter (it would trip on legitimate burst
         * traffic). */
        return -1;
    }

    int rc = slm_gpu_dispatch_locked(kind, input, input_cap,
                                      output, output_cap);
    spin_unlock(&g_gpu_dispatch_lock);
    irq_restore(irq);
    gpu_dispatch_record_result(rc);
    return rc;
}

/* ---- Kind-named shims (backward compatibility) ----
 *
 * Existing Lua bindings (l_gpu_run_mnist, l_gpu_set_mnist_input)
 * and Rust FFI callers (kernel_ffi::gpu_run_mnist) still call the
 * MNIST-named entry points. These thin wrappers preserve those
 * symbols. New model integrations should use the generic
 * slm_gpu_run / slm_gpu_set_input / slm_gpu_run_with_input
 * functions directly with the appropriate kind constant.
 *
 * Output-size constants below are derived from the model:
 *   MNIST:     10 fp32 logits = 40 bytes
 *   SCHED_MLP: 42 fp32 logits = 168 bytes (AI_SCHED_N_ACTIONS=42) */

int slm_gpu_run_mnist(void *logits_bytes_out)
{
    return slm_gpu_run(GA10B_PIPELINE_KIND_MNIST, logits_bytes_out, 40u);
}

int slm_gpu_set_mnist_input(const void *bytes, size_t cap)
{
    return slm_gpu_set_input(GA10B_PIPELINE_KIND_MNIST, bytes, cap);
}

int slm_gpu_set_mnist_input_fill(uint32_t value_bits, uint32_t n_floats)
{
    return slm_gpu_set_input_fill(GA10B_PIPELINE_KIND_MNIST,
                                   value_bits, n_floats);
}

int slm_gpu_run_sched_inference(const void *state_bytes,
                                 size_t state_bytes_len,
                                 void *logits_bytes_out)
{
    /* #651 admission control. Two layers between the per-CPU
     * `assign_cpu` hot path and the IRQ-off dispatch:
     *
     *   1. Rate limiter — peek at the system-wide last-dispatch
     *      timestamp; if we're inside the RATE_LIMIT_NS window from
     *      the previous successful dispatch, return immediately.
     *   2. Try-lock — even within the window, if another CPU is
     *      currently dispatching, return immediately rather than
     *      spin IRQ-off behind it.
     *
     * Either rejection path returns -1; `forward_via_device` already
     * silently falls back to CPU NEON for negative rc. */
    uint64_t now = slm_get_time_ns();
    uint64_t last = atomic_load_explicit(&g_sched_dispatch_last_ns,
                                          memory_order_relaxed);
    if (sched_dispatch_within_rate_limit(now, last)) {
        return -1;
    }

    int rc = slm_gpu_try_run_with_input(GA10B_PIPELINE_KIND_SCHED_MLP,
                                         state_bytes, state_bytes_len,
                                         logits_bytes_out, 168u);
    if (rc == 0) {
        /* Update timestamp only on a successful dispatch. Failed
         * dispatches (-1 from try-lock contention or downstream rc)
         * leave the window open so the next caller can attempt a
         * fresh dispatch instead of waiting out the rate limit on
         * stale state. Race-tolerant: two CPUs may both dispatch in
         * the same window because the peek above is non-atomic, but
         * the try-lock serialises them so only one actually runs the
         * GPU; the other gets -1 from try-lock and never reaches
         * here. The store is `relaxed` because it publishes nothing
         * other than itself (the timestamp is a scalar, no other
         * state synchronises through it) and the only reader is the
         * `relaxed` load above. */
        atomic_store_explicit(&g_sched_dispatch_last_ns,
                              slm_get_time_ns(),
                              memory_order_relaxed);
    }
    return rc;
}
#else
int slm_gpu_run(uint32_t kind, void *output, size_t output_cap)
{
    (void)kind; (void)output; (void)output_cap;
    return -1;
}
int slm_gpu_set_input(uint32_t kind, const void *bytes, size_t cap)
{
    (void)kind; (void)bytes; (void)cap;
    return -1;
}
int slm_gpu_set_input_fill(uint32_t kind, uint32_t value_bits,
                           uint32_t n_floats)
{
    (void)kind; (void)value_bits; (void)n_floats;
    return -1;
}
int slm_gpu_run_with_input(uint32_t kind,
                           const void *input, size_t input_cap,
                           void *output, size_t output_cap)
{
    (void)kind; (void)input; (void)input_cap;
    (void)output; (void)output_cap;
    return -1;
}
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
void slm_gpu_dispatch_breaker_test_record(int rc) { (void)rc; }
int  slm_gpu_dispatch_breaker_test_count(void) { return 0; }
#endif

/*
 * SLM GPU handoff descriptor lookup (M6.A-1 scaffolding).
 *
 * Marked `weak` so the future M6.A-2 pre-kexec loader integration
 * can override it with a strong definition that returns the actual
 * physical address staged at kexec time. Until that lands, every
 * platform returns 0 and the Rust runtime
 * (`runtime/src/inference/gpu_slm.rs::Handoff::try_from_kernel`)
 * falls back to CPU.
 *
 * Pattern matches the `__attribute__((weak))` use elsewhere in the
 * tree (see `kernel/src/shell_sys.c:hailo_control_signal_driver_shutdown`,
 * `kernel/src/camera.c:mock_camera_frame_*`).
 */
__attribute__((weak)) uint64_t slm_gpu_get_handoff_phys(void)
{
    return 0;
}

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

/*
 * SLM-runtime → operator-library dispatch (#714 §B.3)
 *
 * The Rust-side OperatorLibraryBackend in
 * runtime/src/inference/gpu_slm.rs calls these per-op shims to fire
 * a SASS dispatch through the inherited GPU channel. Each shim:
 *
 *   1. Validates the helper-staged SASS pool is present (handoff
 *      has shader_phys/shader_gpu_va; size >= OPLIB_POOL_MIN_BYTES).
 *   2. Validates per-op size constraints fit in a 64 KB scratch slot.
 *   3. Copies CPU-side input data into the matching GPU scratch slot
 *      (helper-mapped, so the GPU can read it via its GMMU); issues
 *      cache_clean_range so the CPU's writes drain to PoC.
 *   4. Calls slm_oplib_dispatch() with the staged scratch GPU VAs.
 *   5. cache_invalidate_range on the output slot, then memcpy GPU
 *      output back to the CPU-side caller buffer.
 *
 * Returns 0 on success, -1 on any failure (no handoff, size cap,
 * dispatch error). On failure the caller treats it as
 * BackendError::NotAvailable and falls back to the CPU NEON path.
 *
 * Per-call overhead (1536-wide RMSNORM): ~5-10 µs cache + memcpy
 * round-trip plus ~30-50 µs GPU dispatch latency. Net loss vs CPU
 * NEON for cheap ops; the framework wins for matmul-shaped ops once
 * those are wired (Q4K_DOT, GQA_ATTN).
 */

#ifdef PLATFORM_JETSON_ORIN_NANO

/* Per-op dispatch counters — #764 acceptance + ongoing diagnostic.
 *
 * Each `slm_runtime_dispatch_<op>_simt` increments `g_dispatch_attempts`
 * on entry and `g_dispatch_ok` only on the success path. The
 * difference is the per-op silent-fallback rate, which the shell's
 * `slm gpu` verb surfaces alongside the tier table. Without these,
 * "is the GPU dispatch path actually running?" was answerable only by
 * sprinkling uart_printf instrumentation through the FFI on every
 * investigation.
 *
 * Counters are unsigned 64-bit so a long-running session can't wrap
 * — at 1k dispatches/s a u32 wraps in ~50 days, u64 in ~500M years.
 * Relaxed ordering: counters are observed by a single shell reader
 * after the inference completes; we don't need synchronization with
 * the dispatch hot path. */
static _Atomic uint64_t g_op_dispatch_attempts[8];
static _Atomic uint64_t g_op_dispatch_ok[8];

static inline void op_dispatch_record_attempt(uint32_t op_kind)
{
    if (op_kind < 8u) {
        atomic_fetch_add_explicit(&g_op_dispatch_attempts[op_kind], 1u,
                                  memory_order_relaxed);
    }
}

static inline void op_dispatch_record_ok(uint32_t op_kind)
{
    if (op_kind < 8u) {
        atomic_fetch_add_explicit(&g_op_dispatch_ok[op_kind], 1u,
                                  memory_order_relaxed);
    }
}

void slm_runtime_dispatch_stats(uint32_t op_kind,
                                 uint64_t *out_attempts,
                                 uint64_t *out_ok)
{
    if (out_attempts) {
        *out_attempts = (op_kind < 8u)
            ? atomic_load_explicit(&g_op_dispatch_attempts[op_kind],
                                    memory_order_relaxed)
            : 0u;
    }
    if (out_ok) {
        *out_ok = (op_kind < 8u)
            ? atomic_load_explicit(&g_op_dispatch_ok[op_kind],
                                    memory_order_relaxed)
            : 0u;
    }
}

void slm_runtime_dispatch_stats_reset(void)
{
    for (uint32_t i = 0; i < 8u; i++) {
        atomic_store_explicit(&g_op_dispatch_attempts[i], 0u,
                              memory_order_relaxed);
        atomic_store_explicit(&g_op_dispatch_ok[i], 0u,
                              memory_order_relaxed);
    }
}

int slm_runtime_dispatch_rmsnorm_simt(const void *x_cpu_in,
                                       const void *gamma_cpu_in,
                                       void *out_cpu_out,
                                       uint32_t n_rows,
                                       uint32_t n,
                                       uint32_t eps_bits)
{
    if (x_cpu_in == NULL || gamma_cpu_in == NULL || out_cpu_out == NULL) {
        return -1;
    }
    if (n_rows == 0 || n == 0) {
        return -1;
    }
    op_dispatch_record_attempt(SLM_GPU_OP_RMSNORM);

    /* Sizes: x is [n_rows × n] FP16, gamma is [n] FP16, out is
     * [n_rows × n] FP16. Cap each at the 64 KB slot ceiling.
     * Validated outside the lock so the caller takes the early-exit
     * path on shape errors without paying the IRQ-off cost. */
    uint64_t x_bytes     = (uint64_t)n_rows * n * 2u;
    uint64_t gamma_bytes = (uint64_t)n * 2u;
    uint64_t out_bytes   = x_bytes;
    if (x_bytes > OPLIB_POOL_SLOT_BYTES ||
        gamma_bytes > OPLIB_POOL_SLOT_BYTES ||
        out_bytes > OPLIB_POOL_SLOT_BYTES) {
        return -1;
    }

    /* Serialize against the older `slm_gpu_*` FFI (which uses the
     * same lock) and against any other concurrent caller of this
     * shim — e.g. slm-runner's task pulling /slm/prompt messages
     * while the shell user dispatches a smoke verb on a different
     * task. Both ultimately mutate the inherited GPU channel
     * (pushbuffer / semaphore / QMD pool) through `g_nvgpu_b` +
     * `g_handoff`; without the lock the channel-state mutations
     * race. The lock is cleared on every return path. */
    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);

    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    if (h == NULL || h->shader_gpu_va == 0 || h->shader_phys == 0 ||
        h->shader_size < OPLIB_POOL_MIN_BYTES) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return -1;
    }
    struct ga10b_bringup *b = ga10b_bringup_state();
    if (b == NULL) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return -1;
    }

    /* Scratch addresses: same slot layout as the smoke verb. */
    uint64_t in_phys  = h->shader_phys + OPLIB_POOL_OFF_SCRATCH0;
    uint64_t in_va    = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH0;
    uint64_t gam_phys = h->shader_phys + OPLIB_POOL_OFF_SCRATCH1;
    uint64_t gam_va   = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH1;
    uint64_t out_phys = h->shader_phys + OPLIB_POOL_OFF_SCRATCH2;
    uint64_t out_va   = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH2;

    /* Stage CPU input + gamma into GPU scratch. Jetson's 1:1
     * phys/virt mapping for DRAM lets the kernel-side phys pointer
     * be dereferenced as a regular C pointer. */
    memcpy((void *)(uintptr_t)in_phys,  x_cpu_in,     (size_t)x_bytes);
    memcpy((void *)(uintptr_t)gam_phys, gamma_cpu_in, (size_t)gamma_bytes);
    cache_clean_range((void *)(uintptr_t)in_phys,
                      (size_t)((x_bytes + 4095u) & ~4095ull));
    cache_clean_range((void *)(uintptr_t)gam_phys,
                      (size_t)((gamma_bytes + 4095u) & ~4095ull));

    /* Build dispatcher args + fire. */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_RMSNORM,
        .u.rmsnorm = {
            .x_gpu_va     = in_va,
            .gamma_gpu_va = gam_va,
            .out_gpu_va   = out_va,
            .n_rows       = n_rows,
            .n            = n,
            .eps_bits     = eps_bits,
        },
    };
    int rc = slm_oplib_dispatch(b, 0,
                                 SLM_GPU_OP_RMSNORM,
                                 SLM_GPU_TIER_SIMT,
                                 SLM_GPU_DTYPE_FP16,
                                 &args);
    if (rc < 0) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return rc;
    }

    /* Pull GPU output back into the CPU caller's buffer. */
    cache_invalidate_range((void *)(uintptr_t)out_phys,
                           (size_t)((out_bytes + 4095u) & ~4095ull));
    memcpy(out_cpu_out, (void *)(uintptr_t)out_phys, (size_t)out_bytes);

    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    op_dispatch_record_ok(SLM_GPU_OP_RMSNORM);
    return 0;
}

/* W7: dispatch SWIGLU on (gate, up) → out element-wise. Per-call
 * staging mirrors RMSNORM/Q4K_DOT: gate → SCRATCH0, up → SCRATCH1,
 * out → SCRATCH2 then memcpy back. */
int slm_runtime_dispatch_swiglu_simt(const void *gate_cpu_in,
                                      const void *up_cpu_in,
                                      void *out_cpu_out,
                                      uint32_t n)
{
    if (gate_cpu_in == NULL || up_cpu_in == NULL || out_cpu_out == NULL ||
        n == 0) {
        return -1;
    }
    op_dispatch_record_attempt(SLM_GPU_OP_SWIGLU);
    uint64_t bytes = (uint64_t)n * 2u;
    if (bytes > OPLIB_POOL_SLOT_BYTES) {
        return -1;
    }

    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);

    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    if (h == NULL || h->shader_gpu_va == 0 || h->shader_phys == 0 ||
        h->shader_size < OPLIB_POOL_MIN_BYTES) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return -1;
    }
    struct ga10b_bringup *b = ga10b_bringup_state();
    if (b == NULL) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return -1;
    }

    uint64_t gate_phys = h->shader_phys + OPLIB_POOL_OFF_SCRATCH0;
    uint64_t gate_va   = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH0;
    uint64_t up_phys   = h->shader_phys + OPLIB_POOL_OFF_SCRATCH1;
    uint64_t up_va     = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH1;
    uint64_t out_phys  = h->shader_phys + OPLIB_POOL_OFF_SCRATCH2;
    uint64_t out_va    = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH2;

    memcpy((void *)(uintptr_t)gate_phys, gate_cpu_in, (size_t)bytes);
    memcpy((void *)(uintptr_t)up_phys,   up_cpu_in,   (size_t)bytes);
    cache_clean_range((void *)(uintptr_t)gate_phys,
                      (size_t)((bytes + 4095u) & ~4095ull));
    cache_clean_range((void *)(uintptr_t)up_phys,
                      (size_t)((bytes + 4095u) & ~4095ull));

    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_SWIGLU,
        .u.swiglu = {
            .gate_gpu_va = gate_va,
            .up_gpu_va   = up_va,
            .out_gpu_va  = out_va,
            .n           = n,
        },
    };
    int rc = slm_oplib_dispatch(b, 0,
                                 SLM_GPU_OP_SWIGLU,
                                 SLM_GPU_TIER_SIMT,
                                 SLM_GPU_DTYPE_FP16,
                                 &args);
    if (rc < 0) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return rc;
    }

    cache_invalidate_range((void *)(uintptr_t)out_phys,
                           (size_t)((bytes + 4095u) & ~4095ull));
    memcpy(out_cpu_out, (void *)(uintptr_t)out_phys, (size_t)bytes);

    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    op_dispatch_record_ok(SLM_GPU_OP_SWIGLU);
    return 0;
}

/* W6: dispatch GQA_ATTN with per-call staging of Q/K/V into
 * SCRATCH0/1/2; out shares SCRATCH0 at a sub-page offset (matches
 * the existing GQA smoke verb's slot layout). seq_len is capped
 * by the K/V scratch-slot footprint. */
int slm_runtime_dispatch_gqa_attn_simt(const void *q_cpu_in,
                                        const void *k_cpu_in,
                                        const void *v_cpu_in,
                                        void *out_cpu_out,
                                        uint32_t n_head_q,
                                        uint32_t n_head_kv,
                                        uint32_t head_dim,
                                        uint32_t seq_len)
{
    if (q_cpu_in == NULL || k_cpu_in == NULL || v_cpu_in == NULL ||
        out_cpu_out == NULL ||
        n_head_q == 0 || n_head_kv == 0 || head_dim == 0 || seq_len == 0) {
        return -1;
    }
    if ((n_head_q % n_head_kv) != 0) {
        return -1;
    }
    op_dispatch_record_attempt(SLM_GPU_OP_GQA_ATTN);
    uint64_t q_bytes   = (uint64_t)n_head_q  * head_dim * 2u;
    uint64_t kv_bytes  = (uint64_t)seq_len   * n_head_kv * head_dim * 2u;
    uint64_t out_bytes = q_bytes;
    /* Q + out share SCRATCH0; the sub-page offset for `out` lands
     * at SCRATCH0 + 0x1000, so Q must fit in the first 0x1000
     * bytes of the slot. n_head_q*head_dim*2 ≤ 0x1000 means
     * n_head_q*head_dim ≤ 2048 — Qwen2.5-1.5B's 16 q-heads × 128
     * head_dim = 2048 fits exactly; SmolLM2's 9 × 64 = 576 fits
     * comfortably. */
    if (q_bytes > 0x1000ull || kv_bytes > OPLIB_POOL_SLOT_BYTES ||
        out_bytes > 0x1000ull) {
        return -1;
    }

    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);

    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    if (h == NULL || h->shader_gpu_va == 0 || h->shader_phys == 0 ||
        h->shader_size < OPLIB_POOL_MIN_BYTES) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return -1;
    }
    struct ga10b_bringup *b = ga10b_bringup_state();
    if (b == NULL) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return -1;
    }

    uint64_t q_phys   = h->shader_phys + OPLIB_POOL_OFF_SCRATCH0;
    uint64_t q_va     = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH0;
    uint64_t k_phys   = h->shader_phys + OPLIB_POOL_OFF_SCRATCH1;
    uint64_t k_va     = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH1;
    uint64_t v_phys   = h->shader_phys + OPLIB_POOL_OFF_SCRATCH2;
    uint64_t v_va     = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH2;
    /* out lives in SCRATCH0's second 4 KB sub-page. */
    uint64_t out_phys = h->shader_phys + OPLIB_POOL_OFF_SCRATCH0 + 0x1000ull;
    uint64_t out_va   = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH0 + 0x1000ull;

    memcpy((void *)(uintptr_t)q_phys, q_cpu_in, (size_t)q_bytes);
    memcpy((void *)(uintptr_t)k_phys, k_cpu_in, (size_t)kv_bytes);
    memcpy((void *)(uintptr_t)v_phys, v_cpu_in, (size_t)kv_bytes);
    cache_clean_range((void *)(uintptr_t)q_phys, 4096u);
    cache_clean_range((void *)(uintptr_t)k_phys,
                      (size_t)((kv_bytes + 4095u) & ~4095ull));
    cache_clean_range((void *)(uintptr_t)v_phys,
                      (size_t)((kv_bytes + 4095u) & ~4095ull));

    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_GQA_ATTN,
        .u.gqa_attn = {
            .q_gpu_va   = q_va,
            .k_gpu_va   = k_va,
            .v_gpu_va   = v_va,
            .out_gpu_va = out_va,
            .n_head_q   = n_head_q,
            .n_head_kv  = n_head_kv,
            .head_dim   = head_dim,
            .seq_len    = seq_len,
        },
    };
    int rc = slm_oplib_dispatch(b, 0,
                                 SLM_GPU_OP_GQA_ATTN,
                                 SLM_GPU_TIER_SIMT,
                                 SLM_GPU_DTYPE_FP16,
                                 &args);
    if (rc < 0) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return rc;
    }

    cache_invalidate_range((void *)(uintptr_t)out_phys, 4096u);
    memcpy(out_cpu_out, (void *)(uintptr_t)out_phys, (size_t)out_bytes);

    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    op_dispatch_record_ok(SLM_GPU_OP_GQA_ATTN);
    return 0;
}

/* W5: dispatch Q4K_DOT against a GPU-resident weight matrix.
 * Per-call activation staging: x → SCRATCH0, dispatch with
 * (scratch_x_va, weights_gpu_va, scratch_out_va), pull FP32 out.
 * Uses two scratch slots (SCRATCH0 for x_in, SCRATCH1 for out). */
int slm_runtime_dispatch_q4k_dot_simt(const void *x_cpu_in,
                                       uint64_t weights_gpu_va,
                                       uint64_t weights_size_bytes,
                                       void *out_cpu_out,
                                       uint32_t k,
                                       uint32_t n)
{
    if (x_cpu_in == NULL || out_cpu_out == NULL || weights_gpu_va == 0 ||
        k == 0 || n == 0 || (k & 255u) != 0) {
        return -1;
    }
    op_dispatch_record_attempt(SLM_GPU_OP_Q4K_DOT);
    uint64_t x_bytes   = (uint64_t)k * 2u;        /* FP16 */
    uint64_t out_bytes = (uint64_t)n * 4u;        /* FP32 */
    if (x_bytes > OPLIB_POOL_SLOT_BYTES || out_bytes > OPLIB_POOL_SLOT_BYTES) {
        return -1;
    }
    /* Sanity: weights_size_bytes is informational; require it covers
     * at least one Q4_K row (144 bytes) when supplied. */
    if (weights_size_bytes != 0 && weights_size_bytes < 144u) {
        return -1;
    }

    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);

    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    if (h == NULL || h->shader_gpu_va == 0 || h->shader_phys == 0 ||
        h->shader_size < OPLIB_POOL_MIN_BYTES) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return -1;
    }
    struct ga10b_bringup *b = ga10b_bringup_state();
    if (b == NULL) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return -1;
    }

    uint64_t x_phys   = h->shader_phys + OPLIB_POOL_OFF_SCRATCH0;
    uint64_t x_va     = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH0;
    uint64_t out_phys = h->shader_phys + OPLIB_POOL_OFF_SCRATCH1;
    uint64_t out_va   = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH1;

    memcpy((void *)(uintptr_t)x_phys, x_cpu_in, (size_t)x_bytes);
    cache_clean_range((void *)(uintptr_t)x_phys,
                      (size_t)((x_bytes + 4095u) & ~4095ull));

    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_Q4K_DOT,
        .u.q4k_dot = {
            .x_gpu_va       = x_va,
            .weights_gpu_va = weights_gpu_va,
            .out_gpu_va     = out_va,
            .k              = k,
            .n              = n,
        },
    };
    int rc = slm_oplib_dispatch(b, 0,
                                 SLM_GPU_OP_Q4K_DOT,
                                 SLM_GPU_TIER_SIMT,
                                 SLM_GPU_DTYPE_FP16,
                                 &args);
    if (rc < 0) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return rc;
    }

    cache_invalidate_range((void *)(uintptr_t)out_phys,
                           (size_t)((out_bytes + 4095u) & ~4095ull));
    memcpy(out_cpu_out, (void *)(uintptr_t)out_phys, (size_t)out_bytes);

    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    op_dispatch_record_ok(SLM_GPU_OP_Q4K_DOT);
    return 0;
}

/* W4: dispatch EMBEDDING against a GPU-resident table. The table's
 * gpu_va comes from `gpu_tensor_map`; output is written to a
 * scratch slot, then memcpy'd back to the caller's CPU buffer. */
int slm_runtime_dispatch_embedding_simt(uint64_t table_gpu_va,
                                         uint64_t table_size_bytes,
                                         uint32_t token_id,
                                         void *out_cpu_out,
                                         uint32_t embedding_length,
                                         uint32_t table_row_bytes)
{
    if (out_cpu_out == NULL || table_gpu_va == 0 ||
        embedding_length == 0 || table_row_bytes == 0) {
        return -1;
    }
    op_dispatch_record_attempt(SLM_GPU_OP_EMBEDDING);
    /* table_size_bytes is informational; pin a sanity range. */
    if (table_size_bytes != 0 &&
        table_size_bytes < (uint64_t)table_row_bytes) {
        return -1;
    }
    uint64_t out_bytes = (uint64_t)embedding_length * 2u;
    if (out_bytes > OPLIB_POOL_SLOT_BYTES) {
        return -1;
    }

    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);

    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    if (h == NULL || h->shader_gpu_va == 0 || h->shader_phys == 0 ||
        h->shader_size < OPLIB_POOL_MIN_BYTES) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return -1;
    }
    struct ga10b_bringup *b = ga10b_bringup_state();
    if (b == NULL) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return -1;
    }

    /* Output slot in the existing scratch carve-out. SCRATCH0 has
     * worked for every smoke verb; reuse it. */
    uint64_t out_phys = h->shader_phys + OPLIB_POOL_OFF_SCRATCH0;
    uint64_t out_va   = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH0;

    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_EMBEDDING,
        .u.embedding = {
            .table_gpu_va     = table_gpu_va,
            .out_gpu_va       = out_va,
            .token_id         = token_id,
            .embedding_length = embedding_length,
            .table_row_bytes  = table_row_bytes,
        },
    };
    int rc = slm_oplib_dispatch(b, 0,
                                 SLM_GPU_OP_EMBEDDING,
                                 SLM_GPU_TIER_SIMT,
                                 SLM_GPU_DTYPE_FP16,
                                 &args);
    if (rc < 0) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return rc;
    }

    cache_invalidate_range((void *)(uintptr_t)out_phys,
                           (size_t)((out_bytes + 4095u) & ~4095ull));
    memcpy(out_cpu_out, (void *)(uintptr_t)out_phys, (size_t)out_bytes);

    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    op_dispatch_record_ok(SLM_GPU_OP_EMBEDDING);
    return 0;
}

/* W2: stage CPU weight bytes into the helper-published GPU weights
 * pool. See oplib_weights_pool.h for the per-page GMMU-walk
 * staging mechanism. The lock guards the (alloc, stage) pair so a
 * concurrent dispatch can't run mid-stage and observe a half-
 * populated tensor — both `oplib_weights_pool_alloc` (mutates the
 * bump pointer) and `oplib_weights_pool_stage` (touches the
 * channel's GMMU walker state) need protection. */
int slm_runtime_stage_weight(const void *cpu_bytes,
                              uint64_t len,
                              uint64_t *out_gpu_va)
{
    if (cpu_bytes == NULL || len == 0 || out_gpu_va == NULL) {
        return -1;
    }

    irq_flags_t irq = spin_lock_irqsave(&g_gpu_dispatch_lock);

    uint64_t gva = oplib_weights_pool_alloc((size_t)len, 0);
    if (gva == 0) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return -1;
    }
    int rc = oplib_weights_pool_stage(gva, cpu_bytes, (size_t)len);
    if (rc != 0) {
        spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
        return rc;
    }
    *out_gpu_va = gva;
    spin_unlock_irqrestore(&g_gpu_dispatch_lock, irq);
    return 0;
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

int slm_runtime_dispatch_rmsnorm_simt(const void *x_cpu_in,
                                       const void *gamma_cpu_in,
                                       void *out_cpu_out,
                                       uint32_t n_rows,
                                       uint32_t n,
                                       uint32_t eps_bits)
{
    (void)x_cpu_in;
    (void)gamma_cpu_in;
    (void)out_cpu_out;
    (void)n_rows;
    (void)n;
    (void)eps_bits;
    /* Non-Jetson: no GA10B GMMU, no operator library staging. */
    return -1;
}

int slm_runtime_stage_weight(const void *cpu_bytes,
                              uint64_t len,
                              uint64_t *out_gpu_va)
{
    (void)cpu_bytes;
    (void)len;
    (void)out_gpu_va;
    return -1;
}

int slm_runtime_dispatch_embedding_simt(uint64_t table_gpu_va,
                                         uint64_t table_size_bytes,
                                         uint32_t token_id,
                                         void *out_cpu_out,
                                         uint32_t embedding_length,
                                         uint32_t table_row_bytes)
{
    (void)table_gpu_va; (void)table_size_bytes;
    (void)token_id; (void)out_cpu_out;
    (void)embedding_length; (void)table_row_bytes;
    return -1;
}

int slm_runtime_dispatch_q4k_dot_simt(const void *x_cpu_in,
                                       uint64_t weights_gpu_va,
                                       uint64_t weights_size_bytes,
                                       void *out_cpu_out,
                                       uint32_t k,
                                       uint32_t n)
{
    (void)x_cpu_in; (void)weights_gpu_va; (void)weights_size_bytes;
    (void)out_cpu_out; (void)k; (void)n;
    return -1;
}

int slm_runtime_dispatch_gqa_attn_simt(const void *q_cpu_in,
                                        const void *k_cpu_in,
                                        const void *v_cpu_in,
                                        void *out_cpu_out,
                                        uint32_t n_head_q,
                                        uint32_t n_head_kv,
                                        uint32_t head_dim,
                                        uint32_t seq_len)
{
    (void)q_cpu_in; (void)k_cpu_in; (void)v_cpu_in; (void)out_cpu_out;
    (void)n_head_q; (void)n_head_kv; (void)head_dim; (void)seq_len;
    return -1;
}

int slm_runtime_dispatch_swiglu_simt(const void *gate_cpu_in,
                                      const void *up_cpu_in,
                                      void *out_cpu_out,
                                      uint32_t n)
{
    (void)gate_cpu_in; (void)up_cpu_in; (void)out_cpu_out; (void)n;
    return -1;
}

void slm_runtime_dispatch_stats(uint32_t op_kind,
                                 uint64_t *out_attempts,
                                 uint64_t *out_ok)
{
    (void)op_kind;
    if (out_attempts) { *out_attempts = 0; }
    if (out_ok) { *out_ok = 0; }
}

void slm_runtime_dispatch_stats_reset(void) { }

#endif /* PLATFORM_JETSON_ORIN_NANO */
