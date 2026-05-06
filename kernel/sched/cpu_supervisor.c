/*
 * cpu_supervisor.c — Tier 2 of #216 (secondary-CPU dormancy recovery).
 *
 * Tier 1 (PR #290) made handle_page_fault on Pi 5 power-gate the
 * faulting CPU via psci_cpu_off() instead of wedging it in a
 * `while(1) wfi` loop. That kept the rest of the suite running, but
 * left the dead CPU off the bus permanently — a CPU 1 fault during
 * test_isolated_core_latency cost the system 25 % of its compute
 * for the rest of the boot.
 *
 * Tier 2 (this file) closes that loop. A supervisor task on CPU 0
 * samples sched_diag_idle_loops[] every second; if a secondary's
 * counter is frozen for SUPERVISOR_FROZEN_THRESHOLD samples the
 * supervisor:
 *
 *   1. Marks the CPU offline so the work-stealing path stops
 *      considering it a steal target.
 *   2. Resets the per-CPU run queue head/tail/counters so the
 *      resurrected CPU starts with a clean queue. Tasks that were
 *      sitting on the dead CPU's queue are not migrated — they are
 *      treated as collateral damage of the fault that killed the
 *      CPU. (Auto-migration would require holding the fault-time
 *      task state, which we don't.)
 *   3. Calls psci_cpu_on(mpidr, secondary_entry, cpu) to bring the
 *      core back from EL3.
 *   4. Waits up to RESURRECT_TIMEOUT_US for cpu_boot_flag[cpu] to
 *      flip, then declares success.
 *
 * Diagnostics surface via the `cpu` shell command's existing
 * `dormancy` output and a new `cpu resurrect <N>` shell command for
 * manual control (e.g. driving from integration tests).
 *
 * Pi-5-only today — Jetson and QEMU haven't had their fault
 * recovery stories validated. Auto-resurrection is gated behind
 * `CPU_AUTO_RESURRECT` (CMake option, default ON for Pi 5). Tests
 * that need to observe dormancy without auto-recovery flip it OFF.
 */

#include "config.h"

#if defined(PLATFORM_RASPI5)

#include "smp.h"
#include "sched.h"
#include "task.h"
#include "timer.h"
#include "spinlock.h"
#include "debug.h"
#include "cache.h"
#include "cpu_supervisor.h"
#include <stdint.h>
#include <stdbool.h>

/* sched_diag_idle_loops is exposed by sched.c; same backing as the
 * `cpu` / per-test dormancy detector reads.
 * cpu_boot_flag and secondary_entry are declared in smp.h. */
extern volatile uint32_t *sched_diag_idle_loops;

/* PSCI return codes used here. psci_call returns the raw int; we
 * don't import the full enum. PSCI_ALREADY_ON is the discriminant
 * between "software wedge — drain alone is the recovery" and "real
 * PSCI failure — leave the CPU dead". */
#define PSCI_SUCCESS_LOCAL          0
#define PSCI_ALREADY_ON_LOCAL       (-4)

/* Sample interval and freeze threshold. SUPERVISOR_FROZEN_THRESHOLD
 * is the number of consecutive 1-second samples a CPU's idle counter
 * must be unchanged before we declare it dormant. 6 s gives the
 * existing per-test detector (3-sample stall × ~1 s/test) a chance
 * to fire first, and matches the boot-test cycle length so a real
 * boot regression doesn't get masked by aggressive auto-recovery. */
#define SUPERVISOR_TICK_MS          1000u
#define SUPERVISOR_FROZEN_THRESHOLD 6u
#define RESURRECT_TIMEOUT_US        5000000u   /* 5 s */
#define RESURRECT_POLL_US           100u

/* Halfway warning derives from THRESHOLD / 2; that integer-division
 * is only meaningful when THRESHOLD spans at least 2 samples. With
 * threshold=1 the supervisor would resurrect on the first frozen
 * sample (no halfway distinction), and threshold=0 would resurrect
 * unconditionally — both nonsensical. Catch a future tweak that
 * accidentally lowers this. */
_Static_assert(SUPERVISOR_FROZEN_THRESHOLD >= 2,
    "SUPERVISOR_FROZEN_THRESHOLD must be >= 2 for the halfway warning math");

/* Per-CPU dormancy state. Plain volatile (no atomic) because writers
 * are single (the supervisor task on CPU 0); cross-CPU readers are
 * informational (the `cpu` shell command). */
static volatile uint32_t last_idle_loops[MAX_CPUS];
static volatile uint32_t frozen_samples[MAX_CPUS];
static volatile uint64_t resurrect_attempts[MAX_CPUS];
static volatile uint64_t resurrect_successes[MAX_CPUS];
static volatile uint64_t dormancy_warnings;

/*
 * Wait up to `timeout_us` microseconds for `cpu_boot_flag[cpu]` to
 * become non-zero. Returns the elapsed microseconds when the flag
 * was observed, or `timeout_us` if the wait expired.
 *
 * Pi 5 / Jetson per-core L2 caches are incoherent, so we try BOTH
 * visibility paths each iteration:
 *   1. LDAR (atomic load-acquire) — works when caches happen to be
 *      coherent.
 *   2. DC IVAC + plain read — forces the local L2 line to be
 *      invalidated and re-fetched from the PoC where the secondary's
 *      STLR has been pushed via cache_clean.
 * Mirrors the pattern in kernel/sched/smp.c around line 459.
 */
static uint32_t wait_for_boot_flag(uint32_t cpu, uint32_t timeout_us)
{
    uint32_t waited_us = 0;
    while (waited_us < timeout_us) {
        if (__atomic_load_n(&cpu_boot_flag[cpu], __ATOMIC_ACQUIRE)) {
            return waited_us;
        }
        cache_invalidate(&cpu_boot_flag[cpu]);
        if (cpu_boot_flag[cpu]) {
            return waited_us;
        }
        timer_busy_wait_us(RESURRECT_POLL_US);
        waited_us += RESURRECT_POLL_US;
    }
    return timeout_us;
}

int cpu_supervisor_resurrect(uint32_t cpu)
{
    if (cpu == 0 || cpu >= cpu_count) {
        WARN("cpu_supervisor: invalid resurrect target %u (cpu_count=%u)",
             cpu, cpu_count);
        return -1;
    }

    INFO("cpu_supervisor: attempting resurrect of CPU %u (idle_loops=%u, "
         "frozen %u samples)", cpu, sched_diag_idle_loops[cpu],
         frozen_samples[cpu]);

    /* (1) Take the CPU offline. The work-stealer reads cpu_data->online
     * to decide whether a CPU's deque is stealable; setting this here
     * stops a thief from racing into the run queue we're about to
     * reset. cache_clean already issues dsb sy internally. */
    cpu_data[cpu].online = false;
    cache_clean(&cpu_data[cpu].online);

    /* (2) Reset run queue (drains under rq_lock internally). */
    sched_drain_cpu_runqueue(cpu);

    /* (3) Reset diag counters so the dormancy detector starts fresh
     * after the resurrection attempt. */
    if (sched_diag_idle_loops) sched_diag_idle_loops[cpu] = 0;
    last_idle_loops[cpu] = 0;
    frozen_samples[cpu]  = 0;

    /* (4) Reset boot flag and clean it so the secondary's STLR is
     * the only writer the polling loop sees. */
    __atomic_store_n(&cpu_boot_flag[cpu], 0, __ATOMIC_RELEASE);
    cache_clean(&cpu_boot_flag[cpu]);

    /* (5) PSCI CPU_ON. Forward the same arguments boot_secondary uses
     * the first time around — the secondary_entry assembly reads x0
     * to recover its logical CPU id. */
    uint64_t mpidr = cpu_logical_map[cpu];
    int rc = psci_cpu_on(mpidr, (uintptr_t)&secondary_entry, cpu);
    resurrect_attempts[cpu]++;

    if (rc == PSCI_ALREADY_ON_LOCAL) {
        /* Software wedge — CPU is alive but the scheduler stopped
         * picking work for it (e.g. spinning in WFE without ever
         * taking SEV). We can't psci_cpu_off from here (the CPU has
         * to call it on itself). The drain we did earlier IS the
         * recovery — restore online=true so the scheduler treats
         * the CPU as a valid target again and its idle loop picks
         * up new work on the next SEV. */
        cpu_data[cpu].online = true;
        cache_clean(&cpu_data[cpu].online);
        WARN("cpu_supervisor: CPU %u was ALREADY_ON (software wedge); "
             "queue drained, online flag restored", cpu);
        return -2;
    }
    if (rc != PSCI_SUCCESS_LOCAL) {
        /* Real PSCI failure (DENIED, INVALID_PARAMETERS, NOT_PRESENT,
         * INTERNAL_FAILURE). Leave online=false so the scheduler
         * stops targeting the CPU. The next supervisor tick will
         * still see the dormant counter, but won't loop forever:
         * resurrect_attempts climbs and the operator sees the
         * persistent WARN. A higher-level recovery (reboot, manual
         * intervention) is the only remaining option. */
        WARN("cpu_supervisor: psci_cpu_on(%lu, mpidr=0x%lx) returned %d — "
             "permanent failure, CPU left offline",
             (unsigned long)cpu, (unsigned long)mpidr, rc);
        return -4;
    }

    /* (6) Poll for cpu_boot_flag. */
    uint32_t waited_us = wait_for_boot_flag(cpu, RESURRECT_TIMEOUT_US);

    /* Whether the flag was observed or we timed out, the CPU is up
     * (PSCI returned success). Restore online + bump successes. The
     * timeout case is the cache-incoherency fallback: if the CPU
     * actually isn't running, idle_loops will still be frozen on
     * the next supervisor tick and we'll resurrect again. */
    cpu_data[cpu].online = true;
    cache_clean(&cpu_data[cpu].online);
    resurrect_successes[cpu]++;

    if (waited_us < RESURRECT_TIMEOUT_US) {
        INFO("cpu_supervisor: CPU %u resurrected (boot flag set after "
             "%u us; total successes=%lu)",
             cpu, waited_us, (unsigned long)resurrect_successes[cpu]);
    } else {
        WARN("cpu_supervisor: CPU %u did not signal online within %u us; "
             "PSCI returned success so trusting CPU is up "
             "(cache-incoherency fallback)",
             cpu, RESURRECT_TIMEOUT_US);
    }
    return 0;
}

void cpu_supervisor_get_stats(struct cpu_supervisor_stats *out)
{
    if (!out) return;
    out->dormancy_warnings = dormancy_warnings;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        out->frozen_samples[i]      = frozen_samples[i];
        out->resurrect_attempts[i]  = resurrect_attempts[i];
        out->resurrect_successes[i] = resurrect_successes[i];
    }
}

/*
 * Per-tick check called from the supervisor task. Snapshots
 * idle_loops, bumps the frozen counter for any CPU whose value is
 * unchanged, and triggers resurrection at the threshold. Pure
 * polling loop — no IRQ context, no spinlock-aware contortions
 * needed.
 */
static void supervisor_tick(void)
{
    if (!sched_diag_idle_loops) return;

    for (uint32_t c = 1; c < cpu_count; c++) {
        uint32_t cur = sched_diag_idle_loops[c];
        if (cur != last_idle_loops[c]) {
            last_idle_loops[c] = cur;
            frozen_samples[c]  = 0;
            continue;
        }

        /* Counter unchanged this sample. */
        frozen_samples[c]++;
        if (frozen_samples[c] == SUPERVISOR_FROZEN_THRESHOLD / 2) {
            /* Halfway to threshold — one warning per dormancy episode. */
            WARN("cpu_supervisor: CPU %u idle counter frozen at %u for "
                 "%u samples (will resurrect at %u)",
                 c, cur, SUPERVISOR_FROZEN_THRESHOLD / 2,
                 SUPERVISOR_FROZEN_THRESHOLD);
            dormancy_warnings++;
        }
        if (frozen_samples[c] >= SUPERVISOR_FROZEN_THRESHOLD) {
            (void)cpu_supervisor_resurrect(c);
            /* resurrect resets frozen_samples[c] to 0 either way. */
        }
    }
}

/*
 * Supervisor task entry. Runs forever on CPU 0, sampling once per
 * SUPERVISOR_TICK_MS. Lives at TASK_PRIORITY_LOW — it's monitoring
 * infrastructure, not load-bearing for any deadline.
 */
static void cpu_supervisor_task_entry(void *arg)
{
    (void)arg;
    /* Seed `last_idle_loops` so the first sample doesn't insta-trip. */
    if (sched_diag_idle_loops) {
        for (uint32_t c = 0; c < MAX_CPUS; c++) {
            last_idle_loops[c] = sched_diag_idle_loops[c];
            frozen_samples[c]  = 0;
        }
    }

    while (true) {
        sleep_ms(SUPERVISOR_TICK_MS);
        supervisor_tick();
    }
}

void cpu_supervisor_start(void)
{
    /* Single-CPU deployments don't need the supervisor — there are
     * no secondaries to monitor. */
    if (cpu_count <= 1) {
        return;
    }

    struct task *sup = task_create_with_priority("cpu_supervisor",
                                                 cpu_supervisor_task_entry,
                                                 NULL,
                                                 TASK_PRIORITY_LOW);
    if (!sup) {
        WARN("cpu_supervisor: failed to create supervisor task — "
             "auto-resurrection disabled");
        return;
    }
    /* Pin to CPU 0. The supervisor monitors secondary CPUs; pinning
     * it ensures it always has a live CPU to schedule on, even if a
     * secondary is the one being resurrected. */
    sup->cpu_affinity = 0;
    scheduler_add_task(sup);
    INFO("cpu_supervisor: started (sample %u ms, threshold %u samples)",
         SUPERVISOR_TICK_MS, SUPERVISOR_FROZEN_THRESHOLD);
}

#else  /* !PLATFORM_RASPI5 */

#include "cpu_supervisor.h"

/* Stubs on non-Pi-5 platforms so the API is callable unconditionally
 * from kernel init. Jetson and QEMU have their own fault-recovery
 * stories and are tracked separately. */
void cpu_supervisor_start(void) {}
int  cpu_supervisor_resurrect(uint32_t cpu) { (void)cpu; return -1; }
void cpu_supervisor_get_stats(struct cpu_supervisor_stats *out)
{
    if (!out) return;
    *out = (struct cpu_supervisor_stats){0};
}

#endif /* PLATFORM_RASPI5 */
