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

/* x86-64 single-core: CPU ID is always 0 */
static inline uint32_t cpu_id(void) {
    return 0;
}

static inline struct per_cpu *this_cpu(void) {
    return &cpu_data[0];
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

#endif /* SMP_H */
