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
 * the preset table (`pmu_default_events` in `kernel/arch/arm64/pmu.c`).
 * x86-64 has its own RDPMC equivalent tracked separately as #870 —
 * this header is ARM-only and #if'd out on PLATFORM_X86_64.
 *
 * Counter sizing: PMEVCNTR<x>_EL0 are 32-bit event counters; PMCCNTR_EL0
 * is 64-bit (we don't enable the LP=0 wraparound). The profile harness
 * resets counters at the start of each `execute_node` so the per-op
 * delta never exceeds the µs-scale, well below the seconds-scale 32-bit
 * overflow horizon at Pi 5 / Jetson clock rates.
 *
 * Per-CPU init: every CPU must call `pmu_enable_self()` after EL2 setup
 * has reached a stable state — at NS-EL2/VHE on Pi 5 and Jetson, that
 * means after the kernel pivots to its own EL2 vector table and HCR_EL2
 * is configured. Primary CPU calls it from `kernel_main` (after
 * `timer_init`); secondary CPUs call it from `secondary_init`. The
 * function takes no locks and does not touch memory beyond a per-CPU
 * readiness flag; the only EL-related requirement is that MDCR_EL2
 * writes don't trap (i.e. we're at NS-EL2 directly, not EL1). Skipping
 * a CPU leaves PMCR_EL0.E=0 for that CPU and every counter reads as 0
 * — the counter never ticks.
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
 * Sequence (kernel/arch/arm64/pmu.c implements the steps):
 *   0. (EL2 only) MDCR_EL2.HPMN := PMU_NUM_EVENT_COUNTERS — put all
 *      six event counters in the non-secure partition so NS-EL2 reads
 *      don't return RAZ. Skipped at EL1 (where the write would trap).
 *   1. PMCR_EL0       := {LC=1, C=1, P=1, E=1} — reset all counters,
 *      enable, put the cycle counter in 64-bit mode where supported.
 *   2. PMCCFILTR_EL0  := NSH=1 — count cycles in NS-EL2 (VHE). Without
 *      this, the cycle counter stays at 0 under VHE because the default
 *      filter excludes EL2 events.
 *   3. PMUSERENR_EL0  := 0 — kernel-only access (we run at EL1/EL2;
 *      no EL0 PMU reads are exposed).
 *   4. PMINTENCLR_EL1 := 0xFFFFFFFF — disable PMU overflow interrupts.
 *      We use polled reads; interrupt routing on Pi 5 / Jetson at
 *      NS-EL2 is its own bringup and not needed for delta sampling.
 *   5. PMOVSCLR_EL0   := 0xFFFFFFFF — clear pending overflow flags.
 *   6. Program the six event counters per `pmu_default_events`; each
 *      gets PMU_FILTER_NSH OR'd in so they also count NS-EL2 events.
 *   7. PMCNTENSET_EL0 := bits 0..5 + bit 31 — enable the six event
 *      counters and the cycle counter.
 *
 * Must be called from each CPU after EL2 setup is stable (Pi 5 /
 * Jetson VHE pivot) or unconditionally at EL1 (QEMU). Safe to call
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

/*
 * Sample the calling CPU's cache pressure (L1D miss rate) and update
 * its per-CPU EWMA. Must run on the CPU being sampled — PMU sysregs
 * are per-CPU. The scheduler tick (`scheduler_tick` in sched.c) is the
 * canonical caller; each CPU updates its own slot on every tick.
 *
 * Does NOT reset PMU counters — it tracks deltas against the previous
 * snapshot. The per-op profile harness (#871) calls `pmu_reset` at
 * the start of each `execute_node`; this sampler is robust to that
 * (subtraction with 32-bit wrap is wrap-safe in uint32_t arithmetic)
 * but a reset between two consecutive ticks will cause that tick's
 * sample to under-report by one execute_node's worth of misses. The
 * AI policy already tolerates this kind of noise.
 *
 * No-op on PLATFORM_X86_64 and on a CPU where pmu_enable_self() has
 * not run yet.
 */
void pmu_sample_cache_pressure(void);

/*
 * Read the most recent per-CPU cache_pressure EWMA for `cpu`,
 * normalized to [0.0, 1.0]. Safe to call from any CPU — the array is
 * a plain per-CPU u32 cache that the writer updates with relaxed
 * stores. Returns 0.0 for an uninitialised slot.
 *
 * Returned as a fixed-point uint32_t scaled by PMU_Q16_ONE to avoid
 * pulling FP into pmu.c (which is compiled with -mgeneral-regs-only
 * like the rest of the kernel; FP conversion lives in `ai_state.c`).
 */
uint32_t pmu_get_cache_pressure_q16(uint32_t cpu);

/* Q16.16 fixed-point unit value (represents 1.0 — the saturation
 * cap for pmu_get_cache_pressure_q16). Exposed so callers and
 * regression tests can reference the cap symbolically instead of
 * inlining the literal. */
#define PMU_Q16_ONE (1u << 16)

#endif /* !PLATFORM_X86_64 */

#endif /* SLM_PMU_H */
