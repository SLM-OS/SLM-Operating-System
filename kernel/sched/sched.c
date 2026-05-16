/*
 * sched.c - Per-core priority scheduler for SLM-OS
 *
 * Each CPU has its own run queue ordered by effective priority (highest first).
 * Within the same priority, tasks are scheduled FIFO.
 *
 * Each CPU has its own run queue lock, reducing contention for multi-core
 * scheduling. Global statistics use atomic operations or accept benign races.
 */

#include "sched.h"
#include "sched_policy.h"
#include "sched_trace.h"
#include "task.h"
#include "uart.h"
#include "debug.h"
#include "platform.h"
#include "smp.h"
#if !defined(PLATFORM_X86_64)
#include "pmu.h"
#endif
#include "spinlock.h"
#include "slm_ffi.h"
#include "gic.h"
#include "timer.h"
#include "cache.h"
#include "ncmem.h"
#include "preempt.h"
#if !defined(PLATFORM_X86_64)
#include "vmm.h"
#endif
#include "string.h"
#include "runtime_model.h"
#if CONFIG_WORK_STEALING
#include "steal_deque.h"
#endif
#include <stdint.h>

/* Deadline boost thresholds (in nanoseconds) */
#define DEADLINE_CRITICAL_NS    (10 * 1000000ULL)   /* 10ms - boost to CRITICAL */
#define DEADLINE_HIGH_NS        (50 * 1000000ULL)   /* 50ms - boost to HIGH */
#define DEADLINE_BOOST_NS       (100 * 1000000ULL)  /* 100ms - boost +1 */

#define SCHED_BALANCE_DEFAULT_ENABLED          1u
#define SCHED_BALANCE_DEFAULT_MIN_TARGET_READY 2u
#define SCHED_BALANCE_DEFAULT_MIN_ACTIVE_CPUS  2u
#define SCHED_BALANCE_DEFAULT_IMBALANCE_NUM    3u
#define SCHED_BALANCE_DEFAULT_IMBALANCE_DEN    2u

static void sched_set_default_deadline_thresholds(
    struct sched_runtime_deadline_thresholds *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->feature_version = SCHED_MODEL_FEATURE_VERSION_V1;
    cfg->action_version = SCHED_MODEL_ACTION_VERSION_V1;
    cfg->critical_ns = DEADLINE_CRITICAL_NS;
    cfg->high_ns = DEADLINE_HIGH_NS;
    cfg->boost_ns = DEADLINE_BOOST_NS;
}

/* External functions from task.c */
extern void task_set_current(struct task *task);
extern void task_destroy(struct task *task);

/*
 * Per-CPU preemption disable flag.
 *
 * Set during the critical window in schedule() between releasing the
 * run queue lock and completing the context switch. scheduler_tick()
 * checks this flag and skips calling schedule() if set.
 *
 * Without this, a timer IRQ firing in the window between rq_unlock
 * and switch_to causes a reentrant schedule() that corrupts state:
 * - x86-64: task_current() returns the wrong task (set before switch)
 * - ARM64 (Pi 5): deadlock during UART output after DAIF unmask fix
 *
 * This is the cross-platform equivalent of Linux's preempt_count.
 */
volatile int preempt_disabled[MAX_CPUS];

/* `SPINLOCK_ARRAY` in spinlock.h hardcodes a 64-byte alignment to
 * avoid pulling cache.h into every TU that uses spinlocks. This TU
 * sees both headers, so pin the assumption with a static_assert: if
 * `CACHE_LINE_SIZE` ever changes (e.g. a new ARM CPU with 128-byte
 * lines), the build fails here instead of letting the macro silently
 * under-align. */
_Static_assert(CACHE_LINE_SIZE == 64,
               "SPINLOCK_ARRAY in spinlock.h hardcodes 64-byte alignment");

/* Per-CPU run queue locks — always in cacheable memory.
 * Separated from cpu_runqueue because exclusive load/store (ldaxr/stxr)
 * used by spinlocks may not work on Non-Cacheable memory (BCM2712). */
static SPINLOCK_ARRAY(rq_lock, MAX_CPUS);

#if CONFIG_WORK_STEALING
/*
 * Per-CPU stealable task deque (#59 Phase B, S1 NC placement). One
 * entry per CPU. Tasks are pushed here (in addition to the linked-list
 * run queue) when scheduler_add_task_to_cpu is called with a task whose
 * cpu_affinity is CPU_AFFINITY_ANY. Idle CPUs drain these in
 * sched_try_steal() before going to WFE.
 *
 * The deque may contain stale pointers (tasks that have since run or
 * been terminated); sched_try_steal() validates under victim's rq_lock
 * and the per-slot generation counter (#139) before accepting a steal.
 *
 * Placement:
 *   - PLATFORM_HAS_NC_MEMORY (Pi 5, Jetson) — allocated from NC memory
 *     at scheduler_init(). Cross-CPU steals see writes instantly, no
 *     DC CIVAC/CVAC needed.
 *   - Other (QEMU ARM64, x86-64) — cacheable BSS array. Caches are
 *     coherent; no NC backing needed.
 *
 * Locking: see `steal_deque_lock[MAX_CPUS]` below. The deque's own
 * `steal_deque_*` functions are lockless and the callers in sched.c
 * are responsible for serializing access. The steal_deque_t used to
 * embed its own spinlock, but on Jetson `SPINLOCK_SKIP_LOCKING` made
 * that lock a no-op (exclusive monitors don't work on NC memory /
 * post-kexec A78AE), so concurrent push/pop/steal could corrupt the
 * deque — observed as Instruction Abort at ELR=0x0 during `bench
 * stealing` on jetson-nano-2. The external-lock pattern matches
 * `rq_lock[]`: the lock lives in cacheable memory and uses real
 * exclusive monitors on every platform that supports them.
 *
 * The two configurations share the same access pattern through
 * `cpu_steal_deques[cpu]`; the definition below selects storage.
 */
#if defined(PLATFORM_HAS_NC_MEMORY)
/* cpu_steal_deques is a pointer into NC memory, assigned in
 * scheduler_init() from ncmem_alloc. Indexed via cpu_steal_deques[cpu]
 * at every call site, same spelling as the BSS array below. */
static steal_deque_t *cpu_steal_deques;
#else
static steal_deque_t cpu_steal_deques[MAX_CPUS];
#endif

/* Per-CPU steal-deque locks. Cacheable so exclusive-monitor
 * primitives work on every platform, unlike the embedded lock in
 * steal_deque_t which was neutered by SPINLOCK_SKIP_LOCKING on
 * Jetson. Acquire the victim's lock before any steal_deque_*
 * operation; never held across other locks. */
static SPINLOCK_ARRAY(steal_deque_lock, MAX_CPUS);
#endif /* CONFIG_WORK_STEALING */

/* Lock helpers that use the correct lock for a given CPU */
static inline irq_flags_t rq_lock_irqsave(uint32_t cpu)
{
    return spin_lock_irqsave(&rq_lock[cpu]);
}
static inline void rq_unlock_irqrestore(uint32_t cpu, irq_flags_t flags)
{
    spin_unlock_irqrestore(&rq_lock[cpu], flags);
}

/*
 * Per-CPU run queue access.
 *
 * On Pi 5, run queues are in non-cacheable memory at NC_MEM_BASE to bypass
 * the L1/L2 incoherency (SMPEN not set by TF-A). The address is computed
 * from compile-time constants — no cacheable pointer indirection that would
 * itself be invisible to other CPUs.
 *
 * On other platforms (QEMU, Jetson), caches are coherent and run queues
 * live in the normal BSS-resident fallback array.
 */
/* Forward declaration — defined below after sched struct */
static inline struct cpu_runqueue *cpu_rq(uint32_t cpu);

/*
 * Global scheduler state.
 *
 * Lock strategy: Each CPU run queue has its own lock for local operations.
 * Cross-queue operations (migration) lock both queues in CPU ID order
 * to prevent deadlock. Global statistics are racy but acceptable.
 */
static struct {
    struct cpu_runqueue cpu_fallback[MAX_CPUS];
    uint32_t task_count;                 /* Total tasks (racy but OK for stats) */
    uint64_t context_switches;           /* Total switches (racy but OK) */
    uint64_t timer_ticks;                /* Timer interrupts (racy but OK) */
    uint32_t isolated_cores;             /* Bitmask of isolated cores */
    int initialized;                     /* Scheduler initialized flag */
} sched;

/* ============================================================================
 * AI scheduler counters (M5) — guarded by CONFIG_AI_SCHEDULER
 * ============================================================================ */

#ifdef CONFIG_AI_SCHEDULER

/* Deadline miss tracking: rolling window of last 100 completions */
static struct {
    uint32_t miss_count;              /* Misses in current window */
    uint32_t total_count;             /* Total completions */
    uint8_t  window[100];             /* Ring buffer: 0=hit, 1=miss */
    uint32_t window_idx;              /* Next write position */
} ai_deadline_stats;

/* Completion latency tracking */
static struct {
    uint64_t cumulative_ns;           /* Sum of all task latencies */
    uint32_t count;                   /* Number of completions */
} ai_latency_stats;

/* Top-8 task cache: updated periodically in scheduler_tick().
 *
 * Each entry stores the task pointer alongside the captured `generation`
 * value at publish time. `task_destroy` (kernel/sched/task.c) bumps
 * `task->generation` when a slot is freed, so a stale entry whose
 * captured generation no longer matches the slot's current generation is
 * silently filtered out by `sched_ai_get_top_tasks` before any field is
 * dereferenced by AI feature extraction. Without that check the
 * extractor would read fields belonging to a recycled task (or zeros
 * post-destroy), feeding stale features to the AI scheduler. */
#include "ai_types.h"
static struct {
    struct task *tasks[AI_STATE_NUM_TASKS];
    uint32_t generations[AI_STATE_NUM_TASKS];
    uint32_t count;
    uint64_t last_update_tick;
} ai_top_tasks;

#define AI_TOP_TASKS_UPDATE_INTERVAL 10  /* Update every 10 ticks (100ms) */

/*
 * Update the top-8 task cache by scanning all run queues.
 * Called from scheduler_tick() on CPU 0 every N ticks.
 *
 * Locking: acquires each CPU's rq_lock one at a time, walks that queue,
 * then releases before moving on. Never holds more than one rq_lock at a
 * time (avoids starving other CPUs and prevents deadlock). The staging
 * buffer accumulates across CPUs; only the final snapshot is published
 * to ai_top_tasks so FFI consumers never see a half-built list.
 */
static void ai_update_top_tasks(void)
{
    struct task *staging[AI_STATE_NUM_TASKS];
    uint32_t staging_gen[AI_STATE_NUM_TASKS];
    uint32_t count = 0;

    for (uint32_t c = 0; c < cpu_count; c++) {
        irq_flags_t flags = rq_lock_irqsave(c);
        struct cpu_runqueue *rq = cpu_rq(c);
        struct task *t = rq->head;

        while (t) {
            if (count < AI_STATE_NUM_TASKS) {
                staging[count] = t;
                /* Capture generation under rq_lock so it pairs with the
                 * task pointer; task_destroy bumps generation while
                 * holding task_lock + the owning rq_lock, so a captured
                 * (t, gen) pair is consistent with the task's identity at
                 * this instant. */
                staging_gen[count] = t->generation;
                count++;
            } else {
                uint32_t min_idx = 0;
                uint8_t min_pri = staging[0]->effective_priority;
                for (uint32_t i = 1; i < AI_STATE_NUM_TASKS; i++) {
                    if (staging[i]->effective_priority < min_pri) {
                        min_pri = staging[i]->effective_priority;
                        min_idx = i;
                    }
                }
                if (t->effective_priority > min_pri) {
                    staging[min_idx] = t;
                    staging_gen[min_idx] = t->generation;
                }
            }
            t = t->next;
        }

        rq_unlock_irqrestore(c, flags);
    }

    for (uint32_t i = 0; i < count; i++) {
        ai_top_tasks.tasks[i] = staging[i];
        ai_top_tasks.generations[i] = staging_gen[i];
    }
    ai_top_tasks.count = count;
    ai_top_tasks.last_update_tick = sched.timer_ticks;
}

/*
 * Record a task completion for deadline/latency tracking.
 * Called from task_exit path (via sched_ai_record_completion).
 */
void sched_ai_record_completion(struct task *task)
{
    if (!task) return;

    uint64_t now = slm_get_time_ns();
    task->completion_time_ns = now;

    /* Deadline miss tracking */
    if (task->deadline_ns > 0) {
        uint8_t missed = (now > task->deadline_ns) ? 1 : 0;

        /* Update rolling window */
        uint32_t idx = ai_deadline_stats.window_idx % 100;
        /* Subtract old value from running count */
        if (ai_deadline_stats.total_count >= 100) {
            ai_deadline_stats.miss_count -= ai_deadline_stats.window[idx];
        }
        ai_deadline_stats.window[idx] = missed;
        ai_deadline_stats.miss_count += missed;
        ai_deadline_stats.window_idx++;
        ai_deadline_stats.total_count++;
    }

    /* Latency tracking */
    if (task->arrival_time_ns > 0) {
        uint64_t latency = now - task->arrival_time_ns;
        ai_latency_stats.cumulative_ns += latency;
        ai_latency_stats.count++;
    }
}

/*
 * Integer accessors for ai_state.c (state vector extraction).
 * Return raw integer values — float conversion happens in ai_state.c
 * (which is compiled without -mgeneral-regs-only).
 */
void sched_ai_get_utilization_raw(uint32_t cpu, uint64_t *running, uint64_t *total)
{
    struct cpu_runqueue *rq = cpu_rq(cpu);
    *running = rq->running_ticks;
    *total = rq->total_ticks;
}

void sched_ai_get_deadline_miss_raw(uint32_t *misses, uint32_t *window_size)
{
    *misses = ai_deadline_stats.miss_count;
    uint32_t ws = ai_deadline_stats.total_count;
    if (ws > 100) ws = 100;
    *window_size = ws;
}

void sched_ai_get_latency_raw(uint64_t *cumulative_ns, uint32_t *count)
{
    *cumulative_ns = ai_latency_stats.cumulative_ns;
    *count = ai_latency_stats.count;
}

uint32_t sched_ai_get_top_tasks(struct task **out, uint32_t max)
{
    uint32_t n = ai_top_tasks.count;
    if (n > max) n = max;
    uint32_t valid = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct task *t = ai_top_tasks.tasks[i];
        uint32_t captured_gen = ai_top_tasks.generations[i];
        /* Skip entries whose owning task slot has been recycled since the
         * last ai_update_top_tasks() snapshot. task_destroy bumps
         * generation, so a mismatch here means the pointer no longer
         * refers to the task that was on the run queue when we sampled. */
        if (t && __atomic_load_n(&t->generation, __ATOMIC_ACQUIRE) == captured_gen) {
            out[valid++] = t;
        }
    }
    return valid;
}

#endif /* CONFIG_AI_SCHEDULER */

/* Per-CPU diagnostic counters for cross-CPU dispatch debugging.
 * On Pi 5 these must be in NC memory for cross-CPU visibility —
 * secondary CPU writes to cacheable BSS are invisible to CPU 0. */
#if defined(PLATFORM_HAS_NC_MEMORY)
/* Allocated from NC region in scheduler_init for cross-CPU visibility */
volatile uint32_t *sched_diag_tick;
volatile uint32_t *sched_diag_schedule;
volatile uint32_t *sched_diag_picked;
volatile uint32_t *sched_diag_idle_loops;  /* idle task iteration count per CPU */
/* Work-stealing observability counters (#105). Incremented by
 * sched_try_steal on the thief's CPU and read by the `cpu` shell
 * command. NC-backed on PLATFORM_HAS_NC_MEMORY for the same
 * cross-CPU visibility reasons as the other sched_diag_* counters.
 * All four are initialised to zero by scheduler_init. */
volatile uint32_t *sched_diag_steal_attempts;      /* sched_try_steal entries  */
volatile uint32_t *sched_diag_steal_successes;     /* live task returned       */
volatile uint32_t *sched_diag_steal_stale;         /* stale pointer discarded  */
volatile uint32_t *sched_diag_steal_empty_victim;  /* victim had nothing to take */
volatile uint32_t *sched_diag_steal_push_full;     /* push failed — deque full (#175) */
static struct cpu_runqueue *nc_runqueues;
/* Scheduler init flag — uses the SAME pattern as the working cpu_boot_flag
 * handshake: cacheline-aligned, atomic store + cache_invalidate polling
 * with delay for natural L2 eviction. */
static volatile uint32_t sched_init_flag __attribute__((aligned(64)));
#define nc_sched_initialized sched_init_flag
#else
volatile uint32_t sched_diag_tick[MAX_CPUS];
volatile uint32_t sched_diag_schedule[MAX_CPUS];
volatile uint32_t sched_diag_picked[MAX_CPUS];
volatile uint32_t sched_diag_idle_loops[MAX_CPUS];
volatile uint32_t sched_diag_steal_attempts[MAX_CPUS];
volatile uint32_t sched_diag_steal_successes[MAX_CPUS];
volatile uint32_t sched_diag_steal_stale[MAX_CPUS];
volatile uint32_t sched_diag_steal_empty_victim[MAX_CPUS];
volatile uint32_t sched_diag_steal_push_full[MAX_CPUS];
#endif

/* cpu_rq() implementation — must be after sched struct definition */
static inline struct cpu_runqueue *cpu_rq(uint32_t cpu)
{
#if defined(PLATFORM_HAS_NC_MEMORY)
    return &nc_runqueues[cpu];
#else
    return &sched.cpu_fallback[cpu];
#endif
}

/*
 * Public accessor for cpu_rq — used by external policies (sched_heuristic.c,
 * sched_ai.c) that need to inspect run queue state for CPU assignment.
 */
struct cpu_runqueue *sched_cpu_rq(uint32_t cpu)
{
    return cpu_rq(cpu);
}

/*
 * Drain the per-CPU run queue under rq_lock. Used by the dormancy
 * recovery path (#216 Tier 2, kernel/sched/cpu_supervisor.c) when a
 * secondary CPU has been declared dead and is about to be
 * resurrected via psci_cpu_on. The tasks still on the queue are
 * leaked — the fault that killed the CPU may have left their
 * state in indeterminate condition, and migrating them risks
 * propagating the corruption. A future tier will iterate the queue
 * and individually validate each task before deciding to migrate or
 * terminate.
 *
 * Caller is expected to have set cpu_data[cpu].online = false
 * before calling this so steal_deque-based thieves stop racing.
 */
void sched_drain_cpu_runqueue(uint32_t cpu)
{
    if (cpu >= MAX_CPUS) return;
    irq_flags_t flags = rq_lock_irqsave(cpu);
    struct cpu_runqueue *rq = cpu_rq(cpu);
    if (rq) {
        rq->head = NULL;
        rq->tail = NULL;
        rq->ready_count = 0;
        rq->zombie = NULL;
    }
    rq_unlock_irqrestore(cpu, flags);
}

/* ============================================================================
 * Policy registry and active policy
 * ============================================================================ */

static const struct sched_policy_ops *policy_registry[SCHED_POLICY_MAX];
static int policy_registry_count;

static const struct sched_policy_ops *active_policy = &sched_policy_heuristic;

int sched_register_policy(const struct sched_policy_ops *policy)
{
    if (!policy || !policy->name || !policy->assign_cpu) {
        return -1;
    }
    if (policy_registry_count >= SCHED_POLICY_MAX) {
        WARN("Policy registry full, cannot register '%s'", policy->name);
        return -1;
    }
    policy_registry[policy_registry_count++] = policy;
    return 0;
}

int sched_set_policy(const struct sched_policy_ops *policy)
{
    if (!policy || !policy->assign_cpu) {
        return -1;
    }

    /* Hold IRQs disabled for the entire operation — init, swap, shutdown,
     * and log. On Pi 5, enabling IRQs mid-switch allows a timer tick that
     * can context-switch away from the caller. If the idle task then runs
     * with IRQs masked (Pi 5 secondary idle skips daifclr), the caller
     * never resumes. */
    irq_flags_t flags = irq_save();

    /* Init the new policy before swapping (may fail) */
    if (policy->init) {
        int ret = policy->init();
        if (ret < 0) {
            irq_restore(flags);
            WARN("Policy '%s' init failed (%d)", policy->name, ret);
            return -1;
        }
    }

    const struct sched_policy_ops *old = active_policy;
    active_policy = policy;

    /* Shut down old policy after swap */
    if (old && old->shutdown) {
        old->shutdown();
    }

    irq_restore(flags);

    INFO("Scheduler policy: %s -> %s",
         old ? old->name : "(none)", policy->name);
    return 0;
}

const char *sched_get_policy(void)
{
    return active_policy ? active_policy->name : "none";
}

const struct sched_policy_ops *sched_find_policy(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < policy_registry_count; i++) {
        if (strcmp(policy_registry[i]->name, name) == 0) {
            return policy_registry[i];
        }
    }
    return NULL;
}

int sched_policy_count(void)
{
    return policy_registry_count;
}

const struct sched_policy_ops *sched_policy_get(int index)
{
    if (index < 0 || index >= policy_registry_count) {
        return NULL;
    }
    return policy_registry[index];
}

/*
 * Idle task - runs when no other tasks are ready.
 * Each CPU has its own idle task.
 */
static void idle_task_func(void *arg)
{
    (void)arg;

    while (1) {
        /* Idle-loop heartbeat. The sched_diag_idle_loops array is
         * NC-backed on PLATFORM_HAS_NC_MEMORY (see scheduler_init)
         * and plain BSS elsewhere — both paths produce a counter
         * that CPU 0 can read without cache maintenance. Used by
         * `ws-diag` (integration tests) and `cpu` (shell) to
         * distinguish "secondary never reached idle" (counter==0)
         * from "secondary is idling but SEV is not reaching it"
         * (counter frozen after initial iterations).
         *
         * NULL guard applies only on PLATFORM_HAS_NC_MEMORY where
         * the symbol is pointer-backed (scheduler_init allocates
         * via ncmem_alloc, which could in principle return NULL if
         * the 2 MB NC arena is exhausted; scheduler_init panics
         * first in practice). On other platforms the symbol is a
         * BSS array whose address is always non-NULL — GCC's
         * -Werror=address would flag an unconditional `if (arr)`
         * there, so the check is gated. */
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (sched_diag_idle_loops)
#endif
            sched_diag_idle_loops[cpu_id()]++;
#if defined(SCHED_DEBUG_NC_TRACE) && defined(PLATFORM_HAS_NC_MEMORY)
        /* Belt-and-braces trace slot at a fixed NC address so early-
         * boot analysis can confirm the counter is wired up even
         * before scheduler_init finishes. cpu_logical_map[] lookup —
         * Jetson dual-cluster safe (#647). */
        {
            uint64_t mpidr;
            __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
            int hw_cpu = cpu_logical_id(mpidr);
            if (hw_cpu >= 0 && hw_cpu < (int)MAX_CPUS)
                (*(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + hw_cpu * 4))++;
        }
#endif

        /* Unmask IRQ so timer interrupts can fire.
         * This must be inside the loop because context switch saves/restores
         * DAIF. When idle is preempted by the timer ISR, the saved DAIF has
         * IRQ masked (hardware masks IRQ on exception entry). On resume,
         * the restored DAIF keeps IRQ masked — so we must re-clear it here. */
#if defined(PLATFORM_X86_64)
        __asm__ volatile("sti" ::: "memory");
        __asm__ volatile("hlt");
#elif defined(PLATFORM_HAS_NC_MEMORY)
        /* Pi 5/Jetson idle loop.
         *
         * With SECONDARY_PREEMPT: all CPUs unmask IRQs and WFI so
         * timer-driven preemption works on secondary CPUs too.
         * The ELR trampoline handles the switch_to-in-ISR problem.
         *
         * Without SECONDARY_PREEMPT: only CPU 0 takes timer IRQs in
         * idle. Secondary CPUs use WFE and rely on cooperative
         * cross-CPU dispatch (SEV).
         *
         * With PI5_FIQ_TIMER: also clear DAIF.F. On this GICv2 the
         * timer PPI remains in Group 0 and arrives as FIQ; masking F
         * would keep the interrupt line dead. (daifclr #3 clears I+F.) */
#if defined(PI5_FIQ_TIMER)
        __asm__ volatile("msr daifclr, #3" ::: "memory");
        __asm__ volatile("isb" ::: "memory");
        __asm__ volatile("wfi");
#elif defined(SECONDARY_PREEMPT) && \
      (!defined(PLATFORM_JETSON_ORIN_NANO) || defined(JETSON_HW_TICK))
        /* Pi 5 always (#742 landed the BL31 routing fix), or Jetson
         * when JETSON_HW_TICK=ON pairs with the patched BL31 from
         * tools/tfa-patches/0004-*. Both: per-CPU CNTHP fires Group
         * 1 NS IRQs at NS-EL2/VHE; daifclr unmasks I, WFI sleeps
         * until the next tick. The ELR trampoline handles the
         * switch_to-in-ISR problem on secondary CPUs. */
        __asm__ volatile("msr daifclr, #2" ::: "memory");
        __asm__ volatile("isb" ::: "memory");
        __asm__ volatile("wfi");
#elif defined(PLATFORM_JETSON_ORIN_NANO)
        /* Jetson without JETSON_HW_TICK (stock BL31): hardware timer
         * IRQs do not deliver to EL1/EL2 because the GIC group/route
         * is owned by EL3 firmware — see "ARM64 Hardware Timer IRQs"
         * in kernel/CLAUDE.md. The Pi-5-equivalent SECONDARY_PREEMPT
         * branch above would WFI here and never wake — when shell is
         * the only ready task on CPU 0 and it calls task_sleep_ms,
         * the shell blocks, idle takes over, WFI hangs forever
         * waiting for an IRQ that never comes. Reproducer:
         * `lua-admin -e slm.sleep(500)` on the serial console after
         * a fresh kexec. Telnet "fixes" it because lwIP/net_pump
         * activity yields on a secondary CPU, and *that* yield
         * triggers coop_preempt_maybe_tick → scheduler_tick →
         * task_wake_sleepers, which finds CPU 0's sleeper and wakes
         * it.
         *
         * Fix: on CPU 0, skip the wait entirely and fall through to
         * the post-loop yield(), which drives schedule() and fires
         * coop_preempt_maybe_tick. Secondary CPUs still WFE — they're
         * only wake-worthy when cross-CPU dispatch SEVs them, which
         * is fine. The cost is CPU 0 burns power instead of sleeping;
         * acceptable for stock-BL31 builds. The JETSON_HW_TICK build
         * path above is the long-term answer (#380 / PR with
         * tools/tfa-patches/0004-*). */
        if (cpu_id() != 0) {
            __asm__ volatile("wfe" ::: "memory");
        }
#else
        if (cpu_id() == 0) {
            __asm__ volatile("msr daifclr, #2" ::: "memory");
            __asm__ volatile("wfi");
        } else {
            __asm__ volatile("wfe" ::: "memory");
        }
#endif
#else
        __asm__ volatile("msr daifclr, #2" ::: "memory");
        __asm__ volatile("wfi");
#endif

        /* Yield to check if other tasks are ready */
        yield();
    }
}

/*
 * Update a task's effective priority based on deadline proximity.
 *
 * Called when adding a task to the run queue and during scheduler tick.
 * The effective_priority determines scheduling order.
 */
static void update_deadline_boost(struct task *task)
{
    struct sched_runtime_deadline_thresholds runtime_cfg;

    if (!task) {
        return;
    }

    /* Deadline may have been set by another CPU */
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_invalidate(&task->deadline_ns);
#endif

    if (task->deadline_ns == 0) {
        /* No deadline - effective priority equals base priority */
        task->effective_priority = task->priority;
#if !defined(PLATFORM_HAS_NC_MEMORY)
        cache_clean(&task->effective_priority);
#endif
        return;
    }

    uint64_t now = slm_get_time_ns();
    uint8_t base = task->priority;
    uint8_t boosted = base;
    sched_set_default_deadline_thresholds(&runtime_cfg);
#ifdef CONFIG_AI_SCHEDULER
    (void)sched_runtime_deadline_thresholds_snapshot(&runtime_cfg);
#endif

    if (now >= task->deadline_ns) {
        /* Deadline missed! Boost to critical to finish ASAP */
        boosted = TASK_PRIORITY_CRITICAL;
    } else {
        uint64_t remaining = task->deadline_ns - now;

        if (remaining < runtime_cfg.critical_ns) {
            /* < 10ms: boost to CRITICAL (7) */
            boosted = TASK_PRIORITY_CRITICAL;
        } else if (remaining < runtime_cfg.high_ns) {
            /* < 50ms: boost to HIGH (6) */
            boosted = TASK_PRIORITY_HIGH;
        } else if (remaining < runtime_cfg.boost_ns) {
            /* < 100ms: boost +1 (capped at max) */
            boosted = (base < TASK_PRIORITY_MAX) ? base + 1 : TASK_PRIORITY_MAX;
        }
        /* else: > 100ms remaining, no boost */
    }

    task->effective_priority = boosted;
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&task->effective_priority);
#endif
}

/*
 * Initialize the scheduler (called on boot CPU).
 */
#if defined(PLATFORM_HAS_NC_MEMORY)
/* Called from main.c after vmm_init, before smp_init */
void nc_zero_sched_init_flag(void)
{
    volatile uint32_t *nc_flag = (volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 64);
    *nc_flag = 0;
    __asm__ volatile("dc civac, %0" :: "r"(nc_flag) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");

    /* Zero NC_DBG and NC_FLAG_READ regions so stale previous-boot data
     * is cleared. Any non-zero value after boot = written by current boot. */
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + i * 4) = 0;
        *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 192 + i * 4) = 0;
    }
}
#endif

void scheduler_init(void)
{

    /* Initialize global state */
    sched.task_count = 0;
    sched.context_switches = 0;
    sched.timer_ticks = 0;
    sched.isolated_cores = 0;

#if defined(PLATFORM_HAS_NC_MEMORY)
    nc_runqueues = ncmem_alloc(MAX_CPUS * sizeof(struct cpu_runqueue),
                               CACHE_LINE_SIZE);
    if (!nc_runqueues) {
        INFO("SMP: NC run queue allocation failed");
        for (;;)
            ;
    }
    INFO("SMP: run queues in NC memory at 0x%lx",
         (unsigned long)nc_runqueues);
#endif

    /* Initialize task table (NC on Pi 5, BSS fallback otherwise) */
    extern void task_table_init(void);
    task_table_init();

    /* Initialize sleep queue (#319) */
    task_sleep_init();

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* Allocate diagnostic counters from NC memory for cross-CPU visibility */
    sched_diag_tick = ncmem_alloc(MAX_CPUS * sizeof(uint32_t), 64);
    sched_diag_schedule = ncmem_alloc(MAX_CPUS * sizeof(uint32_t), 64);
    sched_diag_picked = ncmem_alloc(MAX_CPUS * sizeof(uint32_t), 64);
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        sched_diag_tick[i] = 0;
        sched_diag_schedule[i] = 0;
        sched_diag_picked[i] = 0;
    }
    sched_diag_idle_loops = ncmem_alloc(MAX_CPUS * sizeof(uint32_t), 64);
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        sched_diag_idle_loops[i] = 0;

    /* Work-stealing observability counters (#105, #175). */
    sched_diag_steal_attempts     = ncmem_alloc(MAX_CPUS * sizeof(uint32_t), 64);
    sched_diag_steal_successes    = ncmem_alloc(MAX_CPUS * sizeof(uint32_t), 64);
    sched_diag_steal_stale        = ncmem_alloc(MAX_CPUS * sizeof(uint32_t), 64);
    sched_diag_steal_empty_victim = ncmem_alloc(MAX_CPUS * sizeof(uint32_t), 64);
    sched_diag_steal_push_full    = ncmem_alloc(MAX_CPUS * sizeof(uint32_t), 64);
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        sched_diag_steal_attempts[i] = 0;
        sched_diag_steal_successes[i] = 0;
        sched_diag_steal_stale[i] = 0;
        sched_diag_steal_empty_victim[i] = 0;
        sched_diag_steal_push_full[i] = 0;
    }
#endif

    /* Secondary-CPU preemption state (Pi 5 only). No-op on other platforms. */
    preempt_init();
    /* #137: sanity-check CPU 0's MPIDR against the trampoline formula
     * before any secondary comes up. No-op when SECONDARY_PREEMPT is
     * undefined. */
    preempt_check_cpu_mpidr(0);

#if CONFIG_WORK_STEALING && defined(PLATFORM_HAS_NC_MEMORY)
    /* Allocate the per-CPU steal deque array from NC memory so cross-CPU
     * steals see writes without cache maintenance. `steal_deque_t`
     * contains a spinlock, which relies on SPINLOCK_SKIP_LOCKING
     * (barrier-only) on NC-memory platforms; ldaxr/stxr on NC memory
     * is never executed. See the comment above the cpu_steal_deques
     * declaration. */
    cpu_steal_deques = ncmem_alloc(MAX_CPUS * sizeof(steal_deque_t),
                                   CACHE_LINE_SIZE);
    if (!cpu_steal_deques) {
        panic("scheduler_init: failed to allocate cpu_steal_deques from NC memory");
    }
#endif

    /* Initialize per-CPU run queues with per-queue locks */
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        spin_init(&rq_lock[i]);
        cpu_rq(i)->head = NULL;
        cpu_rq(i)->tail = NULL;
        cpu_rq(i)->idle_task = NULL;
        cpu_rq(i)->zombie = NULL;
        cpu_rq(i)->ready_count = 0;
#if CONFIG_WORK_STEALING
        spin_init(&steal_deque_lock[i]);
        steal_deque_init(&cpu_steal_deques[i]);
#endif
    }

    /* Create idle task for boot CPU (CPU 0) */
    cpu_rq(0)->idle_task = task_create("idle", idle_task_func, NULL);
    if (!cpu_rq(0)->idle_task) {
        panic("scheduler_init: failed to create idle task");
    }
    cpu_rq(0)->idle_task->state = TASK_READY;
    cpu_rq(0)->idle_task->cpu_affinity = 0;  /* Pinned to CPU 0 */
    cpu_rq(0)->idle_task->assigned_cpu = 0;
    cpu_rq(0)->idle_task->flags |= TASK_FLAG_IDLE;   /* permanent idle marker */

    /* Register built-in policy */
    sched_register_policy(&sched_policy_heuristic);

    sched.initialized = 1;
    cache_clean(&sched.initialized);
#if defined(PLATFORM_HAS_NC_MEMORY)
    /* Also set an NC flag for secondary CPUs.
     * The cacheable sched.initialized + cache_clean/invalidate doesn't
     * work because DC CIVAC doesn't propagate through L2 on Pi 5.
     * Secondary CPUs poll the NC flag instead. */
    /* Signal via cpu_boot_flag — the ONLY proven cross-CPU channel.
     * Write value 2 to each secondary CPU's boot flag slot.
     * Same pattern as boot handshake (atomic store + cache_clean). */
    nc_sched_initialized = 1;
    /* Write to NC flag AND CIVAC to push through any cacheable TLB stale entry.
     * Even if CPU 0's TLB maps this address as cacheable (stale from boot.S),
     * CIVAC pushes L1→L2→DRAM. Secondary CPUs read from NC (DRAM). */
    {
        volatile uint32_t *nc_flag = (volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 64);
        *nc_flag = 1;
        __asm__ volatile("dc civac, %0" :: "r"(nc_flag) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");
    }
    {
        extern volatile uint32_t cpu_boot_flag[];
        for (uint32_t i = 1; i < cpu_count; i++) {
            cpu_boot_flag[i] = 2;
            /* DC CIVAC: clean L1→L2→PoC (main memory on BCM2712).
             * DC CVAC only goes to L2. DC CIVAC cleans then invalidates,
             * which forces the line all the way to DRAM. */
            __asm__ volatile("dc civac, %0" :: "r"(&cpu_boot_flag[i]) : "memory");
            __asm__ volatile("dsb sy" ::: "memory");
        }
    }
#endif

    /* Wake any secondary CPUs waiting for scheduler init */
#if !defined(PLATFORM_X86_64)
    __asm__ volatile("sev" ::: "memory");
#endif

    INFO("Scheduler initialized");
}

/*
 * Check if the scheduler has been initialized.
 */
int scheduler_is_initialized(void)
{
#if defined(PLATFORM_HAS_NC_MEMORY)
    /* Read the NC flag at a fixed NC address.
     * CPU 0 writes 1 to this address in scheduler_init().
     * NC reads bypass L2, going directly to DRAM. */
    return *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 64) == 1;
#else
    cache_invalidate(&sched.initialized);
    return sched.initialized;
#endif
}

/*
 * Initialize scheduler for a secondary CPU.
 * Creates the idle task for this CPU.
 */
void scheduler_init_secondary(uint32_t cpu)
{
    if (cpu == 0 || cpu >= MAX_CPUS) {
        return;  /* CPU 0 uses scheduler_init(), invalid CPUs ignored */
    }

#if !defined(PLATFORM_X86_64)
    /* Debug: mark entry into scheduler_init_secondary via boot_flag */
    {
        extern volatile uint32_t cpu_boot_flag[];
        cpu_boot_flag[cpu] = 0x33;  /* Distinctive: entered scheduler_init_secondary */
        __asm__ volatile("dc civac, %0" :: "r"(&cpu_boot_flag[cpu]) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");
    }
#endif

    /* Create idle task for this CPU */
    char idle_name[TASK_NAME_LEN];
    /* Simple integer to string for idle task name */
    idle_name[0] = 'i';
    idle_name[1] = 'd';
    idle_name[2] = 'l';
    idle_name[3] = 'e';
    idle_name[4] = '_';
    idle_name[5] = '0' + cpu;
    idle_name[6] = '\0';

    /* Create task outside of lock (task_create may allocate memory) */
    struct task *idle = task_create(idle_name, idle_task_func, NULL);
    if (!idle) {
        panic("scheduler_init_secondary: failed to create idle task for CPU %u", cpu);
    }

#if !defined(PLATFORM_X86_64)
    /* Debug: mark post-task_create via boot_flag */
    {
        extern volatile uint32_t cpu_boot_flag[];
        cpu_boot_flag[cpu] = 0x44;  /* Distinctive value to verify current binary */
        __asm__ volatile("dc civac, %0" :: "r"(&cpu_boot_flag[cpu]) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");
    }
#endif

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xB0 = about to acquire rq_lock */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + cpu * 4) = 0xB0;
#endif

    /* Lock this CPU's queue to update idle task */
    irq_flags_t flags = rq_lock_irqsave(cpu);

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xB1 = acquired rq_lock */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + cpu * 4) = 0xB1;
#endif

    idle->state = TASK_READY;
    idle->cpu_affinity = cpu;  /* Pinned to this CPU */
    idle->assigned_cpu = cpu;
    idle->flags |= TASK_FLAG_IDLE;   /* permanent idle marker */
    cpu_rq(cpu)->idle_task = idle;

    rq_unlock_irqrestore(cpu, flags);

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xB2 = released rq_lock, function about to return */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + cpu * 4) = 0xB2;
#endif

    /* Skip INFO print on secondary CPUs — uart_lock contention may hang */
    if (cpu == 0) {
        INFO("CPU %u: scheduler initialized", cpu);
    }
}

/*
 * Add a task to a specific CPU's run queue (internal, requires lock held).
 *
 * Tasks are inserted in priority order (highest effective_priority first).
 * Within the same priority, new tasks go after existing ones (FIFO).
 */
static void add_to_cpu_queue_locked(struct task *task, uint32_t cpu)
{
    struct cpu_runqueue *rq = cpu_rq(cpu);

    task->assigned_cpu = cpu;

    /* Update effective priority based on deadline before insertion */
    update_deadline_boost(task);

    /* Empty queue - just add */
    if (!rq->head) {
        task->next = NULL;
        rq->head = task;
        rq->tail = task;
        rq->ready_count++;
        return;
    }

    /* Find insertion point: insert before first task with LOWER priority */
    struct task *prev = NULL;
    struct task *curr = rq->head;

    while (curr && curr->effective_priority >= task->effective_priority) {
        prev = curr;
        curr = curr->next;
    }

    /* Insert task between prev and curr */
    task->next = curr;

    if (prev) {
        prev->next = task;
    } else {
        /* New head (highest priority) */
        rq->head = task;
    }

    /* Update tail if inserting at end */
    if (!curr) {
        rq->tail = task;
    }

    rq->ready_count++;
}

/*
 * Remove a task from its CPU's run queue (internal, requires lock held).
 * Returns 1 if task was found and removed, 0 if not found.
 */
static int remove_from_cpu_queue_locked(struct task *task, uint32_t cpu)
{
    struct cpu_runqueue *rq = cpu_rq(cpu);
    struct task *prev = NULL;
    struct task *curr = rq->head;

    while (curr) {
        if (curr == task) {
            if (prev) {
                prev->next = curr->next;
            } else {
                rq->head = curr->next;
            }

            if (curr == rq->tail) {
                rq->tail = prev;
            }

            task->next = NULL;
            rq->ready_count--;
            return 1;  /* Found and removed */
        }
        prev = curr;
        curr = curr->next;
    }
    return 0;  /* Not found */
}

/*
 * Add a task to a specific CPU's run queue.
 */
void scheduler_add_task_to_cpu(struct task *task, uint32_t cpu)
{
    if (!task || task->state != TASK_READY || cpu >= cpu_count) {
        return;
    }

    struct cpu_runqueue *rq = cpu_rq(cpu);
    (void)rq; /* Used by DEBUG_PRINT in debug builds */
    irq_flags_t flags = rq_lock_irqsave(cpu);

    add_to_cpu_queue_locked(task, cpu);
    sched.task_count++;  /* Racy but acceptable for stats */

    DEBUG_PRINT("Added task '%s' to CPU %u run queue (ready=%u)",
                task->name, cpu, rq->ready_count);

    /* Clean specific fields written inside the lock to PoC.
     * Without SMPEN, these stay in our L1 cache. The target CPU's
     * schedule() invalidates before reading. Only clean the fields
     * that were modified — NOT the entire task struct (which includes
     * the task's context/stack that may be in active use). */
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&rq->head);
    cache_clean(&rq->tail);
    cache_clean(&rq->ready_count);
    cache_clean(&task->next);
    cache_clean(&task->assigned_cpu);
    cache_clean(&task->state);
    cache_clean(&task->effective_priority);
#endif

    rq_unlock_irqrestore(cpu, flags);

#if CONFIG_WORK_STEALING
    /*
     * Work-stealing: publish unpinned tasks to this CPU's steal deque so
     * other CPUs can pull them. Pinned tasks (cpu_affinity != ANY) stay
     * on their target CPU. Push failures (deque full) are non-fatal —
     * the task still runs on its owner, just not stealable. Idle tasks
     * are never stealable.
     *
     * Serialized by steal_deque_lock[cpu] (cacheable, real exclusive
     * monitors on every platform). The embedded lock inside the
     * deque is a no-op on Jetson.
     */
    if (task->cpu_affinity == CPU_AFFINITY_ANY &&
        task != cpu_rq(cpu)->idle_task) {
        irq_flags_t sd_flags = spin_lock_irqsave(&steal_deque_lock[cpu]);
        int rc = steal_deque_push(&cpu_steal_deques[cpu], task);
        spin_unlock_irqrestore(&steal_deque_lock[cpu], sd_flags);
        if (rc < 0)
            sched_diag_steal_push_full[cpu]++;
    }
#endif

    /* Wake the target CPU if it's parked. Platform backend:
     *   ARM64  — SEV (released WFE on that CPU).
     *   x86-64 — LAPIC IPI RESCHED_VECTOR (wakes HLT, invokes schedule()).
     * Cheap no-op when cpu == cpu_id(). */
    smp_notify_cpu(cpu);
}

/*
 * Proactive load-balancing helper (plan §S5).
 *
 * Scans every non-isolated CPU's `ready_count` and returns the CPU
 * with the smallest count. Ties broken by lower CPU id. The read
 * is racy — another CPU's `add_to_cpu_queue_locked` may be in
 * flight — but the caller's `scheduler_add_task_to_cpu` locks the
 * final target's rq, so a stale read only costs a slightly-imperfect
 * placement.
 *
 * Also computes and returns the sum of `ready_count` across the
 * same non-isolated set so the caller can decide whether the
 * policy-chosen target is "meaningfully overloaded" relative to
 * the average.
 *
 * Isolated CPUs are excluded on both sides — the override must not
 * redirect a task onto a core the admin told the scheduler to keep
 * exclusive. If every CPU is isolated, returns `fallback`.
 */
static uint32_t least_loaded_cpu(uint32_t fallback, uint32_t *out_sum,
                                 uint32_t *out_count)
{
    uint32_t best_cpu = fallback;
    uint32_t best_ready = UINT32_MAX;
    uint32_t sum = 0;
    uint32_t n = 0;
    for (uint32_t c = 0; c < cpu_count; c++) {
        if (sched.isolated_cores & (1U << c)) {
            continue;
        }
        uint32_t r = cpu_rq(c)->ready_count;
        sum += r;
        n++;
        if (r < best_ready) {
            best_ready = r;
            best_cpu = c;
        }
    }
    if (out_sum) {
        *out_sum = sum;
    }
    if (out_count) {
        *out_count = n;
    }
    return best_cpu;
}

static void sched_balance_config_defaults(
    struct sched_runtime_balance_config *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->feature_version = SCHED_MODEL_FEATURE_VERSION_V1;
    cfg->action_version = SCHED_MODEL_ACTION_VERSION_V1;
    cfg->enabled = SCHED_BALANCE_DEFAULT_ENABLED;
    cfg->min_target_ready = SCHED_BALANCE_DEFAULT_MIN_TARGET_READY;
    cfg->min_active_cpus = SCHED_BALANCE_DEFAULT_MIN_ACTIVE_CPUS;
    cfg->imbalance_num = SCHED_BALANCE_DEFAULT_IMBALANCE_NUM;
    cfg->imbalance_den = SCHED_BALANCE_DEFAULT_IMBALANCE_DEN;
}

/*
 * Add a task to the run queue (assigns to a CPU based on affinity/policy).
 *
 * Tasks with explicit CPU affinity are placed on that CPU directly.
 * Tasks with CPU_AFFINITY_ANY are assigned by the active scheduling
 * policy, then passed through a proactive load-balance override
 * (plan §S5): if the policy's target queue is ≥1.5× the average
 * load across all CPUs AND a strictly-less-loaded CPU exists, the
 * task is redirected there. This complements work-stealing — the
 * tick-driven rebalancer and idle-CPU steals fix imbalance that has
 * already happened; this avoids creating imbalance in the first
 * place on bursty workloads like `bench stealing`.
 */
void scheduler_add_task(struct task *task)
{
    if (!task || task->state != TASK_READY) {
        return;
    }

#ifdef CONFIG_AI_SCHEDULER
    task->arrival_time_ns = slm_get_time_ns();
    /* -1 signals "no AI decision recorded yet" (#211). */
    task->last_ai_action = -1;
#endif

    uint32_t target_cpu;

    /* Affinity may have been set by another CPU */
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_invalidate(&task->cpu_affinity);
#endif

    if (task->cpu_affinity != CPU_AFFINITY_ANY) {
        target_cpu = task->cpu_affinity;
    } else {
        target_cpu = active_policy->assign_cpu(task);

        /* S5 proactive load-balance override. Only for unpinned tasks,
         * never to isolated CPUs, never when the target is lightly
         * loaded, and only if we actually have somewhere better to
         * put it. The policy is authoritative for the first placement;
         * S5 only intervenes when the target is meaningfully more
         * loaded than the average across the non-isolated set. */
#ifdef CONFIG_AI_SCHEDULER
        struct sched_runtime_balance_config runtime_cfg;
        const struct sched_runtime_balance_config *active_cfg = NULL;
        sched_runtime_token_t cfg_token = 0;

        sched_balance_config_defaults(&runtime_cfg);
        if (sched_runtime_balance_config_acquire(&active_cfg, &cfg_token)) {
            runtime_cfg = *active_cfg;
            sched_runtime_balance_config_release(cfg_token);
        }
#else
        struct sched_runtime_balance_config runtime_cfg;
        sched_balance_config_defaults(&runtime_cfg);
#endif

        if (cpu_count > 1 && target_cpu < cpu_count &&
            runtime_cfg.enabled != 0 &&
            !(sched.isolated_cores & (1U << target_cpu))) {
            uint32_t target_ready = cpu_rq(target_cpu)->ready_count;
            /* Don't override on lightly-loaded systems — single-digit
             * ready counts are exactly where the policy's warmth
             * heuristics (cache affinity, deadline boost) pay off.
             * Threshold of 2 guarantees we only act after the target
             * actually starts building a backlog. */
            if (target_ready >= runtime_cfg.min_target_ready) {
                uint32_t sum = 0;
                uint32_t active = 0;
                uint32_t idle = least_loaded_cpu(target_cpu, &sum, &active);
                if (active >= runtime_cfg.min_active_cpus && idle != target_cpu) {
                    uint32_t idle_ready = cpu_rq(idle)->ready_count;
                    /* Predicate: target_ready > (sum / active) * 1.5.
                     * Integer form: 2 * target_ready * active > 3 * sum. */
                    if (idle_ready < target_ready &&
                        (uint64_t)runtime_cfg.imbalance_den * target_ready * active >
                            (uint64_t)runtime_cfg.imbalance_num * sum) {
                        target_cpu = idle;
                    }
                }
            }
        }
    }

    scheduler_add_task_to_cpu(task, target_cpu);
}

/*
 * Remove a task from the run queue.
 *
 * Snapshot/lock/recheck mirrors sched_migrate_task: a work-stealing thief
 * on another CPU can update task->assigned_cpu between our unlocked read
 * and the rq_lock acquire. If we just trusted the first read, we'd take
 * the wrong CPU's rq_lock, remove_from_cpu_queue_locked would return 0
 * silently, and the task would stay queued on its new owner.
 */
void scheduler_remove_task(struct task *task)
{
    if (!task) {
        return;
    }

    /* Bounded retry: each successful steal/migrate moves the task at most
     * once per scheduling cycle, so a small cap is plenty. */
    for (int attempts = 0; attempts < 8; attempts++) {
        uint32_t cpu = task->assigned_cpu;
        if (cpu >= cpu_count) {
            return;  /* Defensive: corrupt assigned_cpu — bail. */
        }
        struct cpu_runqueue *rq __attribute__((unused)) = cpu_rq(cpu);
        irq_flags_t flags = rq_lock_irqsave(cpu);

        if (task->assigned_cpu != cpu) {
            /* Moved between read and lock; release and try again. */
            rq_unlock_irqrestore(cpu, flags);
            continue;
        }

        if (remove_from_cpu_queue_locked(task, cpu)) {
            sched.task_count--;  /* Racy but acceptable for stats */
            DEBUG_PRINT("Removed task '%s' from CPU %u run queue (ready=%u)",
                        task->name, cpu, rq->ready_count);
        }

        rq_unlock_irqrestore(cpu, flags);
        return;
    }

    /* Retry budget exhausted — assigned_cpu changed 8 times in our window,
     * which should be effectively impossible. Log loudly so the dequeue
     * miss is visible if it ever happens. */
    WARN("scheduler_remove_task: gave up after 8 retries on task '%s'",
         task->name);
}

/*
 * Mark a task TERMINATED and dequeue it atomically under rq_lock.
 * Called by task_exit to close the pick-a-terminated-task race:
 * without a single locked region around state=TERMINATED + dequeue,
 * a cross-CPU scheduler could see stale state and pick the zombie.
 */
void scheduler_terminate_task(struct task *task)
{
    if (!task) {
        return;
    }

    /* Idle-task guard (#200/#606 investigation, 2026-05-02 / 03).
     * Refuse to mark an idle task TERMINATED — idle is perpetual,
     * and the scheduler dispatch panic captured on the pre-PR-598
     * baseline fired with `current = idle_2 AND state = TERMINATED`.
     * Uses the O(1) flag-bit predicate `is_idle_task()` (Linux
     * PF_IDLE pattern) — replaces an earlier loop-over-cpu_rq
     * pointer comparison. */
    if (is_idle_task(task)) {
        panic("scheduler_terminate_task: refusing to terminate idle task "
              "(task='%s', cpu_affinity=%u)", task->name, task->cpu_affinity);
    }

    /* Same snapshot/lock/recheck pattern as scheduler_remove_task: a
     * concurrent steal can change task->assigned_cpu under us. We pin
     * `cpu` once we successfully recheck under the lock so the post-loop
     * steal_deque_remove targets the same CPU whose rq_lock we held. */
    uint32_t cpu = task->assigned_cpu;
    int locked = 0;
    for (int attempts = 0; attempts < 8 && !locked; attempts++) {
        cpu = task->assigned_cpu;
        if (cpu >= cpu_count) {
            return;
        }
        irq_flags_t flags = rq_lock_irqsave(cpu);
        if (task->assigned_cpu != cpu) {
            rq_unlock_irqrestore(cpu, flags);
            continue;
        }

        struct cpu_runqueue *rq __attribute__((unused)) = cpu_rq(cpu);

        task->state = TASK_TERMINATED;
#if !defined(PLATFORM_HAS_NC_MEMORY) && !defined(PLATFORM_X86_64)
        cache_clean(&task->state);
#endif

        if (remove_from_cpu_queue_locked(task, cpu)) {
            sched.task_count--;
            DEBUG_PRINT("Terminated task '%s' on CPU %u (ready=%u)",
                        task->name, cpu, rq->ready_count);
        }

        rq_unlock_irqrestore(cpu, flags);
        locked = 1;
    }
    if (!locked) {
        /* Retry budget exhausted. The task is left in its prior state
         * (NOT TASK_TERMINATED), which means task_destroy will refuse
         * to reclaim it (per kernel/CLAUDE.md "Leaky tests"). Log so
         * the leak is visible — should never happen in practice. */
        WARN("scheduler_terminate_task: gave up after 8 retries on task '%s' "
             "(state=%d) — leaking task slot",
             task->name, (int)task->state);
        return;
    }

#if CONFIG_WORK_STEALING
    /* Clear the task's pointer from its owner's steal deque so a thief
     * doesn't grab a stale entry that later refers to a recycled task
     * slot (ABA race). See docs/archive/plans/jetson-capstone-execution-plan.md S1
     * for the panic that surfaced this — became observable on Jetson
     * after cpu_steal_deques moved to NC memory in commit <pending>
     * (before that, incoherent caches hid the race by failing the
     * steal silently). Serialized by steal_deque_lock[cpu]. */
    {
        irq_flags_t sd_flags = spin_lock_irqsave(&steal_deque_lock[cpu]);
        steal_deque_remove(&cpu_steal_deques[cpu], task);
        spin_unlock_irqrestore(&steal_deque_lock[cpu], sd_flags);
    }
#endif
}

/*
 * Migrate a task to a different CPU.
 *
 * Locks both source and target queues in CPU ID order to prevent deadlock.
 */
int sched_migrate_task(struct task *task, uint32_t target_cpu)
{
    if (!task || target_cpu >= cpu_count) {
        return -1;
    }

    /* Cannot migrate running tasks */
    if (task->state == TASK_RUNNING) {
        WARN("Cannot migrate running task '%s'", task->name);
        return -1;
    }

    /* Check affinity constraint */
    if (task->cpu_affinity != CPU_AFFINITY_ANY &&
        task->cpu_affinity != target_cpu) {
        WARN("Cannot migrate task '%s' - affinity constraint", task->name);
        return -1;
    }

    uint32_t old_cpu = task->assigned_cpu;

    if (old_cpu == target_cpu) {
        return 0;  /* Already on target CPU */
    }

    /*
     * Lock both queues in CPU ID order to prevent deadlock.
     * If old_cpu < target_cpu, lock old first; otherwise lock target first.
     */
    struct cpu_runqueue *rq_old __attribute__((unused)) = cpu_rq(old_cpu);
    struct cpu_runqueue *rq_new __attribute__((unused)) = cpu_rq(target_cpu);
    irq_flags_t flags;

    if (old_cpu < target_cpu) {
        flags = rq_lock_irqsave(old_cpu);
        spin_lock(&rq_lock[target_cpu]);
    } else {
        flags = rq_lock_irqsave(target_cpu);
        spin_lock(&rq_lock[old_cpu]);
    }

    /* Remove from old CPU queue if task is ready.
     *
     * Check the remove return value: between the outer `old_cpu =
     * task->assigned_cpu` read and this lock-protected region, a
     * work-stealing thief on another CPU may have pulled the task
     * from old_cpu's queue and put it on its own. In that case
     * remove_from_cpu_queue_locked returns 0 and we must NOT
     * add_to_cpu_queue_locked — that would set task->next (and
     * rq->head/tail) and link the task into target_cpu's queue
     * while it's still on the thief's queue, producing a task that
     * exists in two queues simultaneously. Two different CPUs would
     * then pick it up and run it concurrently on shared stack state.
     *
     * If the task was stolen, just update assigned_cpu so future
     * placements prefer target_cpu; the thief (or whoever has the
     * task) will run it. The test still observes the task running
     * because the body is written to NC memory from whichever CPU
     * executes it. */
    if (task->state == TASK_READY) {
        if (remove_from_cpu_queue_locked(task, old_cpu)) {
            add_to_cpu_queue_locked(task, target_cpu);
        } else {
            /* Stolen during the lock window. Record target preference
             * without corrupting queues. */
            task->assigned_cpu = target_cpu;
        }
    } else {
        /* Task is blocked - just update assigned_cpu */
        task->assigned_cpu = target_cpu;
    }

    /* Clean modified fields to PoC for cross-CPU visibility.
     * Without SMPEN, writes stay in this CPU's L1 cache. The target
     * CPU's schedule() invalidates before reading. */
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&rq_old->head);
    cache_clean(&rq_old->tail);
    cache_clean(&rq_old->ready_count);
    cache_clean(&rq_new->head);
    cache_clean(&rq_new->tail);
    cache_clean(&rq_new->ready_count);
    cache_clean(&task->next);
    cache_clean(&task->assigned_cpu);
    cache_clean(&task->state);
    cache_clean(&task->effective_priority);
#endif

    DEBUG_PRINT("Migrated task '%s' from CPU %u to CPU %u",
                task->name, old_cpu, target_cpu);

    /* #195: migration trace event. Recorded inside the dual-locked
     * region to keep the event timestamp ordered with the state
     * change. No-op when tracing is disabled. */
    sched_trace_record_migrate(task, old_cpu, target_cpu);

    /* Unlock in reverse order */
    if (old_cpu < target_cpu) {
        spin_unlock(&rq_lock[target_cpu]);
        rq_unlock_irqrestore(old_cpu, flags);
    } else {
        spin_unlock(&rq_lock[old_cpu]);
        rq_unlock_irqrestore(target_cpu, flags);
    }

    /* Wake target CPU if it's parked in WFE. spin_unlock's SEV is
     * a broadcast but gets consumed by one WFE cycle on each CPU;
     * an explicit notify ensures target_cpu actually re-checks its
     * run queue after the migration is committed. Cheap no-op when
     * target_cpu == cpu_id(). */
    smp_notify_cpu(target_cpu);

    return 0;
}

/*
 * Pick the next task to run on this CPU.
 */
static struct task *pick_next_task(uint32_t cpu)
{
    struct cpu_runqueue *rq = cpu_rq(cpu);

    if (rq->head) {
        /* The comment in `schedule()`'s zombie-cleanup block claimed
         * "the picker's TERMINATED/DESTROYED skip is the
         * authoritative guard against stale-queue races" — but the
         * old picker had no such skip. Add one now so a stale queue
         * head (e.g. a task whose `task_exit` raced its enqueue path)
         * panics here with a meaningful message instead of being
         * silently dispatched to a freed entry pointer. (#200/#606
         * investigation, 2026-05-02.) */
        if (rq->head->state == TASK_TERMINATED ||
            rq->head->state == TASK_DESTROYED) {
            panic("pick_next_task: stale TERMINATED/DESTROYED task at queue "
                  "head (CPU %u, task='%s', state=%d)",
                  cpu, rq->head->name, (int)rq->head->state);
        }
        sched_diag_picked[cpu]++;
        return rq->head;
    }

    /* No ready tasks - run idle task. The idle struct's state is
     * never legitimately TERMINATED/DESTROYED (idle is perpetual);
     * if it is, the corruption happened upstream and the panic in
     * `schedule()` will surface it with a backtrace pointing to
     * the dispatch point rather than this picker. */
    return rq->idle_task;
}

#if CONFIG_WORK_STEALING
/*
 * Attempt to steal one READY task from another CPU's run queue.
 *
 * Must be called with no rq_lock held (takes the victim's rq_lock
 * internally). Returns the stolen task on success — caller owns it and
 * must add it to their own run queue. Returns NULL if no task was
 * stolen after scanning all other CPUs.
 *
 * Stale pointers in the deque are discarded during validation; each
 * failed validation pops one stale entry so the deque does not
 * accumulate garbage indefinitely.
 */
static struct task *sched_try_steal(uint32_t this_cpu)
{
    /* #105: per-CPU observability counters. `sched_diag_steal_*` are
     * incremented on the THIEF's CPU (this_cpu) so a `cpu` shell
     * dump reports the per-core balance of attempts/hits/stale
     * discards/empty-victim scans. Count the isolated-CPU early-
     * out below as an attempt so the shell diag reads honestly —
     * an isolated CPU that entered sched_try_steal DID attempt,
     * we just choose not to probe victims. */
    sched_diag_steal_attempts[this_cpu]++;

    /* Isolated cores must never pull work from other CPUs — that's
     * the whole point of isolation. The S5 placement override
     * already refuses to *target* an isolated CPU; this closes the
     * symmetric hole where an isolated CPU would steal from a
     * non-isolated neighbour and then run its idle loop while
     * holding a general-purpose task that was explicitly routed
     * away from it (test_proactive_load_balance_respects_isolation
     * would otherwise flake when CPU 2 or 3 acted as a thief). */
    if (sched.isolated_cores & (1U << this_cpu)) {
        return NULL;
    }

    for (uint32_t i = 1; i < cpu_count; i++) {
        uint32_t victim = (this_cpu + i) % cpu_count;
        if (victim == this_cpu)
            continue;

        /* Cheap early-out: if the victim is currently inside its own
         * schedule() critical section (between rq_unlock_irqrestore
         * and switch_to — see preempt_disabled flag at sched.c:~1040),
         * don't bother contending the lock it's almost certainly
         * about to take again. The victim's rq_lock (acquired below)
         * is still the primary mutual-exclusion mechanism that keeps
         * the steal race-free; this is belt-and-suspenders per the
         * cross-plan review (B3 / P2-3). */
        if (preempt_disabled[victim])
            continue;

        /* Drain any stale pointers along with finding a live one. Bounded
         * by deque capacity so this loop cannot spin forever. */
        int victim_had_entry = 0;
        for (uint32_t probe = 0; probe < STEAL_DEQUE_CAPACITY; probe++) {
            uint32_t captured_gen = 0;
            /* Serialize this steal against owner's pushes / other
             * thieves' steals on the same victim's deque. Held
             * briefly (for the pop) and released before we take
             * rq_lock[victim] — the two locks are never held
             * together so no ordering constraint exists between
             * them. */
            irq_flags_t sd_flags =
                spin_lock_irqsave(&steal_deque_lock[victim]);
            struct task *candidate =
                steal_deque_steal(&cpu_steal_deques[victim], &captured_gen);
            spin_unlock_irqrestore(&steal_deque_lock[victim], sd_flags);
            if (!candidate)
                break;  /* Empty — try next victim */
            victim_had_entry = 1;

            irq_flags_t flags = rq_lock_irqsave(victim);

            /* Generation check (#139) closes the ABA window: if this
             * deque slot captured a previous life of the task_table
             * slot (same pointer, new logical task), the live
             * task->generation differs and we must discard even
             * though state/affinity/assigned_cpu may all read as
             * valid for the current resident. Checked first so we
             * don't bother with the full structural validation for
             * an entry we already know is stale. */
            int stealable =
                candidate->generation == captured_gen &&
                candidate->state == TASK_READY &&
                candidate->assigned_cpu == victim &&
                candidate->cpu_affinity == CPU_AFFINITY_ANY &&
                remove_from_cpu_queue_locked(candidate, victim);

            rq_unlock_irqrestore(victim, flags);

            if (stealable) {
                sched_diag_steal_successes[this_cpu]++;
                return candidate;
            }
            /* Otherwise: stale pointer (structural mismatch or
             * generation mismatch), already popped from deque. Loop
             * again to drain another entry on the same victim. */
            sched_diag_steal_stale[this_cpu]++;
        }
        if (!victim_had_entry) {
            sched_diag_steal_empty_victim[this_cpu]++;
        }
    }
    return NULL;
}
#endif /* CONFIG_WORK_STEALING */

#if defined(COOP_PREEMPT)
/*
 * Cooperative-preemption tick driver.
 *
 * Pi 5's GICv2 + TF-A configuration does not deliver timer IRQs to EL1
 * (confirmed by Phase 1 diagnostics: HPPIR shows IRQ 30 pending, but
 * no exception vector ever fires). Rather than block preemption
 * entirely, drive scheduler_tick() from schedule() whenever CNTPCT_EL0
 * has advanced by >= one tick period (10 ms at 100 Hz) on this CPU
 * since the last synthetic tick. This gives "cooperative preemption":
 * scheduling decisions — AI policy, deadline boosts, migration —
 * become observable at the next yield or schedule() call, which covers
 * any workload that polls UART / message queues / locks.
 *
 * Tasks that busy-wait without yielding won't preempt mid-execution;
 * that's a known limitation documented in the coop-preempt rationale.
 */
/*
 * 64-byte alignment places each per-CPU entry on its own cache line
 * (no false sharing). Each CPU only reads/writes its own index, so
 * there's no true cross-CPU contention on this array; the alignment
 * is purely to stop other CPUs' dirty updates from ping-ponging a
 * neighboring entry's line. volatile on the declaration prevents
 * the compiler from holding stale values across the coop_preempt
 * tick computation.
 */
static volatile uint64_t coop_last_tick_cntpct[MAX_CPUS]
    __attribute__((aligned(64)));

/* timer_handler_count and pit_ticks are declared in timer.h. */

void scheduler_tick(void);

static inline void coop_preempt_maybe_tick(uint32_t cpu)
{
    uint64_t freq = timer_get_frequency();
    if (freq == 0)
        return;
    uint64_t period = freq / TIMER_HZ;       /* cycles per tick */
    uint64_t now = timer_get_count();
    uint64_t last = coop_last_tick_cntpct[cpu];
    if (last == 0) {
        coop_last_tick_cntpct[cpu] = now;
        /* Publish the initial timestamp so subsequent reads on this
         * CPU — and any observer — see the non-zero marker. */
        __asm__ volatile("dmb ish" ::: "memory");
        return;
    }
    if (now - last < period)
        return;

    /* Catch up — if we haven't scheduled in a long time (idle wfi or
     * long busy-wait), replay one tick and advance our marker by one
     * period. We only do one call per schedule entry to keep this path
     * bounded; the next schedule() call will catch up further. */
    coop_last_tick_cntpct[cpu] = last + period;

    /* dmb before bumping the global counters so any observer reading
     * timer_handler_count / pit_ticks sees our timestamp update as
     * already committed. The coop_last_tick_cntpct slot itself is
     * per-CPU, but the globals below are observed across CPUs. */
    __asm__ volatile("dmb ish" ::: "memory");

    /* Mirror what the real timer ISR does so downstream counters and
     * sleeper wakeups work: bump the global tick counters, then call
     * scheduler_tick to drive the policy. timer_handler_count doubles
     * as "cooperative preemption is live" in the `cpu` diag output. */
    timer_handler_count++;
    pit_ticks++;

    /* scheduler_tick would recursively call schedule() on the non-
     * SECONDARY_PREEMPT path; suppress that recursion by holding
     * preempt_disabled across the call. We're already inside schedule()
     * and about to pick next — the tick's policy work should run but
     * its "schedule now" side effect is redundant.
     *
     * Save-and-restore preempt_disabled so we don't clobber state the
     * outer schedule() may have set earlier (at boot, scheduler_start
     * pre-sets preempt_disabled=1 before first switch_to). */
    int prev_preempt_disabled = preempt_disabled[cpu];
    preempt_disabled[cpu] = 1;
    scheduler_tick();
    preempt_disabled[cpu] = prev_preempt_disabled;
}

/*
 * Voluntary preemption point — Track A of the Pi 5 preemption plan.
 *
 * Called from in-tree CPU-bound loops via the slm_preempt_point()
 * macro in <preempt_point.h>. Cheap when the quantum hasn't expired
 * (CNTPCT load, sub, compare, not-taken branch). Falls through to
 * schedule() when ≥10 ms has elapsed on this CPU since the last
 * coop tick — schedule() then runs coop_preempt_maybe_tick() and
 * may switch_to() a different runnable task.
 *
 * IMPORTANT — caller invariant: must NOT be invoked from inside a
 * spinlock-held region. SLM-OS's `spin_lock` / `spin_lock_irqsave`
 * (kernel/include/spinlock.h) do NOT bump `preempt_disabled[]`, so
 * the early-out below does not protect such callers — schedule()
 * would run with the lock still held, and another task contending
 * the same lock on this CPU would spin forever (spin loops do not
 * call slm_preempt_point()). The `preempt_disabled[cpu]` check
 * exists to skip recursion when the scheduler itself is in the
 * middle of a context switch, not to legitimize lock-held callers.
 */
volatile uint64_t slm_preempt_point_calls;
volatile uint64_t slm_preempt_point_schedule_calls;

void slm_preempt_check_and_yield(void)
{
    uint32_t cpu = cpu_id();
    if (cpu >= MAX_CPUS)
        return;

    slm_preempt_point_calls++;

    /* Skip when the scheduler is already mid-context-switch on this
     * CPU — recursing into schedule() through the preempt-point would
     * just re-enter the same code path. NOTE: this does NOT cover
     * spinlock-held callers; see the function header comment. */
    if (preempt_disabled[cpu])
        return;

    uint64_t freq = timer_get_frequency();
    if (freq == 0)
        return;
    uint64_t period = freq / TIMER_HZ;
    uint64_t now = timer_get_count();
    uint64_t last = coop_last_tick_cntpct[cpu];

    /* Fast path: quantum hasn't elapsed. The `last == 0` arm covers
     * the bootstrap case before the first real tick — we want to
     * fall through into schedule() once on first call so the
     * deadline gets seeded by coop_preempt_maybe_tick. */
    if (last != 0 && (now - last) < period)
        return;

    slm_preempt_point_schedule_calls++;
    schedule();
}
#endif /* COOP_PREEMPT */

/* ---- Periodic load rebalance (D1 / P2-4) ----
 *
 * Every REBALANCE_INTERVAL_TICKS timer ticks on BSP, migrate one
 * task from the busiest run queue to the idlest if the imbalance
 * exceeds REBALANCE_IMBALANCE_MIN. Respects cpu_affinity, never
 * touches the idle task, single-threaded (BSP-only) to keep the
 * decision racing-free. Called from the active policy's tick().
 */
#define REBALANCE_INTERVAL_TICKS   100
#define REBALANCE_IMBALANCE_MIN    2

#define SCHED_REBALANCE_DEFAULT_ENABLED       1u
#define SCHED_REBALANCE_DEFAULT_INTERVAL_TICKS REBALANCE_INTERVAL_TICKS
#define SCHED_REBALANCE_DEFAULT_IMBALANCE_MIN REBALANCE_IMBALANCE_MIN

static uint64_t rebalance_tick_counter;
static uint64_t rebalance_migrations;

static void sched_rebalance_config_defaults(
    struct sched_runtime_rebalance_config *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->feature_version = SCHED_MODEL_FEATURE_VERSION_V1;
    cfg->action_version = SCHED_MODEL_ACTION_VERSION_V1;
    cfg->enabled = SCHED_REBALANCE_DEFAULT_ENABLED;
    cfg->interval_ticks = SCHED_REBALANCE_DEFAULT_INTERVAL_TICKS;
    cfg->imbalance_min = SCHED_REBALANCE_DEFAULT_IMBALANCE_MIN;
}

void sched_rebalance_tick(uint32_t cpu)
{
    struct sched_runtime_rebalance_config runtime_cfg;

    /* Single-threaded: only BSP drives the rebalance decision. */
    if (cpu != 0) {
        return;
    }

    sched_rebalance_config_defaults(&runtime_cfg);
#ifdef CONFIG_AI_SCHEDULER
    (void)sched_runtime_rebalance_config_snapshot(&runtime_cfg);
#endif
    if (runtime_cfg.enabled == 0u) {
        return;
    }

    rebalance_tick_counter++;
    if ((rebalance_tick_counter % runtime_cfg.interval_ticks) != 0) {
        return;
    }

    if (cpu_count < 2) {
        return;
    }

    /* Snapshot-then-decide. Reading ready_count without a lock is
     * racy, but the final migration locks, so a stale snapshot only
     * means we rebalance "close to optimal". */
    uint32_t busy_cpu = 0;
    uint32_t idle_cpu = 0;
    uint32_t max_ready = 0;
    uint32_t min_ready = UINT32_MAX;
    for (uint32_t i = 0; i < cpu_count; i++) {
        uint32_t ready = cpu_rq(i)->ready_count;
        if (ready > max_ready) {
            max_ready = ready;
            busy_cpu = i;
        }
        if (ready < min_ready) {
            min_ready = ready;
            idle_cpu = i;
        }
    }
    if (busy_cpu == idle_cpu) {
        return;
    }
    if (max_ready < (uint32_t)(min_ready + runtime_cfg.imbalance_min)) {
        return;
    }

    /* Walk busy CPU's queue under its rq_lock, pick first migratable
     * task (CPU_AFFINITY_ANY, not idle, state READY), dequeue. */
    struct cpu_runqueue *busy_rq = cpu_rq(busy_cpu);
    irq_flags_t flags = rq_lock_irqsave(busy_cpu);

    struct task *candidate = NULL;
    for (struct task *t = busy_rq->head; t != NULL; t = t->next) {
        if (t == busy_rq->idle_task) {
            continue;
        }
        if (t->cpu_affinity != CPU_AFFINITY_ANY) {
            continue;
        }
        if (t->state != TASK_READY) {
            continue;
        }
        candidate = t;
        break;
    }

    if (candidate) {
        remove_from_cpu_queue_locked(candidate, busy_cpu);
    }

    rq_unlock_irqrestore(busy_cpu, flags);

    if (!candidate) {
        return;
    }

    /* scheduler_add_task_to_cpu handles rq_lock + smp_notify_cpu on
     * the destination CPU.
     *
     * Lock-release-then-modify is safe here. Between the unlock above
     * and the `assigned_cpu = idle_cpu` write below there is a window
     * where a thief on a third CPU could pop the candidate's stale
     * deque entry on busy_cpu's deque. The thief then acquires
     * rq_lock[busy_cpu], runs the validation in `sched_try_steal`,
     * and reaches `remove_from_cpu_queue_locked(candidate, busy_cpu)`
     * — which returns 0 because we already removed candidate from
     * busy_cpu's queue under the lock. The result is `stealable == 0`
     * and the thief discards the entry as stale. The candidate then
     * makes its way to idle_cpu via the add_task call below. */
    candidate->state = TASK_READY;
    candidate->assigned_cpu = idle_cpu;
    scheduler_add_task_to_cpu(candidate, idle_cpu);

    rebalance_migrations++;
}

uint64_t sched_rebalance_get_migrations(void)
{
    return rebalance_migrations;
}

/*
 * Schedule - select next task and switch to it (per-CPU).
 */
void schedule(void)
{
    uint32_t this_cpu = cpu_id();
    struct cpu_runqueue *rq = cpu_rq(this_cpu);
    sched_diag_schedule[this_cpu]++;

#if defined(COOP_PREEMPT)
    /* Cooperative preemption: drive scheduler_tick off CNTPCT when the
     * hardware timer IRQ path isn't delivering (see issue #99). */
    coop_preempt_maybe_tick(this_cpu);
#endif

    /* On non-NC platforms, invalidate cached copy of run queue before reading.
     * On Pi 5 with NC run queues, this is a no-op (NC data not cached). */
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_invalidate_range(rq, sizeof(*rq));
#endif

#if CONFIG_WORK_STEALING
    /*
     * Work-stealing: if the local queue is empty, try to pull a task
     * from another CPU before falling through to idle. Done outside the
     * local rq_lock to avoid nesting with the victim's rq_lock. The
     * read of rq->head is racy (another CPU may add work just after we
     * check), but missing a task is benign — we just go idle and the
     * next schedule() call picks it up.
     */
    if (!rq->head) {
        struct task *stolen = sched_try_steal(this_cpu);
        if (stolen) {
            irq_flags_t sf = rq_lock_irqsave(this_cpu);
            add_to_cpu_queue_locked(stolen, this_cpu);
            rq_unlock_irqrestore(this_cpu, sf);
        }
    }
#endif

    irq_flags_t flags = rq_lock_irqsave(this_cpu);

    /*
     * Clean up zombie task from previous schedule cycle.
     * This is safe because we've already switched away from it.
     *
     * State stays TASK_TERMINATED here so tests / external callers that
     * wait for TERMINATED can observe it before the slot is cleared.
     * The picker's TERMINATED/DESTROYED skip (pick_next_task) is the
     * authoritative guard against stale-queue races; the state flip to
     * TASK_DESTROYED happens inside task_destroy once the state check
     * there has passed, making the transition observable only after
     * reclamation has started.
     */
    if (rq->zombie) {
        struct task *zombie = rq->zombie;
        rq->zombie = NULL;
#if !defined(PLATFORM_HAS_NC_MEMORY)
        cache_clean(&rq->zombie);
#endif
        rq_unlock_irqrestore(this_cpu, flags);

        /* Destroy outside lock - task_destroy may call pmm */
        task_destroy(zombie);

        flags = rq_lock_irqsave(this_cpu);
    }

    struct task *current = task_current();
    struct task *next = pick_next_task(this_cpu);

    /* If current task is still running and ready, re-add to queue */
    if (current) {
#if !defined(PLATFORM_HAS_NC_MEMORY)
        cache_invalidate(&current->state);
#endif
    }
    if (current && current->state == TASK_RUNNING) {
        current->state = TASK_READY;
#if !defined(PLATFORM_HAS_NC_MEMORY)
        cache_clean(&current->state);
#endif

        /* Re-add to run queue if it's a normal task (not idle) */
        if (current != rq->idle_task) {
            /* Priority-ordered insertion (updates deadline boost) */
            add_to_cpu_queue_locked(current, this_cpu);
        }

        /* Update deadline boost for next candidate too */
        if (rq->head) {
            update_deadline_boost(rq->head);
        }

        /* Re-pick in case queue changed */
        next = pick_next_task(this_cpu);
    }

    /* Remove next from front of queue */
    if (next != rq->idle_task && rq->head == next) {
        rq->head = next->next;
        if (!rq->head) {
            rq->tail = NULL;
        }
        next->next = NULL;
        rq->ready_count--;
    }

    /*
     * No switch needed if same task AND task is not terminated.
     * If the current task is terminated, we MUST switch to a different task.
     */
    if (next == current) {
        if (current && (current->state == TASK_TERMINATED ||
                        current->state == TASK_DESTROYED)) {
            rq_unlock_irqrestore(this_cpu, flags);
            panic("schedule: terminated/destroyed task selected as next (CPU %u, task '%s', state=%d)",
                  this_cpu, current->name, (int)current->state);
        }
        if (current) {
            current->state = TASK_RUNNING;
        }
        rq_unlock_irqrestore(this_cpu, flags);

        /* NOTE: On Pi 5, tasks run with DAIF.I=1 (IRQ masked) so pit_ticks
         * does not advance during task execution. pit_ticks only advances
         * when the idle task runs (it does daifclr+wfi). Components that
         * poll pit_ticks must yield frequently enough for idle to run.
         * Timer preemption on Pi 5 is not supported (causes hangs in IPC
         * spinlock paths). See docs/pi5-cross-cpu-dispatch-investigation.md. */
        return;
    }

    /* Perform context switch */
    next->state = TASK_RUNNING;
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&next->state);
#endif
    next->switches++;
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&next->switches);
#endif
    sched.context_switches++;  /* Racy but acceptable for stats */

    /*
     * If current task is terminated, mark it as zombie for cleanup.
     * It will be destroyed on the next schedule() call after we've
     * safely switched to a different stack.
     */
    if (current) {
#if !defined(PLATFORM_HAS_NC_MEMORY)
        cache_invalidate(&current->state);
#endif
    }
    if (current && current->state == TASK_TERMINATED) {
        rq->zombie = current;
    }

    task_set_current(next);

    /* #195: record the context switch while the lock is still held
     * so `current` and `next` reflect the committed state. No-op
     * when tracing is disabled. */
    sched_trace_record_switch(this_cpu, current, next);

    /* Clean run queue fields modified in this schedule() cycle.
     * Without this, the next dc civac at the start of schedule() would
     * write back our stale dirty cacheline, overwriting another CPU's
     * fresh data (e.g., a newly added task from scheduler_add_task). */
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&rq->head);
    cache_clean(&rq->tail);
    cache_clean(&rq->ready_count);
    cache_clean(&rq->zombie);
#endif

    /*
     * Disable preemption during the context switch window.
     *
     * Between rq_unlock (which re-enables IRQs) and switch_to completion,
     * a timer IRQ could fire and call scheduler_tick() → schedule().
     * That reentrant schedule() would see stale task_current() (already
     * set to `next`, but we're still on `current`'s stack). The
     * preempt_disabled flag tells scheduler_tick() to skip the
     * schedule() call during this window. The timer still fires and
     * increments tick counters — it just doesn't try to context-switch.
     *
     * Placement: AFTER the task_set_current / cache_clean sequence is
     * correct because rq_lock_irqsave (above) has already disabled local
     * IRQs, so no timer can deliver between task_set_current and
     * preempt_disabled=1. (SCHED-M1 originally proposed moving this
     * earlier, but the race it targeted does not exist — rq_lock_irqsave
     * already closes the window.)
     */
    preempt_disabled[this_cpu] = 1;

    rq_unlock_irqrestore(this_cpu, flags);

    /* Stack-overflow assertion. Validate the live SP register against
     * `current`'s stack range BEFORE we save it via switch_to.
     *
     * Catches the class of stack overflows that the bottom-of-stack
     * canary (TASK_STACK_CANARY_BYTES, 64 B) misses: a function with
     * a single large stack frame can decrement SP past the canary
     * region in one go, leaving the canary intact while landing the
     * SP inside an adjacent task's stack region. Once that happens
     * the running task starts corrupting its neighbor's frames; the
     * symptom often surfaces much later as a wild fault or a wedge
     * in scheduler bookkeeping, far from the original cause.
     *
     * Bumped STACK_SIZE to 256 KB after a 64 KB stack proved too
     * small for the SLM forward path (Qwen2.5-1.5B Q4_K_M). Keeping
     * this assertion as a permanent guard so a future workload that
     * grows past the new ceiling fails loudly here instead of
     * spelunking through corrupted task state. */
    if (current && current->stack_base && current->stack_top) {
        uint64_t live_sp;
#if defined(PLATFORM_X86_64)
        __asm__ volatile("mov %%rsp, %0" : "=r"(live_sp));
#else
        __asm__ volatile("mov %0, sp"   : "=r"(live_sp));
#endif
        uintptr_t base = (uintptr_t)current->stack_base;
        uintptr_t top  = (uintptr_t)current->stack_top;
        if (live_sp < base || live_sp > top) {
            extern int uart_printf(const char *fmt, ...);
            uart_printf("\nstack overflow: SP is outside current task's stack\n"
                        "  current id=%u name='%s' live_sp=0x%lx but "
                        "stack=[0x%lx..0x%lx)\n"
                        "  next id=%u name='%s'\n",
                        (unsigned int)current->id, current->name,
                        (unsigned long)live_sp,
                        (unsigned long)base, (unsigned long)top,
                        next ? (unsigned int)next->id : 0u,
                        next ? next->name : "(none)");
            panic("stack overflow: refusing to switch_to with SP outside "
                  "current task's [stack_base, stack_top]");
        }
    }

#if !defined(PLATFORM_X86_64)
    /* TTBR0_EL1 management for user-task transitions (#697 PR-3 + PR-4).
     *
     *   user → user / kernel → user: write next's L1.
     *   user → kernel : restore the boot L1 so the outgoing user L1
     *     (which task_destroy may free) is no longer the walker's
     *     active root. Without this, freeing the user L1 corrupts the
     *     active translation tables on the same CPU.
     *   kernel → kernel: leave TTBR0 alone (already boot L1).
     *
     * The swap runs under rq_lock_irqsave, so no other CPU (or this
     * CPU) can observe a half-swapped state. */
    if (next->is_user && next->user_l1_pa) {
        vmm_user_addrspace_switch(next->user_l1_pa, next->user_asid);
    } else if (current && current->is_user) {
        vmm_user_addrspace_switch(vmm_boot_l1_pa(), VMM_KERNEL_ASID);
    }
#endif

    switch_to(current, next);

    /* Resumed on our stack after being switched back.
     * Re-enable preemption so timer ticks can trigger scheduling. */
    preempt_disabled[this_cpu] = 0;
#if defined(SECONDARY_PREEMPT)
    /* Clear any pending reschedule flag set by scheduler_tick while
     * we were mid-switch — we just scheduled, so an immediate re-arm
     * after eret would be redundant. */
    reschedule_pending[this_cpu] = 0;
#endif
}

/*
 * Yield - voluntarily give up the CPU.
 */
void yield(void)
{
    schedule();
}

/*
 * Start the scheduler on this CPU.
 */
void scheduler_start(uint32_t this_cpu)
{
#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xC0 = entered scheduler_start */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + this_cpu * 4) = 0xC0;
#endif

    struct cpu_runqueue *rq = cpu_rq(this_cpu);

    if (!sched.initialized) {
        /* Write panic marker to NC before panic (uart may deadlock) */
#if defined(PLATFORM_HAS_NC_MEMORY)
        *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + this_cpu * 4) = 0xE1;
#endif
        panic("scheduler_start: scheduler not initialized");
    }

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xC1 = passed initialized check */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + this_cpu * 4) = 0xC1;
#endif

    if (this_cpu == 0) {
        INFO("CPU %u: Starting scheduler", this_cpu);
    }

    irq_flags_t flags = rq_lock_irqsave(this_cpu);

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xC2 = acquired rq lock */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + this_cpu * 4) = 0xC2;
#endif

    /* Pick first task */
    struct task *first = pick_next_task(this_cpu);
    if (!first) {
        rq_unlock_irqrestore(this_cpu, flags);
#if defined(PLATFORM_HAS_NC_MEMORY)
        *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + this_cpu * 4) = 0xE2;
#endif
        panic("scheduler_start: no tasks to run");
    }

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xC3 = picked a task */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + this_cpu * 4) = 0xC3;
#endif

    /* Remove from queue */
    if (first != rq->idle_task && rq->head == first) {
        rq->head = first->next;
        if (!rq->head) {
            rq->tail = NULL;
        }
        rq->ready_count--;
    }

    first->state = TASK_RUNNING;
    first->switches++;
    sched.context_switches++;  /* Racy but acceptable for stats */

    task_set_current(first);

    if (this_cpu == 0) {
        INFO("CPU %u: Switching to first task: '%s'", this_cpu, first->name);
    }

    rq_unlock_irqrestore(this_cpu, flags);

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xC4 = unlocked, about to start timer */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + this_cpu * 4) = 0xC4;
#endif

    /* Start timer and enable interrupts now that a task is active.
     * Must be done AFTER task_set_current() so that timer IRQ handler
     * can safely call task_current() in schedule(). */
    if (this_cpu == 0) {
        INFO("Starting timer (100 Hz)...");
    }
    timer_start();

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xC5 = timer started */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + this_cpu * 4) = 0xC5;
#endif

    /* Guard against timer re-entrancy during switch_to.
     * The trampoline clears this after context restore. */
    preempt_disabled[this_cpu] = 1;

    /* Unmask IRQs. If timer fires, scheduler_tick() sees preempt_disabled=1
     * and skips schedule(). Ticks still count for sleep/uptime. */
    if (this_cpu == 0) {
        INFO("Enabling interrupts...");
    }

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xC6 = pre-daifclr */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + this_cpu * 4) = 0xC6;
#endif

    /* Unmask IRQs.
     * With SECONDARY_PREEMPT: all CPUs enable here so the first timer
     * tick can arrive while switch_to runs (preempt_disabled is already
     * 1 to suppress re-entrant scheduling during the switch).
     * Without SECONDARY_PREEMPT, secondary CPUs must skip this because
     * a timer IRQ here would follow the broken direct-schedule-from-ISR
     * path. */
#if defined(PLATFORM_X86_64)
    __asm__ volatile("sti" ::: "memory");
#elif defined(PLATFORM_HAS_NC_MEMORY)
#if defined(PI5_FIQ_TIMER)
    /* Unmask both I and F — timer arrives as FIQ on this GICv2. */
    __asm__ volatile("msr daifclr, #0x3" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
#elif defined(SECONDARY_PREEMPT)
    __asm__ volatile("msr daifclr, #0x2" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
#else
    if (this_cpu == 0) {
        __asm__ volatile("msr daifclr, #0x2" ::: "memory");
        __asm__ volatile("isb" ::: "memory");
    }
    /* Secondary CPUs: idle_task_func does daifclr in its loop */
#endif
#else
    __asm__ volatile("msr daifclr, #0x2" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
#endif

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xC7 = past daifclr */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + this_cpu * 4) = 0xC7;
#endif

    /* Debug: mark that we reached pre-switch point */
    sched_diag_idle_loops[this_cpu] = 0xAAAA;

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xCC = about to call switch_to */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + this_cpu * 4) = 0xCC;
#endif

    /* Switch to first task (NULL = no previous context to save).
     * switch_to never returns — it jumps to task_entry_wrapper.
     * The trampoline clears preempt_disabled[this_cpu]. */
    switch_to(NULL, first);

    /* Should never reach here */
    panic("scheduler_start: switch_to returned!");
}

/*
 * Timer tick handler (called from interrupt).
 */
void scheduler_tick(void)
{
    uint32_t cpu = cpu_id();

    /* Wake any task whose sleep deadline has passed (#319). Cheap walk
     * of a global list; bounded by MAX_TASKS. task_wake_sleepers only
     * flips state back to TASK_READY and calls scheduler_add_task — it
     * does NOT schedule itself, so calling it during the preempt-
     * disabled window below is safe. Placed here (before the
     * preempt_disabled short-circuit at the next block) so sleepers
     * still wake even while this CPU is mid-context-switch. */
    task_wake_sleepers();

    /* Always count ticks (used by sleep_ms, uptime, benchmarks) */
    sched.timer_ticks++;
#if defined(PLATFORM_HAS_NC_MEMORY)
    /* sched_diag_tick pointer is in cacheable BSS — secondary CPUs
     * may have stale L2 data for it. Use direct NC address if available. */
    if (sched_diag_tick)
        sched_diag_tick[cpu]++;
#else
    sched_diag_tick[cpu]++;
#endif

#ifdef CONFIG_AI_SCHEDULER
    /* Per-CPU utilization tracking */
    {
        struct cpu_runqueue *rq = cpu_rq(cpu);
        rq->total_ticks++;
        struct task *cur = task_current();
        if (cur && cur != rq->idle_task) {
            rq->running_ticks++;
        }
    }

    /* Update top-8 task cache periodically (CPU 0 only) */
    if (cpu == 0 && (sched.timer_ticks % AI_TOP_TASKS_UPDATE_INTERVAL) == 0) {
        ai_update_top_tasks();
    }

#if !defined(PLATFORM_X86_64)
    /* Per-CPU cache_pressure sample (#872). Updates this CPU's slot in
     * `pmu_cache_pressure_q16_per_cpu[]`, which `ai_state.c` reads to
     * fill the state-vector cache_pressure feature. No-op on a CPU
     * where the PMU is not yet enabled. */
    pmu_sample_cache_pressure();
#endif
#endif

    /* Policy tick callback (stats collection, rebalancing).
     *
     * Contract: tick() MUST NOT call schedule() directly. The cooperative-
     * preemption path (coop_preempt_maybe_tick) invokes scheduler_tick()
     * from inside schedule() with preempt_disabled[cpu] = 1; a tick
     * callback that re-entered schedule() would either deadlock on
     * rq_lock or run with stale current/next state. Policies should
     * publish work via scheduler_add_task* and let the next preemption
     * point pick it up. */
    if (active_policy && active_policy->tick) {
        active_policy->tick(cpu);
    }

    /* Skip preemption if a context switch is in progress on this CPU.
     * schedule() sets preempt_disabled between rq_unlock and switch_to
     * completion. Calling schedule() here would corrupt state because
     * task_set_current(next) has already been called but the actual
     * stack switch hasn't happened yet. */
    if (preempt_disabled[cpu]) {
        return;
    }

#if defined(SECONDARY_PREEMPT)
    /* Defer the schedule() call to exception-return context via the ELR
     * trampoline. Calling switch_to() from inside the timer ISR hangs
     * on real ARM64 hardware because the abandoned exception frame's
     * SPSR_EL1/ELR_EL1 are never `eret`-ed back. The trampoline arms in
     * maybe_arm_resched_trampoline() (called from el1_irq just before
     * restore_regs/eret), and schedule() runs in task context. */
    reschedule_pending[cpu] = 1;
#else
    /* Preempt current task */
    schedule();
#endif
}

/*
 * Get scheduler statistics.
 *
 * Note: Stats are read without locking (racy but acceptable).
 * This avoids lock contention for frequent stats queries.
 */
void scheduler_get_stats(struct sched_stats *stats)
{
    if (!stats) return;

    /* Read global stats (racy but acceptable for diagnostics) */
    stats->task_count = sched.task_count;
    stats->context_switches = sched.context_switches;
    stats->timer_ticks = sched.timer_ticks;

    /* Sum ready counts from all CPUs */
    stats->ready_count = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        stats->ready_count += cpu_rq(i)->ready_count;
    }
}

/*
 * Dump scheduler state for debugging.
 *
 * Locks each CPU queue individually while dumping it.
 */
void scheduler_dump(void)
{
    /* Print global stats (racy but acceptable for debug output) */
    uart_puts("\nScheduler State:\n");
    uart_printf("  Initialized:      %s\n", sched.initialized ? "yes" : "no");
    uart_printf("  Task count:       %u\n", sched.task_count);
    uart_printf("  Context switches: %lu\n", sched.context_switches);
    uart_printf("  Timer ticks:      %lu\n", sched.timer_ticks);

    struct task *current = task_current();
    if (current) {
        uart_printf("  Current task:     '%s' (id=%u, cpu=%u)\n",
                    current->name, current->id, cpu_id());
    }

    /* Dump per-CPU run queues (lock each individually) */
    for (uint32_t i = 0; i < cpu_count; i++) {
        struct cpu_runqueue *rq = cpu_rq(i);
        irq_flags_t flags = rq_lock_irqsave(i);

        uart_printf("  CPU %u queue (%u): ", i, rq->ready_count);

        if (rq->head) {
            struct task *t = rq->head;
            while (t) {
                uart_printf("%s[p%u]", t->name, t->effective_priority);
                if (t->next) uart_puts(" -> ");
                t = t->next;
            }
            uart_puts("\n");
        } else {
            uart_puts("(empty)\n");
        }

        rq_unlock_irqrestore(i, flags);
    }
}

/*
 * Isolate a core from general scheduling.
 *
 * Isolated cores only run tasks that are explicitly pinned to them
 * via cpu_affinity. Tasks with CPU_AFFINITY_ANY will not be placed
 * on isolated cores.
 *
 * Additionally, SPIs (Shared Peripheral Interrupts) are routed away
 * from isolated cores to minimize interrupt interference. Timer IRQs
 * (PPIs) are unaffected - each CPU still gets its own timer interrupt
 * for scheduler preemption.
 *
 * Use for real-time or latency-sensitive workloads that need
 * dedicated CPU time without interference from other tasks.
 *
 * @cpu: CPU ID to isolate
 *
 * Returns: 0 on success, -1 if invalid CPU
 */
int sched_isolate_core(uint32_t cpu)
{
    if (cpu >= cpu_count) {
        return -1;
    }

    /* Cannot isolate CPU 0 (boot CPU runs system tasks) */
    if (cpu == 0) {
        WARN("Cannot isolate CPU 0 (boot CPU)");
        return -1;
    }

    sched.isolated_cores |= (1U << cpu);

    /* Route SPIs away from this CPU to minimize interrupt interference.
     * Timer IRQs (PPIs) are unaffected - still needed for preemption. */
    gic_exclude_cpu_from_spis(cpu);

    INFO("CPU %u: isolated from general scheduling (SPIs excluded)", cpu);
    return 0;
}

/*
 * Remove core isolation.
 *
 * Restores SPI routing to include this CPU and returns it to the
 * general scheduling pool.
 *
 * @cpu: CPU ID to un-isolate
 *
 * Returns: 0 on success, -1 if invalid CPU
 */
int sched_unisolate_core(uint32_t cpu)
{
    if (cpu >= cpu_count) {
        return -1;
    }

    sched.isolated_cores &= ~(1U << cpu);

    /* Restore SPI routing to this CPU */
    gic_include_cpu_in_spis(cpu);

    INFO("CPU %u: returned to general scheduling (SPIs restored)", cpu);
    return 0;
}

/*
 * Check if a core is isolated.
 *
 * @cpu: CPU ID to check
 *
 * Returns: 1 if isolated, 0 if not (or invalid CPU)
 */
int sched_is_core_isolated(uint32_t cpu)
{
    if (cpu >= cpu_count) {
        return 0;
    }
    return (sched.isolated_cores & (1U << cpu)) != 0;
}

/*
 * Get the bitmask of isolated cores.
 */
uint32_t sched_get_isolated_cores(void)
{
    return sched.isolated_cores;
}

/*
 * Set task CPU affinity.
 *
 * If the task is currently in a run queue, it will be migrated
 * to the new target CPU.
 */
int sched_set_task_affinity(struct task *task, uint32_t cpu)
{
    if (!task) {
        return -1;
    }

    /* Validate CPU (CPU_AFFINITY_ANY is a special value) */
    if (cpu != CPU_AFFINITY_ANY && cpu >= cpu_count) {
        return -1;
    }

    uint32_t old_affinity = task->cpu_affinity;
    task->cpu_affinity = cpu;

    /*
     * If affinity changed and task is ready, migrate to appropriate CPU.
     * Running tasks will be placed correctly on next schedule().
     */
    if (task->state == TASK_READY && old_affinity != cpu) {
        uint32_t target;
        if (cpu == CPU_AFFINITY_ANY) {
            target = active_policy->assign_cpu(task);
        } else {
            target = cpu;
        }

        if (target != task->assigned_cpu) {
            sched_migrate_task(task, target);
        }
    }

    return 0;
}
