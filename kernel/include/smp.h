/*
 * smp.h - Symmetric Multi-Processing for SLM-OS
 *
 * Handles secondary core boot via PSCI and per-CPU data management.
 */

#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/* PSCI function IDs (PSCI v0.2+) */
#define PSCI_VERSION            0x84000000
#define PSCI_CPU_SUSPEND_32     0x84000001
#define PSCI_CPU_SUSPEND_64     0xC4000001
#define PSCI_CPU_OFF            0x84000002
#define PSCI_CPU_ON_32          0x84000003
#define PSCI_CPU_ON_64          0xC4000003
#define PSCI_AFFINITY_INFO_32   0x84000004
#define PSCI_AFFINITY_INFO_64   0xC4000004
#define PSCI_SYSTEM_OFF         0x84000008
#define PSCI_SYSTEM_RESET       0x84000009

/* PSCI return codes */
#define PSCI_SUCCESS            0
#define PSCI_NOT_SUPPORTED      (-1)
#define PSCI_INVALID_PARAMS     (-2)
#define PSCI_DENIED             (-3)
#define PSCI_ALREADY_ON         (-4)
#define PSCI_ON_PENDING         (-5)
#define PSCI_INTERNAL_FAILURE   (-6)
#define PSCI_NOT_PRESENT        (-7)
#define PSCI_DISABLED           (-8)
#define PSCI_INVALID_ADDRESS    (-9)

/* Secondary-CPU wait bound for scheduler_init_primary() to complete.
 * Inner delay is ~100k iterations of a volatile loop. On Pi 5 (~2.4 GHz)
 * that's ~40 us per retry, so 125k retries ≈ 5 s. Matched to allow for
 * the slowest path through scheduler_init. */
#define SCHED_INIT_MAX_RETRIES  125000

/* MPIDR masks */
#define MPIDR_AFF0_MASK         0x000000FFUL
#define MPIDR_AFF1_MASK         0x0000FF00UL
#define MPIDR_AFF2_MASK         0x00FF0000UL
#define MPIDR_AFF3_MASK         0xFF00000000UL
#define MPIDR_AFF_MASK          (MPIDR_AFF0_MASK | MPIDR_AFF1_MASK | \
                                 MPIDR_AFF2_MASK | MPIDR_AFF3_MASK)

/* Per-CPU data structure */
struct per_cpu {
    uint32_t cpu_id;            /* Logical CPU ID */
    uint64_t mpidr;             /* Physical MPIDR value */
    volatile bool online;       /* True when core is up and running */
    void *stack_top;            /* Top of this CPU's boot stack */

    /* Statistics */
    uint64_t boot_time_ns;      /* Time when this CPU came online */
};

/* Global SMP state */
extern struct per_cpu cpu_data[MAX_CPUS];
extern uint64_t cpu_logical_map[MAX_CPUS];
extern uint32_t cpu_count;
extern volatile uint32_t cpus_online;

#if defined(PLATFORM_X86_64)

/*
 * Get current CPU's LAPIC ID.
 * The LAPIC ID register is memory-mapped at LAPIC_BASE + 0x020.
 * Reading it from the default address 0xFEE00000 works for all CPUs.
 */
static inline uint32_t cpu_get_lapic_id(void) {
    volatile uint32_t *lapic = (volatile uint32_t *)0xFEE00000UL;
    return (lapic[0x020 / 4] >> 24) & 0xFF;
}

/*
 * Get logical CPU ID from LAPIC ID.
 * Returns -1 if LAPIC ID not found in mapping table.
 */
int cpu_logical_id(uint64_t mpidr);

/*
 * Get current CPU's logical ID.
 */
static inline uint32_t cpu_id(void) {
    int id = cpu_logical_id(cpu_get_lapic_id());
    return (id >= 0) ? (uint32_t)id : 0;
}

static inline struct per_cpu *this_cpu(void) {
    return &cpu_data[cpu_id()];
}

#else /* ARM64 */

/*
 * Get current CPU's MPIDR value.
 */
static inline uint64_t cpu_get_mpidr(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr;
}

/*
 * Get logical CPU ID from MPIDR value.
 * Returns -1 if MPIDR not found in mapping table.
 */
int cpu_logical_id(uint64_t mpidr);

/*
 * Get current CPU's logical ID.
 */
static inline uint32_t cpu_id(void) {
    int id = cpu_logical_id(cpu_get_mpidr());
    return (id >= 0) ? (uint32_t)id : 0;
}

/*
 * Get per-CPU data for current CPU.
 */
static inline struct per_cpu *this_cpu(void) {
    return &cpu_data[cpu_id()];
}

#endif /* PLATFORM_X86_64 */

/*
 * Initialize SMP subsystem.
 * Called by primary CPU after basic kernel init.
 * Discovers CPUs and brings up secondary cores.
 */
void smp_init(void);

/*
 * Power on a secondary CPU via PSCI.
 *
 * @target_mpidr: MPIDR of target CPU
 * @entry_point:  Address where CPU starts execution
 * @context_id:   Value passed to CPU in x0 on entry
 *
 * Returns: PSCI return code (0 = success)
 */
int psci_cpu_on(uint64_t target_mpidr, uintptr_t entry_point,
                uintptr_t context_id);

/*
 * Record / read the calling CPU's CurrentEL at boot. Used to verify
 * every CPU landed at the expected EL (#683 PR-3 — EL2h+VHE on Pi 5
 * and Jetson, EL1h on QEMU). Returns 0xFFFFFFFF if the slot was not
 * recorded (e.g. NC alloc failed).
 */
void cpu_record_current_el(uint32_t cpu);
uint64_t cpu_get_current_el(uint32_t cpu);

/*
 * Power off the calling CPU via PSCI.
 * Does not return on success.
 */
void psci_cpu_off(void);

/*
 * Reset the system via PSCI.
 * Does not return.
 */
void psci_system_reset(void);

/*
 * Power off the system via PSCI.
 * Does not return.
 */
void psci_system_off(void);

/*
 * Secondary CPU entry point (called from smp_boot.S).
 * Performs per-CPU initialization and enters idle loop.
 */
void secondary_init(uint32_t cpu_id);

/*
 * Assembly entry point for secondary CPUs.
 * Defined in smp_boot.S.
 */
extern void secondary_entry(void);

/*
 * Cross-core boot handshake flag, indexed by logical CPU id. Defined
 * in kernel/sched/smp.c. The secondary CPU's bring-up assembly STLRs
 * a 1 here once it reaches `secondary_init`; the BSP polls this slot
 * after issuing `psci_cpu_on`. Also reset and re-polled by the
 * resurrection path in kernel/sched/cpu_supervisor.c.
 */
extern volatile uint32_t cpu_boot_flag[MAX_CPUS];

/*
 * smp_notify_cpu() — nudge a remote CPU to re-check its run queue.
 *
 * The scheduler calls this after enqueuing a task on another CPU's
 * run queue so that the target CPU, if it is currently idle (WFE on
 * ARM64 / HLT on x86-64), wakes up and picks up the new task
 * without waiting for its next timer tick.
 *
 * Backends:
 *   ARM64 — issues `SEV`; the target CPU's WFE falls through and
 *           the idle loop re-checks the run queue.
 *   x86-64 — sends a RESCHED_VECTOR LAPIC IPI to the target's APIC
 *            ID; the IPI handler calls schedule() directly.
 *
 * A call with @logical_cpu == cpu_id() is a cheap no-op.
 * If @logical_cpu >= cpu_count the implementation must ignore it
 * (some callers compute from task->assigned_cpu which can be stale).
 */
void smp_notify_cpu(uint32_t logical_cpu);

#endif /* SMP_H */
