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
#ifdef CONFIG_AI_SCHEDULER
#include "sched_trace_ai.h"
#endif
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
#include "preempt.h"
#if !defined(PLATFORM_X86_64)
#include "vmm.h"
#endif
#include "string.h"
#include "runtime_model.h"
#if CONFIG_WORK_STEALING
#include "steal_deque.h"
#endif
#include "slot_trace.h"
#include "switch_check.h"
/* Unconditional: the #1216 window hooks and their STEAL_WIN_* reasons
 * are referenced from schedule() in every configuration (they are
 * empty functions without work stealing). */
#include "steal_trace.h"
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
 * used by spinlocks needs cacheable memory. */
static SPINLOCK_ARRAY(rq_lock, MAX_CPUS);

#if CONFIG_WORK_STEALING
/*
 * Per-CPU stealable task deque (#59 Phase B). One
 * entry per CPU. Tasks are pushed here (in addition to the linked-list
 * run queue) when scheduler_add_task_to_cpu is called with a task whose
 * cpu_affinity is CPU_AFFINITY_ANY. Idle CPUs drain these in
 * sched_try_steal() before going to WFE.
 *
 * The deque may contain stale pointers (tasks that have since run or
 * been terminated); sched_try_steal() validates under victim's rq_lock
 * and the per-slot generation counter (#139) before accepting a steal.
 *
 * Placement: cacheable BSS on every platform; the caches are coherent
 * (docs/adr/0007-cross-cpu-coherency.md, #1193).
 *
 * Locking: see `steal_deque_lock[MAX_CPUS]` below. The deque's own
 * `steal_deque_*` functions are lockless and the callers in sched.c
 * are responsible for serializing access. The steal_deque_t used to
 * embed its own spinlock, but on Jetson `SPINLOCK_SKIP_LOCKING` made
 * that lock a no-op, so concurrent push/pop/steal could corrupt the
 * deque — observed as Instruction Abort at ELR=0x0 during `bench
 * stealing` on jetson-nano-2. The external-lock pattern matches
 * `rq_lock[]`: the lock lives in cacheable memory and uses real
 * exclusive monitors on every platform that supports them.
 */
static steal_deque_t cpu_steal_deques[MAX_CPUS];

/* Per-CPU steal-deque locks. Cacheable so exclusive-monitor
 * primitives work on every platform, unlike the embedded lock in
 * steal_deque_t which was neutered by SPINLOCK_SKIP_LOCKING on
 * Jetson. Acquire the victim's lock before any steal_deque_*
 * operation; never held across other locks. */
static SPINLOCK_ARRAY(steal_deque_lock, MAX_CPUS);

/* Failure-reason trace for `bench stealing` (#1203). Layout, writers
 * and the visibility argument are in steal_trace.h. Cacheable BSS on
 * every platform: steal_trace[c] is written only by CPU c inside
 * sched_try_steal, steal_trace_dq[v] only under steal_deque_lock[v]. */
static struct steal_trace_cpu steal_trace[MAX_CPUS];
static struct steal_trace_dq steal_trace_dq[MAX_CPUS];
static volatile uint32_t steal_trace_run;  /* 0 = never armed */

/* Victim-side preempt_disabled window trace (#1216); see steal_trace.h.
 * steal_pdwin[c] is written only by CPU c. The bin bounds and t0 are
 * written by sched_steal_trace_arm() before its release store of the
 * run number. */
#define STEAL_TRACE_WATCH 1U
static struct steal_pdwin_cpu steal_pdwin[MAX_CPUS];
static uint64_t steal_ph_bound[STEAL_PH_BINS - 1];
static uint64_t steal_woff_bound[STEAL_WOFF_BINS - 1];
static volatile uint64_t steal_trace_t0_cnt;
#endif /* CONFIG_WORK_STEALING */

/* Lock helpers that use the correct lock for a given CPU */
static inline irq_flags_t rq_lock_irqsave(uint32_t cpu)
{
    irq_flags_t f = spin_lock_irqsave(&rq_lock[cpu]);
    SLOT_TRACE_RQ_LOCKED(cpu);
    return f;
}
static inline void rq_unlock_irqrestore(uint32_t cpu, irq_flags_t flags)
{
    SLOT_TRACE_RQ_UNLOCKING(cpu);
    spin_unlock_irqrestore(&rq_lock[cpu], flags);
}

/*
 * Per-CPU run queue access. The run queues live in cacheable BSS
 * (sched.cpu_fallback) on every platform.
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

/* Per-CPU diagnostic counters for cross-CPU dispatch debugging,
 * declared in sched_diag.h. */
volatile uint32_t sched_diag_tick[MAX_CPUS];
volatile uint32_t sched_diag_schedule[MAX_CPUS];
volatile uint32_t sched_diag_picked[MAX_CPUS];
volatile uint32_t sched_diag_idle_loops[MAX_CPUS];
volatile uint32_t sched_diag_steal_attempts[MAX_CPUS];
volatile uint32_t sched_diag_steal_successes[MAX_CPUS];
volatile uint32_t sched_diag_steal_stale[MAX_CPUS];
volatile uint32_t sched_diag_steal_empty_victim[MAX_CPUS];
volatile uint32_t sched_diag_steal_push_full[MAX_CPUS];

/* cpu_rq() implementation — must be after sched struct definition */
static inline struct cpu_runqueue *cpu_rq(uint32_t cpu)
{
    return &sched.cpu_fallback[cpu];
}

/*
 * Public accessor for cpu_rq — used by external policies (sched_heuristic.c,
 * sched_ai.c) that need to inspect run queue state for CPU assignment.
 */
struct cpu_runqueue *sched_cpu_rq(uint32_t cpu)
{
    return cpu_rq(cpu);
}

/* Zero a whole run queue (#1045); see sched.h. */
void sched_runqueue_clear(struct cpu_runqueue *rq)
{
    if (!rq) return;
    volatile uint8_t *p = (volatile uint8_t *)rq;
    for (size_t i = 0; i < sizeof(*rq); i++) {
        p[i] = 0;
    }
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
        rq->zombie_gen = 0;
    }
    rq_unlock_irqrestore(cpu, flags);
}

/*
 * Complete prev's switch-out on this CPU (#1213); see sched.h.
 *
 * Ordering: this store-release is executed by the CPU that ran prev,
 * after switch_to() has written prev's callee-saved registers, SP and
 * FP/SIMD state into prev->context and after the last access to prev's
 * stack (switch_to has already moved SP to the incoming task). A
 * destroyer that reads on_cpu == 0 with a load-acquire therefore sees
 * all of those accesses complete before its own writes to the slot and
 * the stack pages (see task_destroy).
 */
void sched_finish_switch(struct task *prev)
{
    if (prev) {
        SWITCH_CHECK_OUT(prev);
        __atomic_store_n(&prev->on_cpu, 0, __ATOMIC_RELEASE);
    }
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
        /* Idle-loop heartbeat, read by `ws-diag` (integration tests)
         * and `cpu` (shell) to distinguish "secondary never reached
         * idle" (counter==0) from "secondary is idling but SEV is not
         * reaching it" (counter frozen after initial iterations). */
        sched_diag_idle_loops[cpu_id()]++;

        /* Unmask IRQ so timer interrupts can fire.
         * This must be inside the loop because context switch saves/restores
         * DAIF. When idle is preempted by the timer ISR, the saved DAIF has
         * IRQ masked (hardware masks IRQ on exception entry). On resume,
         * the restored DAIF keeps IRQ masked — so we must re-clear it here. */
#if defined(PLATFORM_X86_64)
        __asm__ volatile("sti" ::: "memory");
        __asm__ volatile("hlt");
#elif defined(PLATFORM_RASPI5) || defined(PLATFORM_JETSON_ORIN_NANO)
        /* Pi 5/Jetson idle loop. Which CPUs may WFI is a timer-IRQ
         * delivery question, keyed on the platform.
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

#ifdef CONFIG_AI_SCHEDULER
/* AI priority actions on (default) or placement-only (#1069). */
static volatile bool ai_priority_actions = true;

void sched_ai_set_priority_actions(bool enabled)
{
    ai_priority_actions = enabled;
}

bool sched_ai_priority_actions_enabled(void)
{
    return ai_priority_actions;
}

/*
 * One priority_adj step between the kernel's priority levels, matching
 * the training simulator's _adjust_priority (slm_sim/actions.py): find
 * the highest level <= current, then move one level down (lower) or up
 * (raise), clamped at IDLE and CRITICAL. Keep returns current as is.
 */
uint8_t sched_ai_step_priority(uint8_t current, uint8_t priority_adj)
{
    static const uint8_t levels[] = {
        TASK_PRIORITY_IDLE, TASK_PRIORITY_LOW, TASK_PRIORITY_NORMAL,
        TASK_PRIORITY_HIGH, TASK_PRIORITY_CRITICAL,
    };
    const int n = (int)(sizeof(levels) / sizeof(levels[0]));

    if (priority_adj != AI_PRIO_LOWER && priority_adj != AI_PRIO_RAISE) {
        return current;
    }
    int idx = 0;
    for (int i = 0; i < n; i++) {
        if (levels[i] <= current) {
            idx = i;
        }
    }
    if (priority_adj == AI_PRIO_LOWER) {
        idx = (idx > 0) ? idx - 1 : 0;
    } else {
        idx = (idx < n - 1) ? idx + 1 : n - 1;
    }
    return levels[idx];
}

static uint8_t effective_base_priority(struct task *task);

/*
 * Record an AI decision's priority on the task. Steps from the current
 * effective priority (which already carries any earlier AI level and
 * deadline boost), as the simulator does. A single-byte store: readers
 * recompute under rq_lock and see either the old or the new level.
 */
void sched_ai_apply_priority_action(struct task *task, uint8_t priority_adj)
{
    if (!task || !ai_priority_actions) {
        return;
    }
    /* While a task runs on a borrowed priority (#1103) its effective priority
     * says nothing about the task itself: stepping from it would leave the
     * borrowed level behind as the AI's choice after the mutex is released.
     * Record no new level. On the usual path scheduler_add_task has already
     * cleared the old one, so the task has no AI level until its next
     * placement after the inheritance ends. A floor at or below the task's
     * own base priority borrows nothing, so the decision goes ahead. */
    if (task->pi_priority > effective_base_priority(task)) {
        return;
    }
    task->ai_priority = sched_ai_step_priority(task->effective_priority,
                                               priority_adj);
}

/* Base priority for a recompute: the AI's level while one is set. */
static uint8_t effective_base_priority(struct task *task)
{
    if (ai_priority_actions && task->ai_priority != TASK_AI_PRIORITY_NONE) {
        return task->ai_priority;
    }
    return task->priority;
}
#else
static uint8_t effective_base_priority(struct task *task)
{
    return task->priority;
}
#endif /* CONFIG_AI_SCHEDULER */

/*
 * Store a recomputed effective priority, never below what the task has
 * inherited through a pi_mutex (#1103).
 */
static void set_effective_priority(struct task *task, uint8_t priority)
{
    task->effective_priority = (task->pi_priority > priority) ? task->pi_priority : priority;
}

/*
 * Update a task's effective priority based on deadline proximity.
 *
 * Called when adding a task to the run queue and during scheduler tick.
 * The effective_priority determines scheduling order. The base is the
 * task's priority, or the AI policy's last chosen level (#1069). A priority
 * inherited through a pi_mutex is a floor on the result (#1103).
 */
static void update_deadline_boost(struct task *task)
{
    struct sched_runtime_deadline_thresholds runtime_cfg;

    if (!task) {
        return;
    }

    if (task->deadline_ns == 0) {
        /* No deadline: the base priority, or the inherited floor if higher */
        set_effective_priority(task, effective_base_priority(task));
        return;
    }

    uint64_t now = slm_get_time_ns();
    uint8_t base = effective_base_priority(task);
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
            /* < 50ms: boost to HIGH (6). Never lowers: the base can
             * already be CRITICAL (an AI raise, #1069). */
            boosted = (base > TASK_PRIORITY_HIGH) ? base : TASK_PRIORITY_HIGH;
        } else if (remaining < runtime_cfg.boost_ns) {
            /* < 100ms: boost +1 (capped at max) */
            boosted = (base < TASK_PRIORITY_MAX) ? base + 1 : TASK_PRIORITY_MAX;
        }
        /* else: > 100ms remaining, no boost */
    }

    set_effective_priority(task, boosted);
}

void sched_refresh_effective_priority(struct task *task)
{
    update_deadline_boost(task);
}

/*
 * Initialize the scheduler (called on boot CPU).
 */

void scheduler_init(void)
{

    /* Initialize global state */
    sched.task_count = 0;
    sched.context_switches = 0;
    sched.timer_ticks = 0;
    sched.isolated_cores = 0;

    /* Initialize task table */
    extern void task_table_init(void);
    task_table_init();

    /* Initialize sleep queue (#319) */
    task_sleep_init();

    /* Secondary-CPU preemption state (Pi 5 only). No-op on other platforms. */
    preempt_init();
    /* #137: sanity-check CPU 0's MPIDR against the trampoline formula
     * before any secondary comes up. No-op when SECONDARY_PREEMPT is
     * undefined. */
    preempt_check_cpu_mpidr(0);

    /* Initialize per-CPU run queues with per-queue locks */
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        spin_init(&rq_lock[i]);
        /* Whole struct, not field by field: the AI tick counters were
         * missed that way and started from leftover data (#1045). */
        sched_runqueue_clear(cpu_rq(i));
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

    /* Release: run queues, idle task and policy are visible to a
     * secondary that observes the flag with acquire. */
    __atomic_store_n(&sched.initialized, 1, __ATOMIC_RELEASE);

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
    return __atomic_load_n(&sched.initialized, __ATOMIC_ACQUIRE);
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
    }
#endif

    /* Lock this CPU's queue to update idle task */
    irq_flags_t flags = rq_lock_irqsave(cpu);

    idle->state = TASK_READY;
    idle->cpu_affinity = cpu;  /* Pinned to this CPU */
    idle->assigned_cpu = cpu;
    idle->flags |= TASK_FLAG_IDLE;   /* permanent idle marker */
    cpu_rq(cpu)->idle_task = idle;

    rq_unlock_irqrestore(cpu, flags);

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

    SLOT_TRACE(ST_ADD, task, cpu, __builtin_return_address(0), 0);
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
        if (rc == 0) {
            /* #1203 trace: written under steal_deque_lock[cpu]. */
            steal_trace_dq[cpu].pushes++;
            steal_trace_dq[cpu].last_push = timer_get_count();
            steal_trace_dq[cpu].push_run = steal_trace_run;
        }
        spin_unlock_irqrestore(&steal_deque_lock[cpu], sd_flags);
        if (rc < 0) {
            sched_diag_steal_push_full[cpu]++;
        }
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
    /* Each placement is a fresh decision: an AI policy sets a new level
     * in assign_cpu below; any other policy leaves none (#1069). */
    task->ai_priority = TASK_AI_PRIORITY_NONE;
#endif

    uint32_t target_cpu;

    if (task->cpu_affinity != CPU_AFFINITY_ANY) {
        target_cpu = task->cpu_affinity;
    } else {
        target_cpu = active_policy->assign_cpu(task);

#ifdef CONFIG_AI_SCHEDULER
        /* Record the policy's decision before the S5 override below
         * potentially modifies it — the trace is meant for training
         * the policy that just made the choice, not the
         * post-override target. The S5 outcome is recoverable from
         * later events anyway (the next decision's state vector
         * reflects the override). */
        sched_trace_ai_record_decision(task, active_policy->name, target_cpu);
#endif

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
 * Recompute a queued task's priority and move it if the queue order no longer
 * holds. Keeps its place otherwise, so a change that doesn't affect the order
 * (say a floor below its priority) can't send it behind its peers. Returns
 * true if it moved; false also if the task isn't in this queue.
 */
static bool reposition_locked(struct task *task, uint32_t cpu)
{
    struct task *prev = NULL;
    struct task *curr = cpu_rq(cpu)->head;

    while (curr && curr != task) {
        prev = curr;
        curr = curr->next;
    }
    if (!curr) {
        return false;
    }

    update_deadline_boost(task);
    if ((!prev || prev->effective_priority >= task->effective_priority) &&
        (!task->next || task->next->effective_priority <= task->effective_priority)) {
        return false;
    }

    (void)remove_from_cpu_queue_locked(task, cpu);
    add_to_cpu_queue_locked(task, cpu);
    return true;
}

/*
 * Move a queued task to the position its current priority calls for. The
 * run queue is ordered at insertion, so raising the priority of a task that
 * is already queued does not by itself let it run any sooner. A task that
 * is running, blocked or in another CPU's hands is left alone: it picks the
 * priority up at its next insertion.
 */
void sched_reposition_task(struct task *task)
{
    if (!task) {
        return;
    }

    for (int attempts = 0; attempts < 8; attempts++) {
        uint32_t cpu = task->assigned_cpu;
        if (cpu >= cpu_count) {
            return;  /* Defensive: corrupt assigned_cpu — bail. */
        }
        irq_flags_t flags = rq_lock_irqsave(cpu);

        if (task->assigned_cpu != cpu) {
            /* Moved between read and lock; release and try again. */
            rq_unlock_irqrestore(cpu, flags);
            continue;
        }

        bool moved = reposition_locked(task, cpu);
        rq_unlock_irqrestore(cpu, flags);
        if (moved) {
            smp_notify_cpu(cpu);
        }
        return;
    }

    WARN("sched_reposition_task: gave up after 8 retries on task '%s'",
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

        int st_removed = remove_from_cpu_queue_locked(task, cpu);
        SLOT_TRACE(ST_TERMINATE, task, cpu, st_removed, 0);
        if (st_removed) {
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
     * once stealing started succeeding reliably. Serialized by
     * steal_deque_lock[cpu]. */
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
     * task) will run it. */
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
uint32_t sched_steal_trace_arm(void)
{
    /* Single caller (the bench, on CPU 0); the release orders the
     * caller's earlier stores before the new run number. Thieves only
     * compare the number against their own copy. */
    uint32_t run = steal_trace_run + 1;
    if (run == 0) {
        run = 1;
    }
    /* #1216 bin bounds in counter ticks, and the window-log origin. */
    static const uint32_t ph_us[STEAL_PH_BINS - 1] = {
        10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
    static const uint32_t woff_us[STEAL_WOFF_BINS - 1] = {
        1, 2, 4, 8, 16, 32, 64 };
    uint64_t freq = timer_get_frequency();
    for (uint32_t i = 0; i < STEAL_PH_BINS - 1; i++) {
        steal_ph_bound[i] = (uint64_t)ph_us[i] * freq / 1000000ULL;
    }
    for (uint32_t i = 0; i < STEAL_WOFF_BINS - 1; i++) {
        steal_woff_bound[i] = (uint64_t)woff_us[i] * freq / 1000000ULL;
    }
    steal_trace_t0_cnt = timer_get_count();
    __atomic_store_n(&steal_trace_run, run, __ATOMIC_RELEASE);
    return run;
}

uint32_t sched_steal_trace_watch(void)
{
    return STEAL_TRACE_WATCH;
}

uint64_t sched_steal_trace_t0(void)
{
    return steal_trace_t0_cnt;
}

const struct steal_pdwin_cpu *sched_steal_pdwin(uint32_t cpu)
{
    return (cpu < MAX_CPUS) ? &steal_pdwin[cpu] : NULL;
}

static inline uint32_t steal_bin(const uint64_t *bound, uint32_t nbins,
                                 uint64_t d)
{
    uint32_t i = 0;
    while (i < nbins - 1 && d >= bound[i]) {
        i++;
    }
    return i;
}

/* Reset this CPU's per-run window fields at its first event of a new
 * run. The open window, its entry time and last_tick carry over. */
static inline void steal_pdwin_roll(struct steal_pdwin_cpu *w)
{
    uint32_t run = steal_trace_run;
    if (w->run == run) {
        return;
    }
    w->run = run;
    w->first_tick = 0;
    w->tick_n = 0;
    for (uint32_t r = 0; r < STEAL_WIN_NREASON; r++) {
        w->n[r] = 0;
        w->sum[r] = 0;
        w->max[r] = 0;
    }
    for (uint32_t b = 0; b < STEAL_PH_BINS; b++) {
        w->wph[b] = 0;
    }
    w->nlog = 0;
}

/* Called on `cpu` right after it set preempt_disabled[cpu]. A set
 * while a window is open (a nested set) is a no-op. */
void steal_pdwin_set(uint32_t cpu, uint32_t reason)
{
    if (cpu >= MAX_CPUS) {
        return;
    }
    struct steal_pdwin_cpu *w = &steal_pdwin[cpu];
    steal_pdwin_roll(w);
    if (w->win_open) {
        return;
    }
    uint64_t now = timer_get_count();
    w->cur_entry = now;
    w->cur_reason = reason;
    uint64_t lt = w->last_tick;
    if (lt != 0 && now >= lt) {
        w->wph[steal_bin(steal_ph_bound, STEAL_PH_BINS, now - lt)]++;
    }
    w->win_open = 1;
}

/* schedule()'s own set: 'P' if someone pushed onto this CPU's deque
 * since its last window closed, else 'O'. Inside a trampoline window
 * this is a nested set and keeps 'T'. */
void steal_pdwin_set_sched(uint32_t cpu)
{
    if (cpu >= MAX_CPUS) {
        return;
    }
    uint32_t pushed = steal_trace_dq[cpu].pushes !=
                      steal_pdwin[cpu].pushes_at_close;
    steal_pdwin_set(cpu, pushed ? STEAL_WIN_PUSH : STEAL_WIN_OTHER);
}

/* Called on `cpu` right after it cleared preempt_disabled[cpu]. */
void steal_pdwin_clear(uint32_t cpu)
{
    if (cpu >= MAX_CPUS) {
        return;
    }
    struct steal_pdwin_cpu *w = &steal_pdwin[cpu];
    steal_pdwin_roll(w);
    if (!w->win_open) {
        return;
    }
    uint64_t now = timer_get_count();
    uint64_t dur = now - w->cur_entry;
    uint32_t r = w->cur_reason;
    if (r < STEAL_WIN_NREASON) {
        w->n[r]++;
        w->sum[r] += dur;
        uint32_t d32 = dur > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)dur;
        if (d32 > w->max[r]) {
            w->max[r] = d32;
        }
    }
    uint32_t k = w->nlog;
    if (k < STEAL_WIN_LOG_LEN) {
        w->log_entry[k] = (int32_t)(int64_t)(w->cur_entry -
                                             steal_trace_t0_cnt);
        w->log_dur[k] = dur > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)dur;
        w->log_reason[k] = (uint8_t)r;
        w->nlog = k + 1;
    }
    w->pushes_at_close = steal_trace_dq[cpu].pushes;
    w->win_open = 0;
}

/* Hardware timer tick on `cpu`, from sched_note_hw_tick() in the timer
 * ISR (the coop path's synthetic ticks are not counted). */
void steal_pdwin_tick(uint32_t cpu)
{
    if (cpu >= MAX_CPUS) {
        return;
    }
    struct steal_pdwin_cpu *w = &steal_pdwin[cpu];
    steal_pdwin_roll(w);
    uint64_t now = timer_get_count();
    w->last_tick = now;
    if (w->first_tick == 0) {
        w->first_tick = now;
    }
    w->tick_n++;
}

void steal_pdwin_set_tick_asm(uint32_t cpu)
{
    steal_pdwin_set(cpu, STEAL_WIN_TICK);
}

void steal_pdwin_clear_asm(uint32_t cpu)
{
    steal_pdwin_clear(cpu);
}

const struct steal_trace_cpu *sched_steal_trace_cpu(uint32_t cpu)
{
    return (cpu < MAX_CPUS) ? &steal_trace[cpu] : NULL;
}

const struct steal_trace_dq *sched_steal_trace_dq(uint32_t cpu)
{
    return (cpu < MAX_CPUS) ? &steal_trace_dq[cpu] : NULL;
}

/* Record the outcome of one sched_try_steal() call on this CPU (#1203).
 * Plain stores to this CPU's own trace block only. */
static inline void steal_trace_note(struct steal_trace_cpu *tr,
                                    uint64_t now, uint8_t out)
{
    switch (out) {
    case STEAL_OUT_OK:    tr->ok++;      break;
    case STEAL_OUT_ISO:   tr->iso++;     break;
    case STEAL_OUT_STALE: tr->f_stale++; break;
    case STEAL_OUT_PD:    tr->f_pd++;    break;
    case STEAL_OUT_EMPTY: tr->f_empty++; break;
    default:              tr->f_other++; break;
    }
    if (out == STEAL_OUT_OK && tr->first_ok == 0) {
        tr->first_ok = now;
    }
    uint32_t n = tr->nlog;
    if (n < STEAL_TRACE_LOG_LEN) {
        tr->log_dt[n] = (uint32_t)(now - tr->first_att);
        tr->log_out[n] = out;
        tr->nlog = n + 1;
    }
}

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
 *
 * #1203 instrumentation: every exit records an outcome in
 * steal_trace[this_cpu] (see steal_trace.h). It adds per-CPU counter
 * stores, one CNTPCT read per call, one plain load of the victim's
 * lock word before each spin_lock_irqsave, two loads of the deque
 * indices under the lock and, on a preempt_disabled skip, an unlocked
 * load of the victim deque's top and bottom. No locks, no atomics and
 * no prints; every branch the steal takes is unchanged.
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

    struct steal_trace_cpu *tr = &steal_trace[this_cpu];
    uint64_t now = timer_get_count();
    uint32_t run = steal_trace_run;
    if (tr->run != run) {
        tr->run = run;
#if defined(SPINLOCK_SKIP_LOCKING)
        tr->lk_hw = 2;
#else
        tr->lk_hw = (uint32_t)spinlock_hw_enabled;
#endif
        tr->first_att = now;
        tr->first_ok = 0;
        tr->nlog = 0;
        for (uint32_t r = 0; r < STEAL_WIN_NREASON; r++) {
            tr->w_in[r] = 0;
        }
        tr->w_in_unk = 0;
        for (uint32_t b = 0; b < STEAL_WOFF_BINS; b++) {
            tr->w_off[b] = 0;
        }
        for (uint32_t b = 0; b < STEAL_PH_BINS; b++) {
            tr->ph_w[b] = 0;
            tr->ph_w_pd[b] = 0;
            tr->ph_own[b] = 0;
        }
    }
    tr->att++;
    tr->last_att = now;

    /* #1216: where this attempt falls against the watched CPU's tick
     * and preempt_disabled window, and against this CPU's own tick.
     * Plain loads of the watched CPU's flag and trace block; diagnostic
     * only, never acted on. */
    uint32_t ph_w_bin = STEAL_PH_BINS;   /* none: watched CPU not ticked */
    {
        uint32_t wc = STEAL_TRACE_WATCH;
        if (wc < cpu_count && wc != this_cpu) {
            const struct steal_pdwin_cpu *w = &steal_pdwin[wc];
            uint64_t wt = w->last_tick;
            if (wt != 0 && now >= wt) {
                ph_w_bin = steal_bin(steal_ph_bound, STEAL_PH_BINS, now - wt);
                tr->ph_w[ph_w_bin]++;
            }
            if (preempt_disabled[wc]) {
                if (w->win_open) {
                    uint32_t r = w->cur_reason;
                    if (r < STEAL_WIN_NREASON) {
                        tr->w_in[r]++;
                    }
                    uint64_t e = w->cur_entry;
                    if (now >= e) {
                        tr->w_off[steal_bin(steal_woff_bound,
                                            STEAL_WOFF_BINS, now - e)]++;
                    }
                } else {
                    tr->w_in_unk++;
                }
            }
        }
        uint64_t ot = steal_pdwin[this_cpu].last_tick;
        if (ot != 0 && now >= ot) {
            tr->ph_own[steal_bin(steal_ph_bound, STEAL_PH_BINS, now - ot)]++;
        }
    }

    /* Isolated cores must never pull work from other CPUs — that's
     * the whole point of isolation. The S5 placement override
     * already refuses to *target* an isolated CPU; this closes the
     * symmetric hole where an isolated CPU would steal from a
     * non-isolated neighbour and then run its idle loop while
     * holding a general-purpose task that was explicitly routed
     * away from it (test_proactive_load_balance_respects_isolation
     * would otherwise flake when CPU 2 or 3 acted as a thief). */
    if (sched.isolated_cores & (1U << this_cpu)) {
        steal_trace_note(tr, now, STEAL_OUT_ISO);
        return NULL;
    }

    uint32_t probed = 0;      /* victims whose deque was locked and read */
    uint32_t n_empty = 0;     /* ... of which empty                      */
    uint32_t n_stale = 0;     /* entries popped and rejected             */
    uint32_t n_pd = 0;        /* victims skipped on preempt_disabled     */

    for (uint32_t i = 1; i < cpu_count; i++) {
        uint32_t victim = (this_cpu + i) % cpu_count;
        if (victim == this_cpu) {
            continue;
        }

        /* Cheap early-out: if the victim is currently inside its own
         * schedule() critical section (between rq_unlock_irqrestore
         * and switch_to — see preempt_disabled flag at sched.c:~1040),
         * don't bother contending the lock it's almost certainly
         * about to take again. The victim's rq_lock (acquired below)
         * is still the primary mutual-exclusion mechanism that keeps
         * the steal race-free; this is belt-and-suspenders per the
         * cross-plan review (B3 / P2-3). */
        if (preempt_disabled[victim]) {
            SLOT_TRACE(ST_STEAL_PD, NULL, victim, 1, 0);
            n_pd++;
            tr->v_pd++;
            /* Unlocked snapshot: did the skipped victim look like it
             * had work? Diagnostic only, never acted on. */
            if (!steal_deque_is_empty(&cpu_steal_deques[victim])) {
                tr->v_pd_work++;
            }
            continue;
        }

        /* Drain any stale pointers along with finding a live one. Bounded
         * by deque capacity so this loop cannot spin forever. */
        int victim_had_entry = 0;
        probed++;
        for (uint32_t probe = 0; probe < STEAL_DEQUE_CAPACITY; probe++) {
            uint32_t captured_gen = 0;
            /* #1203: was the lock held when this thief arrived? Plain
             * load; the acquisition below is unchanged. */
            if (steal_deque_lock[victim].lock != 0) {
                tr->v_busy++;
            }
            /* Serialize this steal against owner's pushes / other
             * thieves' steals on the same victim's deque. Held
             * briefly (for the pop) and released before we take
             * rq_lock[victim] — the two locks are never held
             * together so no ordering constraint exists between
             * them. */
            irq_flags_t sd_flags =
                spin_lock_irqsave(&steal_deque_lock[victim]);
            int was_nonempty =
                !steal_deque_is_empty(&cpu_steal_deques[victim]);
            struct task *candidate =
                steal_deque_steal(&cpu_steal_deques[victim], &captured_gen);
            if (was_nonempty &&
                steal_deque_is_empty(&cpu_steal_deques[victim])) {
                /* #1203: this steal took the last entry (or skipped the
                 * last NULL slots): the deque was last non-empty now. */
                steal_trace_dq[victim].drains++;
                steal_trace_dq[victim].drain_ts = now;
                steal_trace_dq[victim].drain_run = run;
            }
            spin_unlock_irqrestore(&steal_deque_lock[victim], sd_flags);
            if (!candidate) {
                break;  /* Empty — try next victim */
            }
            victim_had_entry = 1;

            irq_flags_t flags = rq_lock_irqsave(victim);

            /* Generation check (#139) closes the ABA window: if this
             * deque slot captured a previous life of the task_table
             * slot (same pointer, new logical task), the live
             * task->generation differs and we must discard even
             * though state/affinity/assigned_cpu may all read as
             * valid for the current resident. Checked first so we
             * don't bother with the full structural validation for
             * an entry we already know is stale.
             *
             * #1203: the conjunction is unrolled so the trace can name
             * the first clause that failed. Evaluation order and the
             * short-circuit (remove only when every check before it
             * held) match the original single && expression. */
            int stealable = 0;
            if (candidate->generation != captured_gen) {
                tr->v_gen++;
            } else if (candidate->state != TASK_READY) {
                tr->v_state++;
            } else if (candidate->assigned_cpu != victim) {
                tr->v_cpu++;
            } else if (candidate->cpu_affinity != CPU_AFFINITY_ANY) {
                tr->v_aff++;
            } else if (!remove_from_cpu_queue_locked(candidate, victim)) {
                tr->v_rm++;
            } else {
                stealable = 1;
            }

            SLOT_TRACE(ST_STEAL_TRY, candidate, victim, preempt_disabled[victim],
                       (stealable ? 1 :
                        candidate->generation != captured_gen ? 2 :
                        candidate->state != TASK_READY ? 3 :
                        candidate->assigned_cpu != victim ? 4 :
                        candidate->cpu_affinity != CPU_AFFINITY_ANY ? 5 : 6) |
                       ((uint64_t)(task_current_on_cpu(victim) == candidate) << 8));
            rq_unlock_irqrestore(victim, flags);

            if (stealable) {
                sched_diag_steal_successes[this_cpu]++;
                steal_trace_note(tr, now, STEAL_OUT_OK);
                return candidate;
            }
            /* Otherwise: stale pointer (structural mismatch or
             * generation mismatch), already popped from deque. Loop
             * again to drain another entry on the same victim. */
            sched_diag_steal_stale[this_cpu]++;
            n_stale++;
        }
        if (!victim_had_entry) {
            sched_diag_steal_empty_victim[this_cpu]++;
            tr->v_empty++;
            n_empty++;
        }
    }

    uint8_t out;
    if (n_stale > 0) {
        out = STEAL_OUT_STALE;
    } else if (n_pd > 0) {
        out = STEAL_OUT_PD;
        if (ph_w_bin < STEAL_PH_BINS) {
            tr->ph_w_pd[ph_w_bin]++;
        }
    } else if (probed > 0 && n_empty == probed) {
        out = STEAL_OUT_EMPTY;
    } else {
        out = STEAL_OUT_OTHER;
    }
    steal_trace_note(tr, now, out);
    return NULL;
}
#else
/* #1216 window hooks: no trace without work stealing. */
void steal_pdwin_set(uint32_t cpu, uint32_t reason) { (void)cpu; (void)reason; }
void steal_pdwin_set_sched(uint32_t cpu) { (void)cpu; }
void steal_pdwin_clear(uint32_t cpu) { (void)cpu; }
void steal_pdwin_tick(uint32_t cpu) { (void)cpu; }
void steal_pdwin_set_tick_asm(uint32_t cpu) { (void)cpu; }
void steal_pdwin_clear_asm(uint32_t cpu) { (void)cpu; }
#endif /* CONFIG_WORK_STEALING */

/*
 * Per-CPU CNTPCT value of the most recent scheduler tick, from either
 * source: the hardware timer ISR (sched_note_hw_tick) or the
 * cooperative tick synthesized in schedule() (coop_preempt_maybe_tick).
 *
 * 64-byte alignment keeps the array off cache lines shared with other
 * data. Each CPU only reads/writes its own index. volatile prevents
 * the compiler from holding stale values across the coop_preempt tick
 * computation.
 *
 * Compiled on every platform so the hardware-tick bookkeeping can be
 * tested on QEMU, where COOP_PREEMPT is never built.
 */
static volatile uint64_t coop_last_tick_cntpct[MAX_CPUS]
    __attribute__((aligned(64)));

/*
 * Record that the hardware timer just ticked on `cpu` (#1047).
 *
 * Builds with both a hardware tick and COOP_PREEMPT (Jetson with
 * JETSON_HW_TICK=ON SECONDARY_PREEMPT=OFF, the default until #1050) would
 * otherwise count each 10 ms period twice: once in the timer ISR and
 * again when schedule() finds the cooperative deadline expired. Once
 * #1047 unmasked IRQs at task entry, Jetson CPU 0 on that build showed
 * exactly twice the ticks of CPUs 1-5. Resetting the cooperative deadline
 * here means the synthetic tick only fires when the hardware tick
 * has been missing for a full period.
 */
void sched_note_hw_tick(uint32_t cpu)
{
    if (cpu < MAX_CPUS) {
        coop_last_tick_cntpct[cpu] = timer_get_count();
    }
    steal_pdwin_tick(cpu);   /* #1216 trace: no-op without work stealing */
}

uint64_t sched_last_tick_cntpct(uint32_t cpu)
{
    return (cpu < MAX_CPUS) ? coop_last_tick_cntpct[cpu] : 0;
}

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
/* timer_handler_count and pit_ticks are declared in timer.h. */

void scheduler_tick(void);

static inline void coop_preempt_maybe_tick(uint32_t cpu)
{
    uint64_t freq = timer_get_frequency();
    if (freq == 0) {
        return;
    }
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
    if (now - last < period) {
        return;
    }

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
    steal_pdwin_set(cpu, STEAL_WIN_COOP);           /* #1216 trace */
    scheduler_tick();
    preempt_disabled[cpu] = prev_preempt_disabled;
    if (!prev_preempt_disabled) {
        steal_pdwin_clear(cpu);                     /* #1216 trace */
    }
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
    if (cpu >= MAX_CPUS) {
        return;
    }

    slm_preempt_point_calls++;

    /* Skip when the scheduler is already mid-context-switch on this
     * CPU — recursing into schedule() through the preempt-point would
     * just re-enter the same code path. NOTE: this does NOT cover
     * spinlock-held callers; see the function header comment. */
    if (preempt_disabled[cpu]) {
        return;
    }

    uint64_t freq = timer_get_frequency();
    if (freq == 0) {
        return;
    }
    uint64_t period = freq / TIMER_HZ;
    uint64_t now = timer_get_count();
    uint64_t last = coop_last_tick_cntpct[cpu];

    /* Fast path: quantum hasn't elapsed. The `last == 0` arm covers
     * the bootstrap case before the first real tick — we want to
     * fall through into schedule() once on first call so the
     * deadline gets seeded by coop_preempt_maybe_tick. */
    if (last != 0 && (now - last) < period) {
        return;
    }

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
        uint32_t zombie_gen = rq->zombie_gen;
        rq->zombie = NULL;
        SLOT_TRACE(ST_ZOMBIE_REAP, zombie, this_cpu, task_current(), zombie_gen);
        rq_unlock_irqrestore(this_cpu, flags);

        /* Destroy outside lock - task_destroy may call pmm.
         *
         * Only if the slot still holds the task recorded below (#1213):
         * the task's owner may already have destroyed it (it saw
         * TERMINATED), and task_create may already have handed the slot
         * to a new task. Before this check the reaper destroyed that new
         * task mid-construction: its TERMINATED check still saw the old
         * occupant's state, it cleared stack_top, and task_create copied
         * the cleared value into context.sp (SP 0 on the next switch). */
        (void)task_destroy_gen(zombie, zombie_gen);

        flags = rq_lock_irqsave(this_cpu);
    }

    struct task *current = task_current();
    struct task *next = pick_next_task(this_cpu);
    SLOT_TRACE_ONLY(uint32_t st_state0 = current ? (uint32_t)current->state : 0xFFFF;)

    /* If current task is still running and ready, re-add to queue */
    if (current && current->state == TASK_RUNNING) {
        current->state = TASK_READY;

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
        SLOT_TRACE_ONLY(
            if (st_state0 != TASK_RUNNING) {
                SLOT_TRACE(ST_PICK_SAME, current, st_state0, 0, 0);
            })
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

        /* NOTE: before #1047, tasks on Pi 5 and Jetson ran with DAIF.I=1
         * for life, so pit_ticks only advanced while idle ran. Tasks are
         * now unmasked at entry (task_entry_unmasks_irq), so hardware
         * ticks also land while tasks run. Components should still time
         * out against timer_get_count() rather than pit_ticks, which is a
         * global bumped by every CPU's tick and is not wall time on ARM. */
        return;
    }

    /* Perform context switch */

    SWITCH_CHECK_IN(next, this_cpu);
    next->state = TASK_RUNNING;
    next->on_cpu = 1;           /* cleared by sched_finish_switch (#1213) */
    next->switches++;
    sched.context_switches++;  /* Racy but acceptable for stats */

    /*
     * If current task is terminated, mark it as zombie for cleanup.
     * It will be destroyed on the next schedule() call after we've
     * safely switched to a different stack.
     */
    if (current && current->state == TASK_TERMINATED) {
        /* current->on_cpu is still 1 here, so no task_destroy can have
         * bumped its generation yet (#1213): this is the generation of
         * the occupant that is exiting. */
        rq->zombie = current;
        rq->zombie_gen = current->generation;
        SLOT_TRACE(ST_ZOMBIE_SET, current, this_cpu, 0, 0);
    }

    task_set_current(next);

    /* #195: record the context switch while the lock is still held
     * so `current` and `next` reflect the committed state. No-op
     * when tracing is disabled. */
    sched_trace_record_switch(this_cpu, current, next);

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
     * Placement: AFTER the task_set_current call is
     * correct because rq_lock_irqsave (above) has already disabled local
     * IRQs, so no timer can deliver between task_set_current and
     * preempt_disabled=1. (SCHED-M1 originally proposed moving this
     * earlier, but the race it targeted does not exist — rq_lock_irqsave
     * already closes the window.)
     */
    preempt_disabled[this_cpu] = 1;
    SLOT_TRACE(ST_PD_SET, next, this_cpu, cpu_id(), 0);
    steal_pdwin_set_sched(this_cpu);                /* #1216 trace */

    rq_unlock_irqrestore(this_cpu, flags);

    SWITCH_WINDOW_HOLD(current);    /* R1 test hook; empty unless SWITCH_WINDOW_TEST */

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

    SLOT_TRACE(ST_SWITCH, next, SLOT_TRACE_PACK(current), next->context.sp,
               next->context.x19);
    struct task *prev = switch_to(current, next);
    sched_finish_switch(prev);

#if !defined(PLATFORM_X86_64)
    SLOT_TRACE_ONLY({
        uint64_t st_sp;
        __asm__ volatile("mov %0, sp" : "=r"(st_sp));
        SLOT_TRACE(ST_SWITCH_RET, task_current(), st_sp, 0, 0);
    })
#endif

    /* Resumed on our stack after being switched back.
     * Re-enable preemption so timer ticks can trigger scheduling.
     *
     * Re-read the CPU (#1229): this task may have been stolen or migrated
     * while it was READY, so it can resume on a different CPU from the
     * one it called schedule() on. this_cpu is the pre-switch id; using it
     * cleared the old CPU's flag (possibly while that CPU was inside its
     * own switch window) and left this CPU's flag at 1, so its ticks
     * stopped preempting and thieves skipped it (#1219 deadlock). */
    uint32_t now_cpu = cpu_id();
    SLOT_TRACE(ST_PD_CLEAR, task_current(), now_cpu, now_cpu, this_cpu);
    preempt_disabled[now_cpu] = 0;
    steal_pdwin_clear(now_cpu);                     /* #1216 trace */
#if defined(SECONDARY_PREEMPT)
    /* Clear any pending reschedule flag set by scheduler_tick while
     * we were mid-switch — we just scheduled, so an immediate re-arm
     * after eret would be redundant. */
    reschedule_pending[now_cpu] = 0;
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
    struct cpu_runqueue *rq = cpu_rq(this_cpu);

    if (!sched.initialized) {
        panic("scheduler_start: scheduler not initialized");
    }

    if (this_cpu == 0) {
        INFO("CPU %u: Starting scheduler", this_cpu);
    }

    irq_flags_t flags = rq_lock_irqsave(this_cpu);

    /* Pick first task */
    struct task *first = pick_next_task(this_cpu);
    if (!first) {
        rq_unlock_irqrestore(this_cpu, flags);
        panic("scheduler_start: no tasks to run");
    }

    /* Remove from queue */
    if (first != rq->idle_task && rq->head == first) {
        rq->head = first->next;
        if (!rq->head) {
            rq->tail = NULL;
        }
        rq->ready_count--;
    }

    SWITCH_CHECK_IN(first, this_cpu);
    first->state = TASK_RUNNING;
    first->on_cpu = 1;          /* cleared by sched_finish_switch (#1213) */
    first->switches++;
    sched.context_switches++;  /* Racy but acceptable for stats */

    task_set_current(first);

    if (this_cpu == 0) {
        INFO("CPU %u: Switching to first task: '%s'", this_cpu, first->name);
    }

    rq_unlock_irqrestore(this_cpu, flags);

    /* Start timer and enable interrupts now that a task is active.
     * Must be done AFTER task_set_current() so that timer IRQ handler
     * can safely call task_current() in schedule(). */
    if (this_cpu == 0) {
        INFO("Starting timer (100 Hz)...");
    }
    timer_start();

    /* Guard against timer re-entrancy during switch_to.
     * The trampoline clears this after context restore. */
    preempt_disabled[this_cpu] = 1;
    steal_pdwin_set(this_cpu, STEAL_WIN_BOOT);      /* #1216 trace */

    /* Unmask IRQs. If timer fires, scheduler_tick() sees preempt_disabled=1
     * and skips schedule(). Ticks still count for sleep/uptime. */
    if (this_cpu == 0) {
        INFO("Enabling interrupts...");
    }

    /* Unmask IRQs.
     * With SECONDARY_PREEMPT: all CPUs enable here so the first timer
     * tick can arrive while switch_to runs (preempt_disabled is already
     * 1 to suppress re-entrant scheduling during the switch).
     * Without SECONDARY_PREEMPT, secondary CPUs must skip this because
     * a timer IRQ here would follow the broken direct-schedule-from-ISR
     * path. */
#if defined(PLATFORM_X86_64)
    __asm__ volatile("sti" ::: "memory");
#elif defined(PLATFORM_RASPI5) || defined(PLATFORM_JETSON_ORIN_NANO)
    /* Keyed on the platform, as in idle_task_func: the unmask rule is
     * about timer-IRQ delivery, not about where run queues live. */
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

    /* Debug: mark that we reached pre-switch point */
    sched_diag_idle_loops[this_cpu] = 0xAAAA;

    /* Switch to first task (NULL = no previous context to save).
     * switch_to never returns — it jumps to task_entry_wrapper.
     * The trampoline clears preempt_disabled[this_cpu]. */
    SLOT_TRACE(ST_SWITCH, first, SLOT_TRACE_PACK(NULL), first->context.sp,
               first->context.x19);
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
    sched_diag_tick[cpu]++;

#ifdef CONFIG_AI_SCHEDULER
    /* Per-CPU utilization tracking */
    {
        struct cpu_runqueue *rq = cpu_rq(cpu);
        rq->total_ticks++;
        struct task *cur = task_current();
        if (task_counts_as_running(cur)) {
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
    slot_trace_pd_tick(cpu, preempt_disabled[cpu]);
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
