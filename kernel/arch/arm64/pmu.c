/*
 * pmu.c - ARM PMU initialization + register-access primitives (#874).
 *
 * Pi 5 Cortex-A76 and Jetson Cortex-A78AE expose the ARMv8-A
 * architectural PMU at EL1/EL2. TF-A on Pi 5 already clears
 * MDCR_EL3 so non-secure PMU access is allowed; Jetson EL2 access
 * under NVIDIA BL31 is verified separately as #875.
 *
 * The per-CPU init runs from each CPU's own boot path:
 *   - Primary CPU: kernel_main calls pmu_enable_self() after
 *     scheduler_init has settled.
 *   - Secondary CPUs: secondary_init calls pmu_enable_self() after
 *     timer_percpu_init.
 *
 * Snapshot reads do not take a lock — PMU sysregs are per-CPU. A
 * read on CPU N returns CPU N's counters regardless of who's
 * sampling. The profile harness (#871) calls pmu_read_all() from
 * the engine task; whichever CPU happens to run that task gets that
 * CPU's deltas, which is exactly the semantics we want for per-op
 * attribution (the op physically runs on that CPU too).
 */

#include "../../include/pmu.h"
#include "../../include/smp.h"
#include "../../include/config.h"
#include <stdbool.h>
#include <stdint.h>

#if !defined(PLATFORM_X86_64)

/* PMCR_EL0 bit layout (ARM ARM D13.2.118):
 *   bit 0  E   — global enable for all counters
 *   bit 1  P   — reset event counters (write-1-to-act, RAZ)
 *   bit 2  C   — reset cycle counter (write-1-to-act, RAZ)
 *   bit 3  D   — clock divider (0 = 1:1)
 *   bit 4  X   — export events (debug bus, unused here)
 *   bit 5  DP  — disable cycle counter when prohibited (0 = always on)
 *   bit 6  LC  — long cycle counter (cycle counter overflows on 64-bit)
 *   bit 7  LP  — long event counter (event counters overflow on 64-bit)
 */
#define PMCR_EL0_E   (1u << 0)
#define PMCR_EL0_P   (1u << 1)
#define PMCR_EL0_C   (1u << 2)
#define PMCR_EL0_LC  (1u << 6)

/* PMCNTENSET_EL0: bit per counter. Bit 31 enables the cycle counter;
 * bits 0..30 enable PMEVCNTR0..30. We use 0..5 plus the cycle bit. */
#define PMCNTEN_EVENT_MASK_6  0x3Fu
#define PMCNTEN_CYCLE_BIT     (1u << 31)

/* Per-CPU readiness flag. Read by the profile harness to discriminate
 * "PMU not initialized on this CPU yet" from "PMU returned 0 for a
 * real reason." Cache-line padded would be ideal; in practice this
 * flag flips once per boot per CPU so contention is irrelevant. */
static volatile bool pmu_ready_per_cpu[MAX_CPUS];

/* The preset event table for Cortex-A76 / Cortex-A78AE. Indices match
 * the layout documented in pmu.h:
 *   [0] L1D miss   [1] L2D miss   [2] inst retired
 *   [3] branch mispred  [4] mem access  [5] backend stall
 *
 * Both micro-architectures implement these as ARMv8-A architectural
 * events; the encoding is identical across implementations per the
 * ARM ARM. Verified for A78AE in #875.
 */
static const uint32_t pmu_default_events[PMU_NUM_EVENT_COUNTERS] = {
    PMU_EVT_L1D_CACHE_REFILL,
    PMU_EVT_L2D_CACHE_REFILL,
    PMU_EVT_INST_RETIRED,
    PMU_EVT_BR_MIS_PRED,
    PMU_EVT_MEM_ACCESS,
    PMU_EVT_STALL_BACKEND,
};

static inline void pmu_write_pmcr(uint64_t v)
{
    __asm__ volatile("msr pmcr_el0, %0" :: "r"(v) : "memory");
}

static inline uint64_t pmu_read_pmcr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, pmcr_el0" : "=r"(v));
    return v;
}

/*
 * Program PMEVTYPER<idx>_EL0 to track `event_id`. ARM exposes the
 * registers as 32 independent sysregs (PMEVTYPER0..30, plus the cycle-
 * counter type at PMEVTYPER31 / PMCCFILTR). The MSR opcode takes an
 * immediate sysreg name, not an indexed one, so we dispatch on idx.
 *
 * Only idx in [0, 5] is touched; out-of-range is silently ignored.
 */
static void pmu_write_evtyper(uint32_t idx, uint32_t event_id)
{
    uint64_t v = (uint64_t)event_id;
    switch (idx) {
    case 0: __asm__ volatile("msr pmevtyper0_el0, %0" :: "r"(v) : "memory"); break;
    case 1: __asm__ volatile("msr pmevtyper1_el0, %0" :: "r"(v) : "memory"); break;
    case 2: __asm__ volatile("msr pmevtyper2_el0, %0" :: "r"(v) : "memory"); break;
    case 3: __asm__ volatile("msr pmevtyper3_el0, %0" :: "r"(v) : "memory"); break;
    case 4: __asm__ volatile("msr pmevtyper4_el0, %0" :: "r"(v) : "memory"); break;
    case 5: __asm__ volatile("msr pmevtyper5_el0, %0" :: "r"(v) : "memory"); break;
    default: break;
    }
}

static uint32_t pmu_read_evcntr(uint32_t idx)
{
    uint64_t v = 0;
    switch (idx) {
    case 0: __asm__ volatile("mrs %0, pmevcntr0_el0" : "=r"(v)); break;
    case 1: __asm__ volatile("mrs %0, pmevcntr1_el0" : "=r"(v)); break;
    case 2: __asm__ volatile("mrs %0, pmevcntr2_el0" : "=r"(v)); break;
    case 3: __asm__ volatile("mrs %0, pmevcntr3_el0" : "=r"(v)); break;
    case 4: __asm__ volatile("mrs %0, pmevcntr4_el0" : "=r"(v)); break;
    case 5: __asm__ volatile("mrs %0, pmevcntr5_el0" : "=r"(v)); break;
    default: return 0;
    }
    return (uint32_t)v;
}

bool pmu_enable_self(void)
{
    /* Step 1+2: hard reset both counter banks AND enable. The C/P bits
     * are write-1-to-act, so OR-ing them with E in a single write
     * resets and starts the counters in one go. LC=1 puts the cycle
     * counter into 64-bit mode (no 32-bit wrap).
     */
    pmu_write_pmcr(PMCR_EL0_E | PMCR_EL0_P | PMCR_EL0_C | PMCR_EL0_LC);

    /* Step 3: kernel-only access. We never expose PMU reads to EL0
     * userspace (no app code at EL0 reads them today). Clear EN, SW,
     * CR, ER bits — PMUSERENR_EL0 = 0. */
    __asm__ volatile("msr pmuserenr_el0, %0" :: "r"((uint64_t)0) : "memory");

    /* Step 4: disable PMU overflow interrupts. PMINTENCLR_EL0 is
     * write-1-to-clear; writing 0xFFFFFFFF clears every interrupt-
     * enable bit. Polled reads only — interrupt routing on Pi 5 / Jetson
     * NS-EL2 is its own bringup. */
    __asm__ volatile("msr pmintenclr_el1, %0" :: "r"((uint64_t)0xFFFFFFFFULL)
                     : "memory");

    /* Step 5: clear pending overflow flags. PMOVSCLR_EL0 is also write-
     * 1-to-clear. */
    __asm__ volatile("msr pmovsclr_el0, %0" :: "r"((uint64_t)0xFFFFFFFFULL)
                     : "memory");

    /* Step 6: program the six event counters with the architectural
     * preset. */
    for (uint32_t i = 0; i < PMU_NUM_EVENT_COUNTERS; i++) {
        pmu_write_evtyper(i, pmu_default_events[i]);
    }

    /* Step 7: enable the six event counters + the cycle counter.
     * PMCNTENSET_EL0 is write-1-to-set; bits we don't want enabled are
     * unaffected. */
    __asm__ volatile("msr pmcntenset_el0, %0"
                     :: "r"((uint64_t)(PMCNTEN_EVENT_MASK_6 | PMCNTEN_CYCLE_BIT))
                     : "memory");

    /* Mark this CPU's PMU ready. The store-release isn't strictly
     * required here — readers either run on this same CPU (no
     * cross-CPU visibility issue) or treat a stale `false` as "not
     * ready, skip the read," which is the safe direction. */
    uint32_t cpu = cpu_id();
    if (cpu < MAX_CPUS) {
        pmu_ready_per_cpu[cpu] = true;
    }

    /* Sanity: confirm PMCR_EL0.E is actually set. If the EL3 firmware
     * has trapped PMU writes (Jetson stock BL31 is the unknown — #875
     * verifies), the write silently does nothing and we want callers
     * to know. */
    return (pmu_read_pmcr() & PMCR_EL0_E) != 0;
}

uint64_t pmu_read_cycles(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, pmccntr_el0" : "=r"(v));
    return v;
}

uint32_t pmu_read_event(uint32_t idx)
{
    if (idx >= PMU_NUM_EVENT_COUNTERS) {
        return 0;
    }
    return pmu_read_evcntr(idx);
}

void pmu_reset(void)
{
    /* P=1 clears event counters, C=1 clears the cycle counter, E=1
     * keeps them running. The reset bits are write-1-to-act (RAZ on
     * read), so OR-ing them with the currently-set E bit re-arms the
     * counters from zero in a single MSR. */
    pmu_write_pmcr(PMCR_EL0_E | PMCR_EL0_P | PMCR_EL0_C | PMCR_EL0_LC);
}

void pmu_read_all(struct pmu_snapshot *out)
{
    if (!out) {
        return;
    }
    out->cycles = pmu_read_cycles();
    for (uint32_t i = 0; i < PMU_NUM_EVENT_COUNTERS; i++) {
        out->events[i] = pmu_read_evcntr(i);
    }
}

bool pmu_is_ready(void)
{
    uint32_t cpu = cpu_id();
    if (cpu >= MAX_CPUS) {
        return false;
    }
    return pmu_ready_per_cpu[cpu];
}

#endif /* !PLATFORM_X86_64 */
