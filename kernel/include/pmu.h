/*
 * pmu.h - ARM Performance Monitor Unit (PMU) primitives
 *
 * Provides register-access wrappers around the ARMv8-A PMU (sysreg
 * group PMCR_EL0 / PMEVTYPER<x>_EL0 / PMEVCNTR<x>_EL0 / PMCCNTR_EL0)
 * for use by the per-op profile harness (#871) and the AI scheduler's
 * cache_pressure state-vector slot (#872).
 *
 * Pi 5 Cortex-A76 and Jetson Cortex-A78AE are the two ARM targets.
 * Both implement the ARMv8-A architectural event encodings used in
 * the preset table below (`pmu_init_default_events`). x86-64 has its
 * own RDPMC equivalent tracked separately as #870 — this header is
 * ARM-only and #if'd out on PLATFORM_X86_64.
 *
 * Counter sizing: PMEVCNTR<x>_EL0 are 32-bit event counters; PMCCNTR_EL0
 * is 64-bit (we don't enable the LP=0 wraparound). The profile harness
 * resets counters at the start of each `execute_node` so the per-op
 * delta never exceeds the µs-scale, well below the seconds-scale 32-bit
 * overflow horizon at Pi 5 / Jetson clock rates.
 *
 * Per-CPU init: every CPU must call `pmu_enable_self()` after its MMU
 * is live (so cacheable spinlocks work). Primary CPU calls it from
 * `kernel_main`; secondary CPUs call it from `secondary_init`. Skipping
 * a CPU leaves PMCR_EL0.E=0 for that CPU and the profile reads return
 * stale data.
 */

#ifndef SLM_PMU_H
#define SLM_PMU_H

#include <stdint.h>
#include <stdbool.h>

#if !defined(PLATFORM_X86_64)

/* Number of programmable event counters this module configures.
 * ARMv8-A allows up to 31 (PMCR_EL0.N); Cortex-A76 implements 6 and
 * Cortex-A78AE implements 6. We pin the count at 6 so both targets
 * use the same preset table. */
#define PMU_NUM_EVENT_COUNTERS 6

/* ARMv8-A architectural event encodings (PMEVTYPER<x>_EL0.evtCount).
 * These are valid on both Cortex-A76 (Pi 5) and Cortex-A78AE (Jetson).
 * Documented in ARM ARM D7.10 "PMU events and event numbers." */
#define PMU_EVT_L1D_CACHE_REFILL   0x03  /* L1 data cache refill (miss) */
#define PMU_EVT_INST_RETIRED       0x08  /* Instructions retired */
#define PMU_EVT_BR_MIS_PRED        0x10  /* Mispredicted branch */
#define PMU_EVT_MEM_ACCESS         0x13  /* Memory access (load+store) */
#define PMU_EVT_L2D_CACHE_REFILL   0x17  /* L2 data cache refill (miss) */
#define PMU_EVT_STALL_BACKEND      0x24  /* Backend stalled cycles */

/*
 * Initialize the PMU on the current CPU.
 *
 * Sequence:
 *   1. PMCR_EL0 := {DP=0, X=0, D=0, C=1, P=1, E=1} — reset all counters
 *      then enable.
 *   2. PMUSERENR_EL0 := 0 — kernel-only access (we run at EL1/EL2; no
 *      EL0 PMU reads are exposed).
 *   3. PMINTENCLR_EL0 := 0xFFFFFFFF — disable all PMU overflow interrupts
 *      (we use polled reads; interrupt routing on Pi 5 / Jetson at NS-EL2
 *      is its own bringup and not needed for periodic delta sampling).
 *   4. PMOVSCLR_EL0  := 0xFFFFFFFF — clear any pending overflow flags.
 *   5. Program the six counters per `pmu_init_default_events` (see
 *      `kernel/arch/arm64/pmu.c`).
 *   6. PMCNTENSET_EL0 := bits 0..5 + bit 31 — enable the six event
 *      counters and the cycle counter.
 *
 * Must be called from each CPU after its MMU is live. Safe to call
 * more than once on the same CPU — it re-resets every counter.
 *
 * No-op on PLATFORM_X86_64 (the header guards the entire prototype).
 *
 * Returns true on success, false if a sanity check fails (currently
 * always true on ARM64; the return type leaves room for future
 * EL2-trap detection on platforms where access traps).
 */
bool pmu_enable_self(void);

/* Read the 64-bit cycle counter (PMCCNTR_EL0). */
uint64_t pmu_read_cycles(void);

/*
 * Read one of the six programmable event counters (32-bit
 * PMEVCNTR<idx>_EL0).
 *
 * `idx` must be in [0, PMU_NUM_EVENT_COUNTERS). Returns 0 for an
 * out-of-range index — the harness already validates indices via the
 * preset table, so the guard is defensive only.
 */
uint32_t pmu_read_event(uint32_t idx);

/*
 * Reset every counter (cycle + the six programmable events) to zero
 * by writing PMCR_EL0.{C=1, P=1, E=1}. The per-op profile harness
 * (#871) calls this at the start of each `execute_node` invocation so
 * the read-after-op delta is the absolute counter value — no
 * subtraction-with-overflow corner case.
 */
void pmu_reset(void);

/*
 * Snapshot all configured counters in one call. Mirrors the shape used
 * by the profile harness so the FFI side can do a single read per op.
 * `cycles` is the 64-bit PMCCNTR_EL0; the six event entries are
 * PMEVCNTR0_EL0 .. PMEVCNTR5_EL0 in preset order:
 *   [0] = L1D_CACHE_REFILL
 *   [1] = L2D_CACHE_REFILL
 *   [2] = INST_RETIRED
 *   [3] = BR_MIS_PRED
 *   [4] = MEM_ACCESS
 *   [5] = STALL_BACKEND
 */
struct pmu_snapshot {
    uint64_t cycles;
    uint32_t events[PMU_NUM_EVENT_COUNTERS];
};

void pmu_read_all(struct pmu_snapshot *out);

/*
 * Has pmu_enable_self() completed on this CPU yet? Lets callers (the
 * profile harness, the AI scheduler cache_pressure read) gate cleanly
 * on a CPU whose secondary boot hasn't reached pmu init. The per-CPU
 * flag lives in a small static array indexed by `smp_processor_id()`.
 */
bool pmu_is_ready(void);

#endif /* !PLATFORM_X86_64 */

#endif /* SLM_PMU_H */
