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

/* If PMU_NUM_EVENT_COUNTERS grows past 6, both pmu_write_evtyper and
 * pmu_read_evcntr below need new switch arms — silently dropping the
 * extra indices would be a correctness bug (events 6+ never programmed,
 * never read). Pin the contract at compile time. */
_Static_assert(PMU_NUM_EVENT_COUNTERS == 6,
    "pmu.c switch tables hardcode 6 counters; widen them when bumping "
    "PMU_NUM_EVENT_COUNTERS");

/* PMCR_EL0.LC selects the 64-bit cycle counter on ARMv8.5+. Cortex-A76
 * is v8.2 where the bit may be RES0 — the cycle counter falls back to
 * 32-bit and wraps in ~6 s at 1 GHz. The per-op profile harness (#871)
 * calls pmu_reset() at the start of every execute_node so the per-op
 * cycle delta stays in the microsecond range regardless. We set LC=1
 * defensively for v8.5 and later. */

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

/* PMEVTYPER<n>_EL0 / PMCCFILTR_EL0 filter bits (ARM ARM D13.2.125):
 *   bit 31  P    — when set, exclude EL1 events
 *   bit 30  U    — when set, exclude EL0 events
 *   bit 29  NSK  — when set, toggle P semantics in NS state
 *   bit 28  NSU  — when set, toggle U semantics in NS state
 *   bit 27  NSH  — when set, INCLUDE NS-EL2 events (under VHE: kernel)
 *   bit 26  M    — when set, exclude EL3 events
 *
 * Default (all filter bits 0) counts NS-EL1 + NS-EL0 only — NOT
 * NS-EL2. SLM-OS runs at NS-EL2 with E2H=1+TGE=1 (VHE) on Pi 5 and
 * Jetson; QEMU virt runs at EL1. We unconditionally set NSH=1 so
 * EL2 events are captured under VHE. On EL1-only platforms NSH is
 * harmless (no EL2 to filter). */
#define PMU_FILTER_NSH (1ULL << 27)

/* MDCR_EL2.HPMN[4:0] mask. HPMN partitions the PMU event counters
 * into a non-secure half [0, HPMN) and an EL2-only half [HPMN, N).
 * pmu_enable_self writes PMU_NUM_EVENT_COUNTERS here so all six are
 * in the NS partition. */
#define MDCR_EL2_HPMN_MASK 0x1FULL

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
    /* Include NS-EL2 events so the counters actually tick under
     * VHE. See PMU_FILTER_NSH comment above. */
    uint64_t v = (uint64_t)(event_id | PMU_FILTER_NSH);
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
    /*
     * Step 0: MDCR_EL2.HPMN partition setup. HPMN[4:0] defines how
     * many event counters are owned by the non-secure partition.
     * Counters [HPMN, N-1] are EL2-only when running at NS-EL1 and
     * always read as zero from NS-EL1 if not in the NS partition.
     * On warm boot under TF-A (Pi 5 + Jetson) MDCR_EL2 may reset
     * with HPMN=0 — all counters end up in the EL2-only half and
     * even at NS-EL2 the cycle counter ticks but every event
     * counter reads RAZ.
     *
     * Set HPMN = PMU_NUM_EVENT_COUNTERS so all six counters are in
     * the non-secure partition. Clear HPMD (don't disable cycle
     * counter in prohibited regions). Leave TPM/TPMCR alone — those
     * trap NS-EL1 to EL2; since we run at NS-EL2 directly they don't
     * affect us, and clearing them would weaken protection if EL0
     * userspace ever runs.
     *
     * Only safe to do at EL2. At EL1 the MSR traps to EL2, so we
     * detect EL by reading CurrentEL. Pi 5 (EL2+VHE) and Jetson
     * (EL2+VHE) both take this path; QEMU virt runs at EL1 and
     * skips it (MDCR_EL2 is firmware's responsibility there).
     */
    uint64_t current_el;
    __asm__ volatile("mrs %0, currentel" : "=r"(current_el));
    current_el = (current_el >> 2) & 0x3;
    if (current_el >= 2) {
        uint64_t mdcr_el2;
        __asm__ volatile("mrs %0, mdcr_el2" : "=r"(mdcr_el2));
        /* Clear HPMN[4:0] and set to PMU_NUM_EVENT_COUNTERS. */
        mdcr_el2 = (mdcr_el2 & ~MDCR_EL2_HPMN_MASK)
                 | (uint64_t)PMU_NUM_EVENT_COUNTERS;
        __asm__ volatile("msr mdcr_el2, %0" :: "r"(mdcr_el2) : "memory");
        __asm__ volatile("isb" ::: "memory");
    }

    /* Step 1+2: hard reset both counter banks AND enable. The C/P bits
     * are write-1-to-act, so OR-ing them with E in a single write
     * resets and starts the counters in one go. LC=1 puts the cycle
     * counter into 64-bit mode (no 32-bit wrap).
     */
    pmu_write_pmcr(PMCR_EL0_E | PMCR_EL0_P | PMCR_EL0_C | PMCR_EL0_LC);

    /* PMCCFILTR_EL0 governs the cycle counter the same way
     * PMEVTYPER<n>_EL0 governs event counters. Set NSH=1 so EL2
     * cycles count under VHE. Other filter bits stay 0 (count
     * EL0 + EL1, exclude EL3). */
    __asm__ volatile("msr pmccfiltr_el0, %0"
                     :: "r"((uint64_t)PMU_FILTER_NSH) : "memory");

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
