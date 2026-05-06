/*
 * cpu_supervisor.h — Tier 2 of #216 (secondary-CPU dormancy recovery).
 *
 * See kernel/sched/cpu_supervisor.c for the rationale and design.
 *
 * Public surface:
 *   - cpu_supervisor_start()      — spawn the monitor task. Idempotent
 *                                   no-op on platforms without the
 *                                   COOP_PREEMPT / fault-recovery
 *                                   story (Jetson, QEMU, x86-64).
 *   - cpu_supervisor_resurrect()  — manual recovery driver. Called
 *                                   automatically by the supervisor at
 *                                   the dormancy threshold; also
 *                                   exposed for shell + tests.
 *   - cpu_supervisor_get_stats()  — diagnostic snapshot for the `cpu`
 *                                   shell command.
 */

#ifndef CPU_SUPERVISOR_H
#define CPU_SUPERVISOR_H

#include "config.h"
#include <stdint.h>

/* Diagnostic snapshot. Per-CPU counters are indexed by logical CPU
 * id; entries beyond `cpu_count` are zero. */
struct cpu_supervisor_stats {
    uint64_t dormancy_warnings;                 /* total warnings emitted */
    uint32_t frozen_samples[MAX_CPUS];          /* current consecutive frozen sample count */
    uint64_t resurrect_attempts[MAX_CPUS];      /* total psci_cpu_on calls */
    uint64_t resurrect_successes[MAX_CPUS];     /* attempts that flipped boot flag */
};

/*
 * Start the supervisor. Spawns a task pinned to CPU 0 that samples
 * sched_diag_idle_loops once per second and triggers resurrection
 * when a secondary CPU's counter is frozen for SUPERVISOR_FROZEN_THRESHOLD
 * consecutive samples. No-op on single-CPU systems.
 */
void cpu_supervisor_start(void);

/*
 * Resurrect a dormant secondary CPU.
 *   - Marks the CPU offline so work-stealing skips it.
 *   - Resets the run queue head/tail/zombie/ready_count.
 *   - Calls psci_cpu_on() with secondary_entry as the entry point.
 *   - Polls cpu_boot_flag for up to RESURRECT_TIMEOUT_US.
 *
 * Returns 0 on success, negative on failure:
 *   -1: invalid `cpu` argument.
 *   -2: psci_cpu_on returned a non-success code (often ALREADY_ON,
 *       meaning the dormancy was a software wedge rather than a fault).
 *   -3: timeout waiting for cpu_boot_flag.
 *
 * Safe to call from a task context. Must NOT be called from an IRQ
 * handler — internally uses sleep_ms / busy waits.
 */
int cpu_supervisor_resurrect(uint32_t cpu);

/*
 * Snapshot the supervisor's diagnostic counters into `out`. `out`
 * must be non-NULL. Counters are zeroed if the supervisor wasn't
 * started.
 */
void cpu_supervisor_get_stats(struct cpu_supervisor_stats *out);

#endif /* CPU_SUPERVISOR_H */
