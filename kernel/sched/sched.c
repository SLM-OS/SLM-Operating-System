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
#include "task.h"
#include "uart.h"
#include "debug.h"
#include "platform.h"
#include "smp.h"
#include "spinlock.h"
#include "slm_ffi.h"
#include "gic.h"
#include "timer.h"
#include "cache.h"
#include "ncmem.h"
#include <stdint.h>

/* Deadline boost thresholds (in nanoseconds) */
#define DEADLINE_CRITICAL_NS    (10 * 1000000ULL)   /* 10ms - boost to CRITICAL */
#define DEADLINE_HIGH_NS        (50 * 1000000ULL)   /* 50ms - boost to HIGH */
#define DEADLINE_BOOST_NS       (100 * 1000000ULL)  /* 100ms - boost +1 */

/* External functions from task.c */
extern void task_set_current(struct task *task);
extern void task_destroy(struct task *task);

/* Per-CPU run queue.
 * On Pi 5, the data fields live in NC memory for cross-CPU visibility,
 * but the lock stays in cacheable memory (ldaxr/stxr require cacheable). */
struct cpu_runqueue {
    struct task *head;
    struct task *tail;
    struct task *idle_task;
    struct task *zombie;
    uint32_t ready_count;
} __attribute__((aligned(CACHE_LINE_SIZE)));

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

/* Per-CPU run queue locks — always in cacheable memory.
 * Separated from cpu_runqueue because exclusive load/store (ldaxr/stxr)
 * used by spinlocks may not work on Non-Cacheable memory (BCM2712). */
static spinlock_t rq_lock[MAX_CPUS] __attribute__((aligned(CACHE_LINE_SIZE)));

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

/* Per-CPU diagnostic counters for cross-CPU dispatch debugging.
 * On Pi 5 these must be in NC memory for cross-CPU visibility —
 * secondary CPU writes to cacheable BSS are invisible to CPU 0. */
#if defined(PLATFORM_HAS_NC_MEMORY)
/* Allocated from NC region in scheduler_init for cross-CPU visibility */
volatile uint32_t *sched_diag_tick;
volatile uint32_t *sched_diag_schedule;
volatile uint32_t *sched_diag_picked;
volatile uint32_t *sched_diag_idle_loops;  /* idle task iteration count per CPU */
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
#endif

/* cpu_rq() implementation — must be after sched struct definition */
static inline struct cpu_runqueue *cpu_rq(uint32_t cpu)
{
#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC memory at compile-time-known address — no cacheable pointer.
     * Run queues are the first ncmem_alloc() in scheduler_init(). */
    return &((struct cpu_runqueue *)NC_MEM_BASE)[cpu];
#else
    return &sched.cpu_fallback[cpu];
#endif
}

/*
 * Idle task - runs when no other tasks are ready.
 * Each CPU has its own idle task.
 */
static void idle_task_func(void *arg)
{
    (void)arg;

    while (1) {
#if defined(PLATFORM_HAS_NC_MEMORY)
        /* Use fixed NC address — sched_diag_idle_loops pointer is in
         * cacheable BSS and may not be visible to secondary CPUs.
         * Pi 5: CPU index in Aff1 (bits[15:8]), QEMU: Aff0 (bits[7:0]). */
        {
            uint64_t mpidr;
            __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
            uint32_t hw_cpu = (mpidr & 0xFF) | ((mpidr >> 8) & 0xFF);
            (*(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + hw_cpu * 4))++;
        }
#else
        sched_diag_idle_loops[cpu_id()]++;
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
        /* Pi 5: WFE for cooperative scheduling.
         * Timer IRQs on secondary CPUs cause an exception handler hang
         * (under investigation) so we skip daifclr here.
         * WFE wakes on SEV from other CPUs (sent by scheduler_add_task_to_cpu
         * and spin_unlock). CPU sleeps until work is dispatched. */
        __asm__ volatile("wfe" ::: "memory");
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

    if (now >= task->deadline_ns) {
        /* Deadline missed! Boost to critical to finish ASAP */
        boosted = TASK_PRIORITY_CRITICAL;
    } else {
        uint64_t remaining = task->deadline_ns - now;

        if (remaining < DEADLINE_CRITICAL_NS) {
            /* < 10ms: boost to CRITICAL (7) */
            boosted = TASK_PRIORITY_CRITICAL;
        } else if (remaining < DEADLINE_HIGH_NS) {
            /* < 50ms: boost to HIGH (6) */
            boosted = TASK_PRIORITY_HIGH;
        } else if (remaining < DEADLINE_BOOST_NS) {
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
    /* Initialize NC run queue region. cpu_rq() uses NC_MEM_BASE directly
     * (compile-time constant) so no cacheable pointer is needed. */
    ncmem_alloc(MAX_CPUS * sizeof(struct cpu_runqueue), CACHE_LINE_SIZE);
    INFO("SMP: run queues in NC memory at 0x%lx", (unsigned long)NC_MEM_BASE);
#endif

    /* Initialize task table (NC on Pi 5, BSS fallback otherwise) */
    extern void task_table_init(void);
    task_table_init();

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
#endif

    /* Initialize per-CPU run queues with per-queue locks */
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        spin_init(&rq_lock[i]);
        cpu_rq(i)->head = NULL;
        cpu_rq(i)->tail = NULL;
        cpu_rq(i)->idle_task = NULL;
        cpu_rq(i)->zombie = NULL;
        cpu_rq(i)->ready_count = 0;
    }

    /* Create idle task for boot CPU (CPU 0) */
    cpu_rq(0)->idle_task = task_create("idle", idle_task_func, NULL);
    if (!cpu_rq(0)->idle_task) {
        panic("scheduler_init: failed to create idle task");
    }
    cpu_rq(0)->idle_task->state = TASK_READY;
    cpu_rq(0)->idle_task->cpu_affinity = 0;  /* Pinned to CPU 0 */
    cpu_rq(0)->idle_task->assigned_cpu = 0;

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
    cpu_rq(cpu)->idle_task = idle;

    rq_unlock_irqrestore(cpu, flags);

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xB2 = released rq_lock, function about to return */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + cpu * 4) = 0xB2;
#endif

    /* Skip INFO print on secondary CPUs — uart_lock contention may hang */
    if (cpu == 0)
        INFO("CPU %u: scheduler initialized", cpu);
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

#if !defined(PLATFORM_X86_64)
    /* Wake idle CPUs so they can pick up the new task.
     * SEV wakes any CPU in WFE (used by idle loop on NC platforms). */
    __asm__ volatile("sev" ::: "memory");
#endif
}

/*
 * Calculate deadline pressure for a CPU's run queue.
 *
 * Returns a score based on the sum of urgency of deadline-constrained tasks.
 * Higher score = more deadline pressure = avoid placing more work here.
 *
 * Urgency scoring:
 *   - No deadline: 0 points
 *   - Deadline > 100ms: 1 point
 *   - Deadline 50-100ms: 2 points
 *   - Deadline 10-50ms: 4 points
 *   - Deadline < 10ms or missed: 8 points
 */
static uint32_t calculate_deadline_pressure(uint32_t cpu)
{
    struct cpu_runqueue *rq = cpu_rq(cpu);
    uint32_t pressure = 0;
    uint64_t now = slm_get_time_ns();

    struct task *t = rq->head;
    while (t) {
        if (t->deadline_ns > 0) {
            if (now >= t->deadline_ns) {
                /* Deadline missed - very high pressure */
                pressure += 8;
            } else {
                uint64_t remaining = t->deadline_ns - now;
                if (remaining < DEADLINE_CRITICAL_NS) {
                    pressure += 8;  /* < 10ms */
                } else if (remaining < DEADLINE_HIGH_NS) {
                    pressure += 4;  /* < 50ms */
                } else if (remaining < DEADLINE_BOOST_NS) {
                    pressure += 2;  /* < 100ms */
                } else {
                    pressure += 1;  /* distant deadline */
                }
            }
        }
        t = t->next;
    }

    return pressure;
}

/*
 * Find a non-isolated CPU for a task with CPU_AFFINITY_ANY.
 *
 * Uses a combined metric: ready_count + deadline_pressure.
 * This spreads deadline-constrained tasks across cores to reduce
 * the chance of missing deadlines due to queue contention.
 *
 * Returns CPU 0 as fallback (CPU 0 cannot be isolated).
 */
static uint32_t find_target_cpu(void)
{
    /* Round-robin starting point to spread tasks across CPUs.
     * Without this, ties (all CPUs at score 0) always go to CPU 0. */
    static uint32_t rr_next;
    uint32_t start = rr_next % cpu_count;

    uint32_t best_cpu = start;
    uint32_t best_score = cpu_rq(start)->ready_count + calculate_deadline_pressure(start);

    for (uint32_t i = 1; i < cpu_count; i++) {
        uint32_t cpu = (start + i) % cpu_count;

        /* Skip isolated cores */
        if (sched.isolated_cores & (1U << cpu))
            continue;

        uint32_t score = cpu_rq(cpu)->ready_count + calculate_deadline_pressure(cpu);

        if (score < best_score) {
            best_cpu = cpu;
            best_score = score;
        }
    }

    rr_next = best_cpu + 1;
    return best_cpu;
}

/*
 * Find a "performance" core for deadline-critical tasks.
 *
 * In a big.LITTLE system, this would return a big core.
 * In QEMU virt (homogeneous), we use CPU 1+ as "performance" cores
 * to keep CPU 0 available for system tasks.
 *
 * Returns the least-loaded non-isolated CPU > 0, or falls back to find_target_cpu().
 */
static uint32_t find_performance_cpu(void)
{
    if (cpu_count <= 1) {
        return 0;  /* Only one CPU available */
    }

    uint32_t best_cpu = 0;
    uint32_t best_score = UINT32_MAX;
    int found_perf_core = 0;

    /* Prefer CPUs > 0 for performance tasks */
    for (uint32_t cpu = 1; cpu < cpu_count; cpu++) {
        /* Skip isolated cores - they're manually managed */
        if (sched.isolated_cores & (1U << cpu)) {
            continue;
        }

        uint32_t score = cpu_rq(cpu)->ready_count + calculate_deadline_pressure(cpu);
        if (score < best_score) {
            best_cpu = cpu;
            best_score = score;
            found_perf_core = 1;
        }
    }

    /* Fall back to any CPU if no performance core available */
    if (!found_perf_core) {
        return find_target_cpu();
    }

    return best_cpu;
}

/*
 * Add a task to the run queue (assigns to a CPU based on affinity).
 *
 * Policy for deadline-constrained tasks:
 *   - Tasks with deadline_ns > 0 are placed on "performance" cores (CPU > 0)
 *   - This keeps CPU 0 available for system tasks and reduces interference
 *   - On big.LITTLE hardware, this would route to big cores
 */
void scheduler_add_task(struct task *task)
{
    if (!task || task->state != TASK_READY) {
        return;
    }

    uint32_t target_cpu;

    /* Affinity may have been set by another CPU */
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_invalidate(&task->cpu_affinity);
#endif

    if (task->cpu_affinity != CPU_AFFINITY_ANY) {
        target_cpu = task->cpu_affinity;
    } else if (task->deadline_ns > 0) {
        target_cpu = find_performance_cpu();
        DEBUG_PRINT("Deadline task '%s' -> CPU %u (performance core)",
                    task->name, target_cpu);
    } else {
        target_cpu = find_target_cpu();
    }

    scheduler_add_task_to_cpu(task, target_cpu);
}

/*
 * Remove a task from the run queue.
 */
void scheduler_remove_task(struct task *task)
{
    if (!task) {
        return;
    }

    uint32_t cpu = task->assigned_cpu;
    struct cpu_runqueue *rq __attribute__((unused)) = cpu_rq(cpu);
    irq_flags_t flags = rq_lock_irqsave(cpu);

    /*
     * Task might already have been removed from the queue when it
     * started running (schedule() removes from queue before switching).
     * Only decrement task_count if task was actually in the queue.
     */
    if (remove_from_cpu_queue_locked(task, cpu)) {
        sched.task_count--;  /* Racy but acceptable for stats */
        DEBUG_PRINT("Removed task '%s' from CPU %u run queue (ready=%u)",
                    task->name, cpu, rq->ready_count);
    }

    rq_unlock_irqrestore(cpu, flags);
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

    /* Remove from old CPU queue if task is ready */
    if (task->state == TASK_READY) {
        remove_from_cpu_queue_locked(task, old_cpu);
        add_to_cpu_queue_locked(task, target_cpu);
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

    /* Unlock in reverse order */
    if (old_cpu < target_cpu) {
        spin_unlock(&rq_lock[target_cpu]);
        rq_unlock_irqrestore(old_cpu, flags);
    } else {
        spin_unlock(&rq_lock[old_cpu]);
        rq_unlock_irqrestore(target_cpu, flags);
    }

    return 0;
}

/*
 * Pick the next task to run on this CPU.
 */
static struct task *pick_next_task(uint32_t cpu)
{
    struct cpu_runqueue *rq = cpu_rq(cpu);

    if (rq->head) {
        sched_diag_picked[cpu]++;
        return rq->head;
    }

    /* No ready tasks - run idle task */
    return rq->idle_task;
}

/*
 * Schedule - select next task and switch to it (per-CPU).
 */
void schedule(void)
{
    uint32_t this_cpu = cpu_id();
    struct cpu_runqueue *rq = cpu_rq(this_cpu);
    sched_diag_schedule[this_cpu]++;

    /* On non-NC platforms, invalidate cached copy of run queue before reading.
     * On Pi 5 with NC run queues, this is a no-op (NC data not cached). */
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_invalidate_range(rq, sizeof(*rq));
#endif

    irq_flags_t flags = rq_lock_irqsave(this_cpu);

    /*
     * Clean up zombie task from previous schedule cycle.
     * This is safe because we've already switched away from it.
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
        if (current && current->state == TASK_TERMINATED) {
            /*
             * This should never happen: terminated task picked as next.
             * The terminated task should not be in the run queue, and
             * pick_next_task should return idle_task if queue is empty.
             */
            rq_unlock_irqrestore(this_cpu, flags);
            panic("schedule: terminated task selected as next (CPU %u, task '%s')",
                  this_cpu, current->name);
        }
        if (current) {
            current->state = TASK_RUNNING;
        }
        rq_unlock_irqrestore(this_cpu, flags);
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
     * set to `next` at line 846, but we're still on `current`'s stack).
     *
     * The preempt_disabled flag tells scheduler_tick() to skip the
     * schedule() call during this window. The timer still fires and
     * increments tick counters — it just doesn't try to context-switch.
     *
     * After switch_to returns (on the resumed task's stack), we clear
     * the flag so preemption resumes normally.
     */
    preempt_disabled[this_cpu] = 1;

    rq_unlock_irqrestore(this_cpu, flags);

    switch_to(current, next);

    /* Resumed on our stack after being switched back.
     * Re-enable preemption so timer ticks can trigger scheduling. */
    preempt_disabled[this_cpu] = 0;
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

    if (this_cpu == 0)
        INFO("CPU %u: Starting scheduler", this_cpu);

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

    if (this_cpu == 0)
        INFO("CPU %u: Switching to first task: '%s'", this_cpu, first->name);

    rq_unlock_irqrestore(this_cpu, flags);

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xC4 = unlocked, about to start timer */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + this_cpu * 4) = 0xC4;
#endif

    /* Start timer and enable interrupts now that a task is active.
     * Must be done AFTER task_set_current() so that timer IRQ handler
     * can safely call task_current() in schedule(). */
    if (this_cpu == 0)
        INFO("Starting timer (100 Hz)...");
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
    if (this_cpu == 0)
        INFO("Enabling interrupts...");

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC trace: 0xC6 = pre-daifclr */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + this_cpu * 4) = 0xC6;
#endif

    /* Unmask IRQs.
     * On Pi 5 secondary CPUs, skip daifclr for now — the idle task's
     * while loop does its own daifclr + wfi. Enabling interrupts here
     * on the boot stack triggers an exception path that hangs (under
     * investigation). CPU 0 enables interrupts normally. */
#if defined(PLATFORM_X86_64)
    __asm__ volatile("sti" ::: "memory");
#elif defined(PLATFORM_HAS_NC_MEMORY)
    if (this_cpu == 0) {
        __asm__ volatile("msr daifclr, #0x2" ::: "memory");
        __asm__ volatile("isb" ::: "memory");
    }
    /* Secondary CPUs: idle_task_func does daifclr in its loop */
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

    /* Skip preemption if a context switch is in progress on this CPU.
     * schedule() sets preempt_disabled between rq_unlock and switch_to
     * completion. Calling schedule() here would corrupt state because
     * task_set_current(next) has already been called but the actual
     * stack switch hasn't happened yet. */
    if (preempt_disabled[cpu])
        return;

    /* Preempt current task */
    schedule();
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
            target = find_target_cpu();
        } else {
            target = cpu;
        }

        if (target != task->assigned_cpu) {
            sched_migrate_task(task, target);
        }
    }

    return 0;
}
