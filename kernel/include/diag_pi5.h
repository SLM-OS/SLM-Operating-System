/*
 * diag_pi5.h - Pi 5 IRQ-delivery diagnostics (Phase 1 of #99).
 *
 * NC-memory layout used by boot.S (EL2 register snapshot), vectors.S
 * (per-CPU per-vector exception counters), and exceptions.c (el1_fiq
 * source trace). Exposed to userland via the `diag` shell command so
 * we can definitively distinguish the #99 root cause (see
 * docs/pi5-preemption-plan.md Phase 1).
 *
 * Only meaningful when PI5_IRQ_DIAG is defined (RASPI5 platform).
 */

#ifndef DIAG_PI5_H
#define DIAG_PI5_H

#include <stdint.h>
#include "ncmem.h"

#if defined(PI5_IRQ_DIAG)

#if !defined(PLATFORM_HAS_NC_MEMORY)
#error "PI5_IRQ_DIAG requires a platform with NC memory"
#endif

/*
 * Layout (offsets relative to NC_MEM_BASE):
 *   0xFF00..0xFF4F  EL2 register snapshot (primary CPU only)
 *   0xFF50          Magic 0x5EE15EE15EE15EE1 (valid snapshot marker)
 *   0xFF60..0xFFDF  Per-CPU exception counters (4 CPUs × 0x20 bytes)
 *   0xFFE0..0xFFEF  Per-CPU FIQ source trace (last GICC_AIAR value)
 *
 * All slots are u64 unless otherwise noted. The layout must match the
 * offsets used by boot.S and vectors.S.
 */

#define DIAG_EL2_BASE       (NC_MEM_BASE + 0xFF00UL)
#define DIAG_EL2_MAGIC      0x5EE15EE15EE15EE1UL

struct diag_el2_snapshot {
    uint64_t hcr_el2_pre;        /* +0x00  HCR_EL2 at entry to EL2 block */
    uint64_t hcr_el2_post;       /* +0x08  HCR_EL2 after our write */
    uint64_t cnthctl_el2;        /* +0x10  CNTHCTL_EL2 as written */
    uint64_t current_el_entry;   /* +0x18  CurrentEL at firmware entry */
    uint64_t midr_el1;           /* +0x20  MIDR_EL1 */
    uint64_t id_aa64pfr0_el1;    /* +0x28  ID_AA64PFR0_EL1 */
    uint64_t gicd_ctlr_pre;      /* +0x30  GICD_CTLR before our writes */
    uint64_t gicc_ctlr_pre;      /* +0x38  GICC_CTLR before our writes */
    uint64_t gicd_igroupr0_post; /* +0x40  GICD_IGROUPR[0] readback */
    uint64_t gicd_igroupr0_pre;  /* +0x48  GICD_IGROUPR[0] pre-write */
    uint64_t magic;              /* +0x50  DIAG_EL2_MAGIC when set */
};

#define DIAG_VEC_BASE       (NC_MEM_BASE + 0xFF60UL)
#define DIAG_VEC_PER_CPU    0x20UL
#define DIAG_VEC_OFF_SYNC   0x00UL
#define DIAG_VEC_OFF_IRQ    0x08UL
#define DIAG_VEC_OFF_FIQ    0x10UL
#define DIAG_VEC_OFF_SERROR 0x18UL

struct diag_vec_counts {
    uint64_t sync;
    uint64_t irq;
    uint64_t fiq;
    uint64_t serror;
};

#define DIAG_FIQ_TRACE      (NC_MEM_BASE + 0xFFE0UL)

/*
 * Accessors. These are inline so the NC addresses compile to direct
 * absolute loads/stores — consistent with how the rest of the kernel
 * reaches NC memory.
 */

static inline struct diag_el2_snapshot *diag_el2_snap(void)
{
    return (struct diag_el2_snapshot *)DIAG_EL2_BASE;
}

static inline struct diag_vec_counts *diag_vec_counts_cpu(uint32_t cpu)
{
    return (struct diag_vec_counts *)(DIAG_VEC_BASE + cpu * DIAG_VEC_PER_CPU);
}

/* Last FIQ source IRQ captured by el1_fiq_handler (0x3FF if spurious). */
static inline uint32_t diag_fiq_last(uint32_t cpu)
{
    return *(volatile uint32_t *)(DIAG_FIQ_TRACE + cpu * 4);
}

/* Shell command entry point. */
int cmd_diag(int argc, char *argv[]);

/*
 * Compile-time layout asserts — boot.S and vectors.S reference raw
 * offsets into these structures. If someone reorders fields, the
 * hardcoded offsets break silently; the asserts here fail the build
 * instead.
 */
_Static_assert(sizeof(struct diag_el2_snapshot) == 0x58,
    "diag_el2_snapshot layout changed — boot.S offsets need updating");
_Static_assert(__builtin_offsetof(struct diag_el2_snapshot, hcr_el2_post) == 0x08,
    "hcr_el2_post offset must be 0x08 (boot.S hardcodes it)");
_Static_assert(__builtin_offsetof(struct diag_el2_snapshot, magic) == 0x50,
    "magic offset must be 0x50 (boot.S hardcodes it)");
_Static_assert(__builtin_offsetof(struct diag_el2_snapshot, gicd_igroupr0_post) == 0x40,
    "IGROUPR readback offset must be 0x40 (boot.S hardcodes it)");
_Static_assert(sizeof(struct diag_vec_counts) == DIAG_VEC_PER_CPU,
    "diag_vec_counts size must match DIAG_VEC_PER_CPU (vectors.S assumes 0x20)");

#endif /* PI5_IRQ_DIAG */
#endif /* DIAG_PI5_H */
