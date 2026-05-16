/*
 * gic.c - GIC driver for SLM-OS
 *
 * Generic Interrupt Controller driver supporting GICv2 and GICv3.
 * - GICv2: Used by QEMU virt machine (memory-mapped CPU interface)
 * - GICv3: Used by Jetson Orin Nano (system register CPU interface)
 */

#include "gic.h"
#include "platform.h"
#include "config.h"
#include "smp.h"
#include "uart.h"
#include "debug.h"
#include <stddef.h>

/* Default to GICv2 if not specified */
#ifndef GIC_VERSION
#define GIC_VERSION 2
#endif

/*
 * GIC Distributor registers (GICD) - common to v2 and v3
 */
#define GICD_BASE           GIC_DIST_BASE

#define GICD_CTLR           (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_TYPER          (*(volatile uint32_t *)(GICD_BASE + 0x004))
#define GICD_ISENABLER(n)   (*(volatile uint32_t *)(GICD_BASE + 0x100 + 4 * (n)))
#define GICD_ICENABLER(n)   (*(volatile uint32_t *)(GICD_BASE + 0x180 + 4 * (n)))
#define GICD_ISPENDR(n)     (*(volatile uint32_t *)(GICD_BASE + 0x200 + 4 * (n)))
#define GICD_ICPENDR(n)     (*(volatile uint32_t *)(GICD_BASE + 0x280 + 4 * (n)))
#define GICD_IPRIORITYR(n)  (*(volatile uint32_t *)(GICD_BASE + 0x400 + 4 * (n)))
#define GICD_ICFGR(n)       (*(volatile uint32_t *)(GICD_BASE + 0xC00 + 4 * (n)))

#if GIC_VERSION == 2
/* GICv2: Target registers for CPU routing */
#define GICD_ITARGETSR(n)   (*(volatile uint32_t *)(GICD_BASE + 0x800 + 4 * (n)))
#define GICD_SGIR           (*(volatile uint32_t *)(GICD_BASE + 0xF00))
#else
/* GICv3: Affinity routing registers */
#define GICD_IROUTER(n)     (*(volatile uint64_t *)(GICD_BASE + 0x6000 + 8 * (n)))
#endif

/* GICD_CTLR bits - differ between v2 and v3 */
#if GIC_VERSION == 2
#define GICD_CTLR_ENABLE    (1 << 0)
#else
/* GICv3 GICD_CTLR bits */
#define GICD_CTLR_RWP       (1 << 31)   /* Register Write Pending */
#define GICD_CTLR_ARE_NS    (1 << 4)    /* Affinity Routing Enable (NS) */
#define GICD_CTLR_ENABLE_G1 (1 << 1)    /* Enable Group 1 interrupts */
#define GICD_CTLR_ENABLE_G0 (1 << 0)    /* Enable Group 0 interrupts */
#endif

#if GIC_VERSION == 2
/*
 * GICv2 CPU Interface registers (GICC) - memory-mapped
 */
#define GICC_BASE           GIC_CPU_BASE

#define GICC_CTLR           (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR            (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_BPR            (*(volatile uint32_t *)(GICC_BASE + 0x008))
#define GICC_IAR            (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR           (*(volatile uint32_t *)(GICC_BASE + 0x010))

/* GICC_CTLR bits */
#define GICC_CTLR_ENABLE    (1 << 0)

#else /* GIC_VERSION == 3 */

/*
 * GICv3 Redistributor registers (GICR) - per-CPU, memory-mapped
 *
 * Each redistributor has two 64KB frames:
 *   - RD_base: General redistributor registers
 *   - SGI_base: SGI/PPI configuration (at RD_base + 64KB)
 */
#define GICR_BASE           GIC_REDIST_BASE
#define GICR_STRIDE         0x20000     /* 128KB per CPU (2 x 64KB frames) */

/*
 * Per-CPU redistributor base addresses.
 * On some platforms (Jetson Orin), redistributors aren't at sequential
 * stride offsets — there are gaps between clusters. We discover the
 * correct base by walking the GICR chain using GICR_TYPER affinity.
 */
static uintptr_t gicr_cpu_base[MAX_CPUS];

/* Calculate redistributor base for a CPU (must be initialized first) */
static inline uintptr_t gicr_rd_base(uint32_t cpu)
{
    if (gicr_cpu_base[cpu])
        return gicr_cpu_base[cpu];
    /* Fallback to stride calculation (for boot CPU before discovery) */
    return GICR_BASE + (cpu * GICR_STRIDE);
}

static inline uintptr_t gicr_sgi_base(uint32_t cpu)
{
    return gicr_rd_base(cpu) + 0x10000;
}

/* GICR_RD registers */
#define GICR_CTLR(cpu)      (*(volatile uint32_t *)(gicr_rd_base(cpu) + 0x000))
#define GICR_WAKER(cpu)     (*(volatile uint32_t *)(gicr_rd_base(cpu) + 0x014))
#define GICR_TYPER(cpu)     (*(volatile uint64_t *)(gicr_rd_base(cpu) + 0x008))

/* GICR_SGI registers (for PPIs and SGIs) */
#define GICR_IGROUPR0(cpu)      (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0x080))
#define GICR_ISENABLER0(cpu)    (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0x100))
#define GICR_ICENABLER0(cpu)    (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0x180))
#define GICR_ISPENDR0(cpu)      (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0x200))
#define GICR_ICPENDR0(cpu)      (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0x280))
#define GICR_IPRIORITYR(cpu, n) (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0x400 + 4 * (n)))
#define GICR_ICFGR0(cpu)        (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0xC00))
#define GICR_ICFGR1(cpu)        (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0xC04))
#define GICR_IGRPMODR0(cpu)     (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0xD00))

/* GICR_WAKER bits */
#define GICR_WAKER_ProcessorSleep   (1 << 1)
#define GICR_WAKER_ChildrenAsleep   (1 << 2)

/*
 * GICv3 CPU Interface - system registers (ICC_*)
 *
 * These are accessed via MRS/MSR instructions, not memory-mapped.
 * We use individual inline functions instead of macros to be C23 compliant.
 */

/* ICC register bits */
#define ICC_SRE_SRE         (1 << 0)    /* Use system registers */
#define ICC_CTLR_EOImode    (1 << 1)    /* EOI mode control */
#define ICC_IGRPEN1_Enable  (1 << 0)    /* Interrupt Group 1 Enable */

/* Read ICC_SRE_EL1 */
static inline uint64_t icc_read_sre(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(val));
    return val;
}

/* Write ICC_SRE_EL1 — ISB required, affects subsequent system register access */
static inline void icc_write_sre(uint64_t val)
{
    __asm__ volatile("msr ICC_SRE_EL1, %0" :: "r"(val));
    __asm__ volatile("isb");
}

/* Write ICC_PMR_EL1 (Priority Mask) */
static inline void icc_write_pmr(uint64_t val)
{
    __asm__ volatile("msr ICC_PMR_EL1, %0" :: "r"(val));
}

/* Write ICC_BPR1_EL1 (Binary Point) */
static inline void icc_write_bpr1(uint64_t val)
{
    __asm__ volatile("msr ICC_BPR1_EL1, %0" :: "r"(val));
}

/* Write ICC_CTLR_EL1 (Control) */
static inline void icc_write_ctlr(uint64_t val)
{
    __asm__ volatile("msr ICC_CTLR_EL1, %0" :: "r"(val));
}

/* Write ICC_IGRPEN1_EL1 (Group 1 Enable) — ISB required, enables interrupt delivery */
static inline void icc_write_igrpen1(uint64_t val)
{
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(val));
    __asm__ volatile("isb");
}

/* Read ICC_IAR1_EL1 (Interrupt Acknowledge) */
static inline uint64_t icc_read_iar1(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(val));
    return val;
}

/* Write ICC_EOIR1_EL1 (End of Interrupt) */
static inline void icc_write_eoir1(uint64_t val)
{
    __asm__ volatile("msr ICC_EOIR1_EL1, %0" :: "r"(val));
    __asm__ volatile("isb");  /* Ensure EOI takes effect before next instruction */
}

/* Write ICC_SGI1R_EL1 (SGI generation) */
static inline void icc_write_sgi1r(uint64_t val)
{
    __asm__ volatile("msr ICC_SGI1R_EL1, %0" :: "r"(val));
}

#endif /* GIC_VERSION */

/* Special interrupt IDs */
#define GIC_SPURIOUS_INT    1023

#if GIC_VERSION == 3
/*
 * Get current CPU ID from MPIDR.
 * Only used by GICv3 code.
 */
static uint32_t get_cpu_id(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));

    /* Use the SMP logical map for proper MPIDR→CPU ID translation.
     * On Jetson Orin, Aff0=0 for all cores — the core ID is in Aff1/Aff2.
     * cpu_logical_id() handles all platform MPIDR encodings. */
    int id = cpu_logical_id(mpidr);
    if (id >= 0) return (uint32_t)id;

    /* Fallback for boot CPU before SMP init populates the map */
    return 0;
}
#endif

#if GIC_VERSION == 2
/*
 * Initialize the GIC distributor (GICv2).
 */
static void __attribute__((unused)) gic_dist_init(void)
{
    /* Read GICD_CTLR before modifying (check firmware/TF-A state) */
    uint32_t ctlr_before = GICD_CTLR;
    DEBUG_PRINT("GICD_CTLR before init: 0x%x", ctlr_before);

    /* NOTE: Do NOT disable the distributor (GICD_CTLR=0).
     * TF-A configures the GIC from EL3 and disabling/re-enabling from
     * non-secure EL1 may reset secure-side state needed for SPI delivery.
     * Instead, configure interrupts incrementally. */

    /* Get number of interrupt lines */
    uint32_t typer = GICD_TYPER;
    uint32_t num_irqs = ((typer & 0x1F) + 1) * 32;

    /* Check GIC architecture revision */
    uint32_t pidr2 = *(volatile uint32_t *)(GICD_BASE + 0xFE8);
    uint32_t arch_rev = (pidr2 >> 4) & 0xF;
    (void)arch_rev; /* Used by DEBUG_PRINT in debug builds */
    (void)pidr2;    /* Used by DEBUG_PRINT in debug builds */
    DEBUG_PRINT("GICv2: %u IRQs, PIDR2=0x%x arch=%u", num_irqs, pidr2, arch_rev);

    /* Disable all interrupts */
    for (uint32_t i = 0; i < num_irqs / 32; i++) {
        GICD_ICENABLER(i) = 0xFFFFFFFF;
    }

    /* Clear all pending interrupts */
    for (uint32_t i = 0; i < num_irqs / 32; i++) {
        GICD_ICPENDR(i) = 0xFFFFFFFF;
    }

    /* Set all interrupts to default priority */
    for (uint32_t i = 0; i < num_irqs / 4; i++) {
        GICD_IPRIORITYR(i) = 0x80808080;
    }

    /* Route all SPIs to CPU 0 */
    for (uint32_t i = 8; i < num_irqs / 4; i++) {
        GICD_ITARGETSR(i) = 0x01010101;
    }

    /* Set all interrupts to level-triggered */
    for (uint32_t i = 2; i < num_irqs / 16; i++) {
        GICD_ICFGR(i) = 0;
    }

    /*
     * Set all interrupts to Group 1 (non-secure).
     * On Pi 5 (and other platforms with security extensions), Group 0
     * interrupts signal as FIQ and Group 1 as IRQ. Since we only
     * handle IRQ in our exception vectors, all interrupts must be Group 1.
     */
    for (uint32_t i = 0; i < num_irqs / 32; i++) {
        *(volatile uint32_t *)(GICD_BASE + 0x080 + 4 * i) = 0xFFFFFFFF;
    }

    /* Clear any active interrupts */
    for (uint32_t i = 0; i < num_irqs / 32; i++) {
        *(volatile uint32_t *)(GICD_BASE + 0x380 + 4 * i) = 0xFFFFFFFF;
    }

    /* Ensure EnableGrp1 is set (don't touch other bits TF-A configured) */
    GICD_CTLR = ctlr_before | 1;

    DEBUG_PRINT("GICD_CTLR after init: 0x%x", GICD_CTLR);
}

#else /* GIC_VERSION == 3 */

/*
 * Wait for register write to complete (GICv3).
 * Times out after ~1M iterations to avoid hanging on hardware faults.
 */
static void gic_wait_rwp(void)
{
    extern int uart_printf(const char *fmt, ...);
    uint32_t timeout = 1000000;
    while (GICD_CTLR & GICD_CTLR_RWP) {
        if (--timeout == 0) {
            uart_printf("[GIC] WARNING: gic_wait_rwp timed out (GICD_CTLR=0x%x)\n",
                        GICD_CTLR);
            return;
        }
        __asm__ volatile("yield");
    }
}

/*
 * Initialize the GIC distributor (GICv3).
 */
static void gic_dist_init(void)
{
    /* Disable distributor while configuring */
    GICD_CTLR = 0;
    gic_wait_rwp();

    /* Get number of interrupt lines */
    uint32_t typer = GICD_TYPER;
    uint32_t num_irqs = ((typer & 0x1F) + 1) * 32;

    DEBUG_PRINT("GICv3: %u interrupt lines", num_irqs);

    /* Disable all SPIs (IRQ 32+) */
    for (uint32_t i = 1; i < num_irqs / 32; i++) {
        GICD_ICENABLER(i) = 0xFFFFFFFF;
    }

    /* Clear all pending SPIs */
    for (uint32_t i = 1; i < num_irqs / 32; i++) {
        GICD_ICPENDR(i) = 0xFFFFFFFF;
    }

    /* Set all SPI priorities to default */
    for (uint32_t i = 8; i < num_irqs / 4; i++) {
        GICD_IPRIORITYR(i) = 0x80808080;
    }

    /* Set all SPIs to level-triggered */
    for (uint32_t i = 2; i < num_irqs / 16; i++) {
        GICD_ICFGR(i) = 0;
    }

    /* Route all SPIs to this CPU using affinity routing */
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint64_t affinity = (mpidr & 0xFF) |                  /* Aff0 */
                        ((mpidr >> 8) & 0xFF) << 8 |      /* Aff1 */
                        ((mpidr >> 16) & 0xFF) << 16 |    /* Aff2 */
                        ((mpidr >> 32) & 0xFF) << 32;     /* Aff3 */

    for (uint32_t irq = 32; irq < num_irqs; irq++) {
        GICD_IROUTER(irq) = affinity;
    }

    /* Enable distributor with affinity routing */
    GICD_CTLR = GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1;
    gic_wait_rwp();
}

/*
 * Discover the redistributor for the current CPU.
 *
 * On platforms with non-contiguous redistributor layouts (e.g., Jetson Orin
 * with dual clusters where there's a gap between cluster 0 and cluster 1),
 * the simple cpu*stride calculation gives wrong addresses.
 *
 * Jetson Orin redistributor addresses (from Linux dmesg):
 *   CPU 0: 0x0F440000  (offset 0x000000)
 *   CPU 1: 0x0F460000  (offset 0x020000)
 *   CPU 2: 0x0F480000  (offset 0x040000)
 *   CPU 3: 0x0F4A0000  (offset 0x060000)
 *   CPU 4: 0x0F500000  (offset 0x0C0000)  ← gap after cluster 0
 *   CPU 5: 0x0F520000  (offset 0x0E0000)
 */
static void gic_discover_redist(uint32_t cpu)
{
#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* Hardcoded from Linux GICv3 redistributor discovery.
     * Offsets from GIC_REDIST_BASE (0x0F440000). */
    static const uint32_t jetson_redist_offset[] = {
        0x000000, 0x020000, 0x040000, 0x060000,  /* cluster 0 */
        0x0C0000, 0x0E0000                         /* cluster 1 */
    };
    if (cpu < 6) {
        gicr_cpu_base[cpu] = GICR_BASE + jetson_redist_offset[cpu];
        DEBUG_PRINT("GICv3: CPU %u → redistributor at 0x%lx",
                    cpu, (unsigned long)gicr_cpu_base[cpu]);
        return;
    }
#endif

    /* Default: sequential stride (works for QEMU, Pi 5) */
    gicr_cpu_base[cpu] = GICR_BASE + (cpu * GICR_STRIDE);
    DEBUG_PRINT("GICv3: CPU %u → redistributor at 0x%lx (stride)",
                cpu, (unsigned long)gicr_cpu_base[cpu]);
}

/*
 * Initialize the GIC redistributor for a CPU (GICv3).
 */
static void gic_redist_init(uint32_t cpu)
{
    /* Discover the correct redistributor for this CPU */
    gic_discover_redist(cpu);

    /* Wake up the redistributor */
    uint32_t waker = GICR_WAKER(cpu);
    waker &= ~GICR_WAKER_ProcessorSleep;
    GICR_WAKER(cpu) = waker;

    /* Wait for children to wake (with timeout) */
    {
        extern int uart_printf(const char *fmt, ...);
        uint32_t timeout = 1000000;
        while (GICR_WAKER(cpu) & GICR_WAKER_ChildrenAsleep) {
            if (--timeout == 0) {
                uart_printf("[GIC] WARNING: gic_redist_init timed out waiting for "
                            "redistributor %u wake (GICR_WAKER=0x%x)\n",
                            cpu, GICR_WAKER(cpu));
                break;
            }
            __asm__ volatile("yield");
        }
    }

    DEBUG_PRINT("GICv3: Redistributor %u awake", cpu);

    /* Attempt to place all SGIs and PPIs in Group 1 Non-secure.
     *
     * On GICv3 with two security states (Jetson, and any SoC with
     * EL3 firmware holding the GIC in secure mode), a Non-secure
     * write to these registers is silently ignored — the Group
     * configuration is owned by EL3. We write 0xFFFFFFFF / 0 anyway
     * as a best-effort hint for platforms without security (QEMU
     * virt, or a future board with a single-security-state GIC),
     * and the write is harmless on platforms that ignore it.
     *
     * Observed on Jetson Orin Nano 2026-04-13: `GICR_IGROUPR0` reads
     * back as 0x0 after the write (i.e. PPIs stay in Group 0 →
     * deliver as FIQ). The Pi 5 capstone work at #99 hit the same
     * class of issue on GICv2. Both platforms instead rely on
     * cooperative preemption (`COOP_PREEMPT`) via CNTPCT polling at
     * schedule() entry. See docs/archive/plans/jetson-capstone-execution-plan.md
     * revision notes v4 for the full chain of reasoning. */
    GICR_IGROUPR0(cpu)  = 0xFFFFFFFF;  /* Group 1 (best-effort) */
    GICR_IGRPMODR0(cpu) = 0x00000000;  /* Group 1 Non-secure */
    /* DSB SY: ensure the Group-register MMIO writes have reached the
     * GIC before the subsequent enable/priority writes. The GICv3 spec
     * requires an explicit barrier for device-type memory writes that
     * must be ordered against other observers; without it, a read-
     * back (e.g. from the diagnostic print) can see the pre-write
     * value even though the store has retired on this CPU. Cheap
     * (one instruction) and matches the ISB-after-ICC-register-write
     * pattern used elsewhere in this file. */
    __asm__ volatile("dsb sy" ::: "memory");

    /* Disable all SGIs and PPIs */
    GICR_ICENABLER0(cpu) = 0xFFFFFFFF;

    /* Clear pending */
    GICR_ICPENDR0(cpu) = 0xFFFFFFFF;

    /* Set default priority for SGIs/PPIs */
    for (uint32_t i = 0; i < 8; i++) {
        GICR_IPRIORITYR(cpu, i) = 0x80808080;
    }

    /* SGIs edge-triggered, PPIs level-triggered */
    GICR_ICFGR0(cpu) = 0;  /* SGIs: edge (but always edge anyway) */
    GICR_ICFGR1(cpu) = 0;  /* PPIs: level */
}

#endif /* GIC_VERSION */

#if GIC_VERSION == 2
/*
 * Initialize the GIC CPU interface (GICv2).
 */
static void gic_cpu_init(void)
{
    /* Set priority mask to allow all priorities */
    GICC_PMR = 0xF0;

    /* No priority grouping (all bits for priority) */
    GICC_BPR = 0;

    /* Enable CPU interface for Group 1 (non-secure IRQ). Preserve the
     * bypass-disable bits (4 = FIQBypDisGrp1, 5 = IRQBypDisGrp1) from
     * whatever boot.S / firmware left in place — clearing them lets
     * GIC nIRQ/nFIQ outputs follow the external bypass signal, which
     * on Pi 5 is not driven and therefore suppresses all GIC-sourced
     * interrupts. Linux GIC driver reads existing value and preserves
     * bypass bits; we match that pattern. */
    uint32_t ctlr = GICC_CTLR;
    uint32_t bypass = ctlr & ((1u << 4) | (1u << 5));
    GICC_CTLR = GICC_CTLR_ENABLE | bypass;
}

#else /* GIC_VERSION == 3 */

/*
 * Initialize the GIC CPU interface (GICv3).
 * Uses system registers instead of memory-mapped interface.
 */
static void gic_cpu_init(void)
{
    uint32_t cpu = get_cpu_id();

    /* Initialize redistributor for this CPU */
    gic_redist_init(cpu);

    /* Enable system register access */
    uint64_t sre = icc_read_sre();
    sre |= ICC_SRE_SRE;
    icc_write_sre(sre);
    __asm__ volatile("isb");

    /* Set priority mask to allow all priorities */
    icc_write_pmr(0xFF);

    /* No priority grouping */
    icc_write_bpr1(0);

    /* Clear EOImode (standard priority drop + deactivation) */
    icc_write_ctlr(0);

    /* Enable Group 1 interrupts */
    icc_write_igrpen1(ICC_IGRPEN1_Enable);

    __asm__ volatile("isb");

    DEBUG_PRINT("GICv3: CPU %u interface initialized", cpu);
}

#endif /* GIC_VERSION */

/*
 * Initialize the GIC.
 */
void gic_init(void)
{
#if GIC_VERSION == 2
    INFO("Initializing GICv2");
    DEBUG_PRINT("  Distributor: 0x%lx", (unsigned long)GICD_BASE);
    DEBUG_PRINT("  CPU interface: 0x%lx", (unsigned long)GICC_BASE);
#else
    INFO("Initializing GICv3");
    DEBUG_PRINT("  Distributor: 0x%lx", (unsigned long)GICD_BASE);
    DEBUG_PRINT("  Redistributor: 0x%lx", (unsigned long)GICR_BASE);
#endif

#if defined(PLATFORM_RASPI5)
    /* On Pi 5, skip full distributor reinit to preserve TF-A's configuration.
     * TF-A sets SPIs to Group 1, enables Group 0. Our dist_init was
     * overwriting this. Only init the CPU interface. */
    {
        uint32_t typer = GICD_TYPER;
        uint32_t num_irqs = ((typer & 0x1F) + 1) * 32;
        uint32_t pidr2 = *(volatile uint32_t *)(GICD_BASE + 0xFE8);
        (void)pidr2;    /* Used by DEBUG_PRINT in debug builds */
        (void)num_irqs; /* Used by DEBUG_PRINT in debug builds */
        DEBUG_PRINT("GICv2: %u IRQs, PIDR2=0x%x arch=%u", num_irqs, pidr2, (pidr2 >> 4) & 0xF);

        /* Just set SPIs to target CPU 0 and default priority */
        for (uint32_t i = 8; i < num_irqs / 4; i++) {
            GICD_ITARGETSR(i) = 0x01010101;
        }
        for (uint32_t i = 8; i < num_irqs / 4; i++) {
            GICD_IPRIORITYR(i) = 0x40404040;  /* Higher priority than timer (0x80) */
        }

        /* Ensure EnableGrp1 is set */
        uint32_t ctlr = GICD_CTLR;
        GICD_CTLR = ctlr | 1;
        DEBUG_PRINT("GICD_CTLR: 0x%x -> 0x%x", ctlr, GICD_CTLR);
    }
    gic_cpu_init();
#else
    gic_dist_init();
    gic_cpu_init();
#endif

    INFO("GIC initialized");
}

/*
 * Enable a specific interrupt.
 *
 * GICD_ISENABLER / GICR_ISENABLER0 are write-1-to-set registers: writing 1
 * to a bit enables that IRQ, writing 0 is ignored. No read-modify-write is
 * required and any RMW (read-or-mask-write) would be incorrect — it would
 * re-enable any IRQs set between the read and write. Always write a single
 * mask bit per call.
 */
void gic_enable_irq(uint32_t irq)
{
#if GIC_VERSION == 3
    if (irq < 32) {
        /* SGI/PPI: use redistributor */
        uint32_t cpu = get_cpu_id();
        GICR_ISENABLER0(cpu) = (1 << irq);
    } else {
        /* SPI: use distributor */
        uint32_t reg = irq / 32;
        uint32_t bit = irq % 32;
        GICD_ISENABLER(reg) = (1 << bit);
    }
#else
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    GICD_ISENABLER(reg) = (1 << bit);
#endif
}

/*
 * Disable a specific interrupt.
 *
 * GICD_ICENABLER / GICR_ICENABLER0 are write-1-to-clear registers: writing
 * 1 to a bit disables that IRQ, writing 0 is ignored. Like ISENABLER, no
 * read-modify-write is required. Writing a single mask bit per call avoids
 * a races that would occur if a RMW read-back were used.
 */
void gic_disable_irq(uint32_t irq)
{
#if GIC_VERSION == 3
    if (irq < 32) {
        /* SGI/PPI: use redistributor */
        uint32_t cpu = get_cpu_id();
        GICR_ICENABLER0(cpu) = (1 << irq);
    } else {
        /* SPI: use distributor */
        uint32_t reg = irq / 32;
        uint32_t bit = irq % 32;
        GICD_ICENABLER(reg) = (1 << bit);
    }
#else
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    GICD_ICENABLER(reg) = (1 << bit);
#endif
}

/*
 * Set interrupt priority.
 */
void gic_set_priority(uint32_t irq, uint8_t priority)
{
#if GIC_VERSION == 3
    if (irq < 32) {
        /* SGI/PPI: use redistributor */
        uint32_t cpu = get_cpu_id();
        uint32_t reg = irq / 4;
        uint32_t offset = (irq % 4) * 8;
        uint32_t val = GICR_IPRIORITYR(cpu, reg);
        val &= ~(0xFF << offset);
        val |= (priority << offset);
        GICR_IPRIORITYR(cpu, reg) = val;
    } else {
        /* SPI: use distributor */
        uint32_t reg = irq / 4;
        uint32_t offset = (irq % 4) * 8;
        uint32_t val = GICD_IPRIORITYR(reg);
        val &= ~(0xFF << offset);
        val |= (priority << offset);
        GICD_IPRIORITYR(reg) = val;
    }
#else
    uint32_t reg = irq / 4;
    uint32_t offset = (irq % 4) * 8;
    uint32_t val = GICD_IPRIORITYR(reg);
    val &= ~(0xFF << offset);
    val |= (priority << offset);
    GICD_IPRIORITYR(reg) = val;
#endif
}

/*
 * Acknowledge an interrupt.
 */
uint32_t gic_acknowledge(void)
{
#if GIC_VERSION == 3
    return (uint32_t)(icc_read_iar1() & 0x3FF);
#else
    return GICC_IAR & 0x3FF;
#endif
}

/*
 * Signal end of interrupt.
 */
void gic_end_interrupt(uint32_t irq)
{
#if GIC_VERSION == 3
    icc_write_eoir1(irq);
#else
    GICC_EOIR = irq;
#endif
}

/*
 * Check if an interrupt is pending.
 */
int gic_is_pending(uint32_t irq)
{
#if GIC_VERSION == 3
    if (irq < 32) {
        /* SGI/PPI: use redistributor */
        uint32_t cpu = get_cpu_id();
        return (GICR_ISPENDR0(cpu) >> irq) & 1;
    } else {
        /* SPI: use distributor */
        uint32_t reg = irq / 32;
        uint32_t bit = irq % 32;
        return (GICD_ISPENDR(reg) >> bit) & 1;
    }
#else
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    return (GICD_ISPENDR(reg) >> bit) & 1;
#endif
}

/*
 * Send a software-generated interrupt.
 */
void gic_send_sgi(uint32_t irq, uint32_t target_cpu)
{
#if GIC_VERSION == 3
    /*
     * GICv3 uses ICC_SGI1R_EL1 system register.
     * Format: [55:48] = Aff3, [39:32] = Aff2, [23:16] = Aff1, [15:0] = target list
     * We assume single-cluster (Aff1=Aff2=Aff3=0), so target list is 1<<cpu
     */
    uint64_t sgi_val = ((uint64_t)(irq & 0xF) << 24) |  /* INTID */
                       (1UL << target_cpu);              /* Target list for Aff0 */
    icc_write_sgi1r(sgi_val);
    __asm__ volatile("isb");
#else
    GICD_SGIR = ((1U << target_cpu) << 16) | (irq & 0xF);
#endif
}

/*
 * Per-CPU GIC initialization.
 * Called by each secondary CPU after boot.
 * Only initializes the CPU interface (distributor is shared).
 */
void gic_percpu_init(void)
{
    gic_cpu_init();
}

#if GIC_VERSION == 2
/*
 * Set interrupt target CPU(s) for an SPI (GICv2).
 *
 * The ITARGETSR registers control which CPUs can receive each interrupt.
 * Each interrupt has an 8-bit field where each bit represents a CPU.
 */
int gic_set_affinity(uint32_t irq, uint32_t cpu_mask)
{
    /* Only SPIs (32+) can have their affinity changed */
    if (irq < GIC_SPI_START) {
        return -1;  /* SGIs and PPIs are per-CPU, cannot be routed */
    }

    uint32_t reg = irq / 4;
    uint32_t offset = (irq % 4) * 8;

    /* Read current value, update this IRQ's byte, write back */
    uint32_t val = GICD_ITARGETSR(reg);
    val &= ~(0xFF << offset);
    val |= ((cpu_mask & 0xFF) << offset);
    GICD_ITARGETSR(reg) = val;

    return 0;
}

/*
 * Get interrupt target CPU(s) for an SPI (GICv2).
 */
uint32_t gic_get_affinity(uint32_t irq)
{
    /* Only SPIs (32+) have configurable affinity */
    if (irq < GIC_SPI_START) {
        return 0;
    }

    uint32_t reg = irq / 4;
    uint32_t offset = (irq % 4) * 8;

    return (GICD_ITARGETSR(reg) >> offset) & 0xFF;
}

/*
 * Route all SPIs away from a CPU (GICv2).
 *
 * Timer IRQs are PPIs and are unaffected - each CPU still gets its own
 * timer interrupt for scheduler preemption.
 */
void gic_exclude_cpu_from_spis(uint32_t cpu)
{
    if (cpu >= 8) {
        return;  /* GICv2 supports max 8 CPUs */
    }

    uint8_t cpu_bit = (1 << cpu);

    /* Get number of interrupt lines from TYPER */
    uint32_t typer = GICD_TYPER;
    uint32_t num_irqs = ((typer & 0x1F) + 1) * 32;

    /* Update all SPI target registers (starting at IRQ 32) */
    for (uint32_t irq = GIC_SPI_START; irq < num_irqs; irq++) {
        uint32_t reg = irq / 4;
        uint32_t offset = (irq % 4) * 8;

        uint32_t val = GICD_ITARGETSR(reg);
        val &= ~(cpu_bit << offset);  /* Clear this CPU's bit */
        GICD_ITARGETSR(reg) = val;
    }

    DEBUG_PRINT("GIC: Excluded CPU %u from SPI routing", cpu);
}

/*
 * Restore SPI routing to include a CPU (GICv2).
 */
void gic_include_cpu_in_spis(uint32_t cpu)
{
    if (cpu >= 8) {
        return;
    }

    uint8_t cpu_bit = (1 << cpu);

    /* Get number of interrupt lines from TYPER */
    uint32_t typer = GICD_TYPER;
    uint32_t num_irqs = ((typer & 0x1F) + 1) * 32;

    /* Update all SPI target registers (starting at IRQ 32) */
    for (uint32_t irq = GIC_SPI_START; irq < num_irqs; irq++) {
        uint32_t reg = irq / 4;
        uint32_t offset = (irq % 4) * 8;

        uint32_t val = GICD_ITARGETSR(reg);
        val |= (cpu_bit << offset);  /* Set this CPU's bit */
        GICD_ITARGETSR(reg) = val;
    }

    DEBUG_PRINT("GIC: Included CPU %u in SPI routing", cpu);
}

#else /* GIC_VERSION == 3 */

/*
 * GICv3 affinity routing — MPIDR-correct, dual-cluster safe.
 *
 * GICD_IROUTER<n> encodes the SPI target as MPIDR affinity bits
 * (Aff0/Aff1/Aff2/Aff3) plus the IRM bit at [31] selecting between
 * route-to-any-PE (IRM=1) and route-to-specific-PE (IRM=0).
 *
 * Earlier revisions of this driver hardcoded `affinity = cpu` (Aff0
 * holds the logical CPU number). That assumes a single-cluster layout.
 * Jetson Orin Nano is dual-cluster (Aff2.Aff1 = 0.0, 0.1, 0.2, 0.3,
 * 1.2, 1.3 for CPUs 0..5) — every CPU has Aff0=0; the cluster
 * differentiator is Aff1/Aff2. The hardcoded form wrote nonsense
 * affinity values that targeted no actual CPU; on the Jetson it
 * surfaced as a kernel panic when `sched_isolate_core(1)` ran
 * `gic_include_cpu_in_spis(1)` and re-routed half the SPIs to a
 * nonexistent affinity (issue #909).
 *
 * cpu_logical_map[] (populated by smp_init from PSCI / DT) is the
 * canonical logical-CPU → MPIDR table — same source of truth used by
 * the trampoline path (PR #647). All GICv3 affinity sites now resolve
 * through it. The mask matches TF-A's gicd_irouter_val_from_mpidr()
 * (`~/slmos-ref/tf-a/drivers/arm/gic/v3/gicv3_private.h:172`):
 * keep Aff0..Aff3, clear MPIDR_EL1's U / MT / reserved bits.
 */

/* IRM bit at [31]: 0 = route to single PE specified by affinity,
 * 1 = route to any participating PE (1-of-N routing). Functions in
 * this file always use single-PE routing for predictable behaviour. */
#define IROUTER_IRM_PE          (0ULL << 31)

/* MPIDR affinity mask — keeps Aff0..Aff3, clears the MPIDR_EL1
 * U (bit 30), MT (bit 24), and reserved bits. Matches TF-A's
 * MPIDR_AFFINITY_MASK for AArch64. */
#define IROUTER_AFF_MASK        0xFF00FFFFFFULL

/* Maximum SPI count the GICv3 spec allows is IRQ 1019 (32..1019).
 * GICv4-style ESPIs are not used; sticking to the classic range
 * keeps the per-CPU save table small (124 bytes / CPU). */
#define GIC_MAX_SPIS            988  /* IRQs 32..1019 inclusive */
#define GIC_SPI_BITMAP_BYTES    ((GIC_MAX_SPIS + 7) / 8)

/*
 * Per-CPU bitmap recording which SPIs we re-routed AWAY from `cpu`
 * during the last gic_exclude_cpu_from_spis(cpu) call. The matching
 * gic_include_cpu_in_spis(cpu) restores exactly those SPIs back to
 * `cpu` and clears the bits. Static BSS storage; no allocator
 * required. ~744 bytes for MAX_CPUS=6.
 *
 * Each bit position B corresponds to SPI (GIC_SPI_START + B), i.e.
 * IRQ 32 is bit 0, IRQ 33 is bit 1, etc.
 */
static uint8_t spi_excluded_by[MAX_CPUS][GIC_SPI_BITMAP_BYTES];

static inline bool spi_excluded_test(uint32_t cpu, uint32_t spi_off)
{
    if (cpu >= MAX_CPUS || spi_off >= GIC_MAX_SPIS) {
        return false;
    }
    return (spi_excluded_by[cpu][spi_off / 8] >> (spi_off % 8)) & 1u;
}

static inline void spi_excluded_set(uint32_t cpu, uint32_t spi_off)
{
    if (cpu >= MAX_CPUS || spi_off >= GIC_MAX_SPIS) {
        return;
    }
    spi_excluded_by[cpu][spi_off / 8] |= (uint8_t)(1u << (spi_off % 8));
}

static inline void spi_excluded_clear(uint32_t cpu, uint32_t spi_off)
{
    if (cpu >= MAX_CPUS || spi_off >= GIC_MAX_SPIS) {
        return;
    }
    spi_excluded_by[cpu][spi_off / 8] &= (uint8_t)~(1u << (spi_off % 8));
}

/*
 * Convert a logical CPU id into a GICD_IROUTER value targeting that CPU.
 *
 * Returns the affinity-and-IRM-bit value to write into IROUTER. Caller
 * is responsible for any RWP synchronisation.
 *
 * Defensive: if `logical_cpu` is out of range or not in the logical
 * map (cpu_logical_map slot zero), returns 0 — which on most GICv3
 * implementations means "route to MPIDR{0,0,0,0}", i.e. typically the
 * boot CPU. Out-of-range writes are filtered at the API boundary
 * (gic_set_affinity, gic_exclude_cpu_from_spis); this fallback exists
 * only for the unreachable-by-construction branch and is logged.
 */
static uint64_t gic_irouter_val_from_cpu(uint32_t logical_cpu)
{
    if (logical_cpu >= cpu_count) {
        return 0;
    }
    uint64_t mpidr = cpu_logical_map[logical_cpu];
    return (mpidr & IROUTER_AFF_MASK) | IROUTER_IRM_PE;
}

/*
 * Reverse lookup: given an IROUTER value (or any MPIDR-shaped value),
 * find which logical CPU it targets. Returns the bitmask `1 << logical`
 * on match, or 0 if no logical CPU matches.
 */
static uint32_t gic_cpu_mask_from_irouter(uint64_t irouter)
{
    uint64_t aff = irouter & IROUTER_AFF_MASK;
    for (uint32_t i = 0; i < cpu_count; i++) {
        if ((cpu_logical_map[i] & IROUTER_AFF_MASK) == aff) {
            return 1u << i;
        }
    }
    return 0;
}

/*
 * Set interrupt target CPU for an SPI (GICv3).
 *
 * cpu_mask is interpreted as a single CPU ID (lowest set bit). Returns
 * 0 on success, -1 if irq is not an SPI or no valid CPU is set.
 */
int gic_set_affinity(uint32_t irq, uint32_t cpu_mask)
{
    if (irq < GIC_SPI_START) {
        return -1;  /* SGIs and PPIs are per-CPU, cannot be routed */
    }

    /* Find first CPU in mask. */
    uint32_t cpu = 0;
    while (cpu < 32 && !(cpu_mask & (1u << cpu))) {
        cpu++;
    }
    if (cpu >= cpu_count) {
        return -1;
    }

    GICD_IROUTER(irq) = gic_irouter_val_from_cpu(cpu);
    return 0;
}

/*
 * Get interrupt target CPU for an SPI (GICv3).
 *
 * Returns a bitmask (`1 << logical_cpu`) for API compatibility with
 * GICv2, or 0 if the current IROUTER value doesn't map to any logical
 * CPU in `cpu_logical_map[]`.
 */
uint32_t gic_get_affinity(uint32_t irq)
{
    if (irq < GIC_SPI_START) {
        return 0;
    }
    return gic_cpu_mask_from_irouter(GICD_IROUTER(irq));
}

/*
 * Route all SPIs away from a CPU (GICv3).
 *
 * For every SPI currently targeting `cpu`, re-route to CPU 0 and
 * record the move in `spi_excluded_by[cpu]`. The paired
 * `gic_include_cpu_in_spis(cpu)` walks the recorded bitmap and
 * restores routing to `cpu`.
 *
 * Out-of-range `cpu`, including out-of-range logical-map slots, is a
 * no-op (returns silently). Re-entering after a successful exclude
 * without an include in between is also safe: only currently-
 * targeting SPIs are recorded, so a second exclude finds nothing.
 */
void gic_exclude_cpu_from_spis(uint32_t cpu)
{
    if (cpu >= cpu_count || cpu >= MAX_CPUS) {
        return;
    }

    uint64_t cpu_aff   = gic_irouter_val_from_cpu(cpu) & IROUTER_AFF_MASK;
    uint64_t cpu0_irouter = gic_irouter_val_from_cpu(0);

    uint32_t typer = GICD_TYPER;
    uint32_t num_irqs = ((typer & 0x1F) + 1) * 32;
    if (num_irqs > GIC_SPI_START + GIC_MAX_SPIS) {
        num_irqs = GIC_SPI_START + GIC_MAX_SPIS;
    }

    for (uint32_t irq = GIC_SPI_START; irq < num_irqs; irq++) {
        uint64_t current = GICD_IROUTER(irq);
        if ((current & IROUTER_AFF_MASK) == cpu_aff) {
            GICD_IROUTER(irq) = cpu0_irouter;
            spi_excluded_set(cpu, irq - GIC_SPI_START);
        }
    }

    DEBUG_PRINT("GIC: Excluded CPU %u from SPI routing (aff %llx)",
                cpu, (unsigned long long)cpu_aff);
}

/*
 * Restore SPI routing for a CPU (GICv3).
 *
 * Walks the bitmap populated by the matching
 * `gic_exclude_cpu_from_spis(cpu)` and restores each recorded SPI's
 * routing back to `cpu`. Bits are cleared as they're processed so a
 * later exclude/include pair starts from a clean slate.
 */
void gic_include_cpu_in_spis(uint32_t cpu)
{
    if (cpu >= cpu_count || cpu >= MAX_CPUS) {
        return;
    }

    uint64_t cpu_irouter = gic_irouter_val_from_cpu(cpu);

    uint32_t typer = GICD_TYPER;
    uint32_t num_irqs = ((typer & 0x1F) + 1) * 32;
    if (num_irqs > GIC_SPI_START + GIC_MAX_SPIS) {
        num_irqs = GIC_SPI_START + GIC_MAX_SPIS;
    }

    for (uint32_t irq = GIC_SPI_START; irq < num_irqs; irq++) {
        uint32_t spi_off = irq - GIC_SPI_START;
        if (spi_excluded_test(cpu, spi_off)) {
            GICD_IROUTER(irq) = cpu_irouter;
            spi_excluded_clear(cpu, spi_off);
        }
    }

    DEBUG_PRINT("GIC: Included CPU %u in SPI routing", cpu);
}

#endif /* GIC_VERSION */

/* ============================================================================
 * IRQ handler registration table (GIC-version-agnostic)
 *
 * Small static table of (irq, handler) pairs looked up by the EL1 IRQ
 * dispatch in kernel/arch/arm64/exceptions.c when none of the
 * compile-time cases match. Lets drivers whose IRQ number isn't known
 * at compile time (e.g. virtio-mmio devices, where the IRQ follows
 * the slot the device lands in) bind a handler at init time.
 *
 * Sizing (16 slots): current driver set uses one slot (virtio-net on
 * QEMU_VIRT). Headroom budgeted for x86-64 MSI-X (item 3 of #204
 * follow-up), a future stuck-descriptor watchdog slot, and the
 * pending RP1/Jetson NIC drivers — still leaves ~10 spare. Linear
 * scan is fine at this size; the lookup runs in IRQ context where
 * the cache line is hot anyway. Revisit if the table ever exceeds
 * ~32 entries.
 *
 * Concurrency model: registrations happen during single-threaded
 * driver init, lookups run from IRQ context on any CPU the SPI is
 * affine to. Uses release/acquire atomics on `.fn` so a concurrent
 * lookup sees `.irq` before `.fn`. Without that ordering, a reader
 * on another CPU could observe a non-NULL `.fn` while `.irq` is
 * still stale and dispatch to the wrong handler. Today the
 * `gic_enable_irq` that follows `gic_register_handler` provides an
 * MMIO barrier on the registering CPU, but relying on that is
 * fragile — the API advertises itself as generic, so the atomics
 * make the contract explicit.
 *
 * Deliberately outside the GIC_VERSION conditionals — the table
 * layout doesn't depend on v2 vs v3, only on having an IRQ dispatch
 * that can consult it.
 * ============================================================================ */

#define GIC_MAX_REGISTERED_HANDLERS 16

struct gic_handler_entry {
    uint32_t       irq;      /* GIC interrupt ID */
    gic_handler_fn fn;       /* NULL means "free slot" */
};

static struct gic_handler_entry gic_handlers[GIC_MAX_REGISTERED_HANDLERS];

int gic_register_handler(uint32_t irq, gic_handler_fn handler)
{
    if (!handler) {
        return -1;
    }
    for (unsigned i = 0; i < GIC_MAX_REGISTERED_HANDLERS; i++) {
        gic_handler_fn existing = __atomic_load_n(&gic_handlers[i].fn,
                                                  __ATOMIC_ACQUIRE);
        if (existing == NULL) {
            gic_handlers[i].irq = irq;
            /* Release-store publishes .irq + .fn together to any
             * lookup on another CPU. */
            __atomic_store_n(&gic_handlers[i].fn, handler,
                             __ATOMIC_RELEASE);
            return 0;
        }
    }
    return -1;
}

int gic_unregister_handler(uint32_t irq)
{
    for (unsigned i = 0; i < GIC_MAX_REGISTERED_HANDLERS; i++) {
        gic_handler_fn existing = __atomic_load_n(&gic_handlers[i].fn,
                                                  __ATOMIC_ACQUIRE);
        if (existing != NULL && gic_handlers[i].irq == irq) {
            __atomic_store_n(&gic_handlers[i].fn, NULL,
                             __ATOMIC_RELEASE);
            return 0;
        }
    }
    return -1;
}

gic_handler_fn gic_lookup_handler(uint32_t irq)
{
    for (unsigned i = 0; i < GIC_MAX_REGISTERED_HANDLERS; i++) {
        /* Acquire-load pairs with the release-store in register. If
         * we observe .fn != NULL, .irq is guaranteed visible. */
        gic_handler_fn fn = __atomic_load_n(&gic_handlers[i].fn,
                                            __ATOMIC_ACQUIRE);
        if (fn != NULL && gic_handlers[i].irq == irq) {
            return fn;
        }
    }
    return NULL;
}
