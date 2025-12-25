/*
 * gic.c - GIC driver for SLM-OS
 *
 * Generic Interrupt Controller driver supporting GICv2 and GICv3.
 * - GICv2: Used by QEMU virt machine (memory-mapped CPU interface)
 * - GICv3: Used by Jetson Orin Nano (system register CPU interface)
 */

#include "gic.h"
#include "platform.h"
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

/* Calculate redistributor base for a CPU */
static inline uintptr_t gicr_rd_base(uint32_t cpu)
{
    return GICR_BASE + (cpu * GICR_STRIDE);
}

static inline uintptr_t gicr_sgi_base(uint32_t cpu)
{
    return GICR_BASE + (cpu * GICR_STRIDE) + 0x10000;
}

/* GICR_RD registers */
#define GICR_CTLR(cpu)      (*(volatile uint32_t *)(gicr_rd_base(cpu) + 0x000))
#define GICR_WAKER(cpu)     (*(volatile uint32_t *)(gicr_rd_base(cpu) + 0x014))
#define GICR_TYPER(cpu)     (*(volatile uint64_t *)(gicr_rd_base(cpu) + 0x008))

/* GICR_SGI registers (for PPIs and SGIs) */
#define GICR_ISENABLER0(cpu)    (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0x100))
#define GICR_ICENABLER0(cpu)    (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0x180))
#define GICR_ISPENDR0(cpu)      (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0x200))
#define GICR_ICPENDR0(cpu)      (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0x280))
#define GICR_IPRIORITYR(cpu, n) (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0x400 + 4 * (n)))
#define GICR_ICFGR0(cpu)        (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0xC00))
#define GICR_ICFGR1(cpu)        (*(volatile uint32_t *)(gicr_sgi_base(cpu) + 0xC04))

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

/* Write ICC_SRE_EL1 */
static inline void icc_write_sre(uint64_t val)
{
    __asm__ volatile("msr ICC_SRE_EL1, %0" :: "r"(val));
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

/* Write ICC_IGRPEN1_EL1 (Group 1 Enable) */
static inline void icc_write_igrpen1(uint64_t val)
{
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(val));
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
    return mpidr & 0xFF;  /* Aff0 = CPU ID within cluster */
}
#endif

#if GIC_VERSION == 2
/*
 * Initialize the GIC distributor (GICv2).
 */
static void gic_dist_init(void)
{
    /* Disable distributor while configuring */
    GICD_CTLR = 0;

    /* Get number of interrupt lines */
    uint32_t typer = GICD_TYPER;
    uint32_t num_irqs = ((typer & 0x1F) + 1) * 32;

    DEBUG_PRINT("GICv2: %u interrupt lines", num_irqs);

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

    /* Enable distributor */
    GICD_CTLR = GICD_CTLR_ENABLE;
}

#else /* GIC_VERSION == 3 */

/*
 * Wait for register write to complete (GICv3).
 */
static void gic_wait_rwp(void)
{
    while (GICD_CTLR & GICD_CTLR_RWP) {
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
 * Initialize the GIC redistributor for a CPU (GICv3).
 */
static void gic_redist_init(uint32_t cpu)
{
    /* Wake up the redistributor */
    uint32_t waker = GICR_WAKER(cpu);
    waker &= ~GICR_WAKER_ProcessorSleep;
    GICR_WAKER(cpu) = waker;

    /* Wait for children to wake */
    while (GICR_WAKER(cpu) & GICR_WAKER_ChildrenAsleep) {
        __asm__ volatile("yield");
    }

    DEBUG_PRINT("GICv3: Redistributor %u awake", cpu);

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
    GICC_PMR = 0xFF;

    /* No priority grouping (all bits for priority) */
    GICC_BPR = 0;

    /* Enable CPU interface */
    GICC_CTLR = GICC_CTLR_ENABLE;
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

    gic_dist_init();
    gic_cpu_init();

    INFO("GIC initialized");
}

/*
 * Enable a specific interrupt.
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
    GICD_SGIR = (target_cpu << 16) | (irq & 0xF);
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
 * Set interrupt target CPU for an SPI (GICv3).
 *
 * GICv3 uses IROUTER registers with MPIDR-style affinity routing.
 * cpu_mask is interpreted as a single CPU ID (lowest set bit).
 */
int gic_set_affinity(uint32_t irq, uint32_t cpu_mask)
{
    /* Only SPIs (32+) can have their affinity changed */
    if (irq < GIC_SPI_START) {
        return -1;  /* SGIs and PPIs are per-CPU, cannot be routed */
    }

    /* Find first CPU in mask (GICv3 routes to single CPU, not mask) */
    uint32_t cpu = 0;
    while (cpu < 32 && !(cpu_mask & (1 << cpu))) {
        cpu++;
    }
    if (cpu >= 32) {
        return -1;  /* No CPU in mask */
    }

    /* Build affinity value (assuming single cluster) */
    uint64_t affinity = cpu;  /* Aff0 = CPU number */
    GICD_IROUTER(irq) = affinity;

    return 0;
}

/*
 * Get interrupt target CPU for an SPI (GICv3).
 *
 * Returns a bitmask for API compatibility with GICv2.
 */
uint32_t gic_get_affinity(uint32_t irq)
{
    /* Only SPIs (32+) have configurable affinity */
    if (irq < GIC_SPI_START) {
        return 0;
    }

    uint64_t affinity = GICD_IROUTER(irq);
    uint32_t cpu = affinity & 0xFF;  /* Aff0 */

    return (cpu < 32) ? (1 << cpu) : 0;
}

/*
 * Route all SPIs away from a CPU (GICv3).
 *
 * Re-routes SPIs currently targeting this CPU to CPU 0.
 */
void gic_exclude_cpu_from_spis(uint32_t cpu)
{
    /* Get number of interrupt lines from TYPER */
    uint32_t typer = GICD_TYPER;
    uint32_t num_irqs = ((typer & 0x1F) + 1) * 32;

    /* Check each SPI's routing */
    for (uint32_t irq = GIC_SPI_START; irq < num_irqs; irq++) {
        uint64_t affinity = GICD_IROUTER(irq);
        if ((affinity & 0xFF) == cpu) {
            /* Re-route to CPU 0 */
            GICD_IROUTER(irq) = 0;
        }
    }

    DEBUG_PRINT("GIC: Excluded CPU %u from SPI routing", cpu);
}

/*
 * Restore SPI routing to include a CPU (GICv3).
 *
 * Routes SPIs currently on CPU 0 to this CPU (for load balancing).
 * This is a simplified implementation; production code might be smarter.
 */
void gic_include_cpu_in_spis(uint32_t cpu)
{
    /* Get number of interrupt lines from TYPER */
    uint32_t typer = GICD_TYPER;
    uint32_t num_irqs = ((typer & 0x1F) + 1) * 32;

    /* Route some SPIs to this CPU (every Nth SPI) */
    uint32_t count = 0;
    for (uint32_t irq = GIC_SPI_START; irq < num_irqs; irq++) {
        if ((count % (cpu + 1)) == cpu) {
            GICD_IROUTER(irq) = cpu;
        }
        count++;
    }

    DEBUG_PRINT("GIC: Included CPU %u in SPI routing", cpu);
}

#endif /* GIC_VERSION */
