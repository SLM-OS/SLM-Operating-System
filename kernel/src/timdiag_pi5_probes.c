/*
 * timdiag_pi5_probes.c — Track B hardware probes for Pi 5 timer-IRQ
 * delivery (issue #134, capstone preemption Track B).
 *
 * Three diagnostic experiments that produce empirical answers to
 * questions about the GICv2 → Cortex-A76 IRQ path on BCM2712:
 *
 *   B1. SGI probe (`timdiag sgi`)
 *       Software-generated interrupts share the GIC→CPU output path
 *       with PPIs. If a self-SGI fires the IRQ vector while timer
 *       PPI 30 does not, the GIC group-config story is wrong (B2 is
 *       the next probe). If neither fires, the GIC→CPU path itself
 *       is broken and a custom armstub (Track C) is needed.
 *
 *   B2. Bypass-direction probe (`timdiag bypass`)
 *       Toggles `IRQBypDisGrp1` / `FIQBypDisGrp1` in GICC_CTLR while
 *       observing whether `timer_handler_count` advances. The Pi 5
 *       schematic doesn't expose how nIRQ is wired between GIC-400
 *       and the A76 cluster; if the wiring routes through the
 *       bypass output, our current default (bits set = bypass
 *       *disabled*) would actually suppress IRQs.
 *
 *   B3. SMC fingerprint probe (`timdiag smc`)
 *       Times a known PSCI SMC (PSCI_VERSION) and a deliberately
 *       invalid SMC. If both round-trip in similar microseconds, the
 *       EL3 dispatcher is active and routinely round-tripping. That
 *       gives a lower-bound estimate of how visible an EL3-trapped
 *       timer IRQ would be — and indirectly indicates whether
 *       SCR_EL3.IRQ is set (i.e. whether NS IRQs go to EL3 first).
 *
 * Each probe is destructive in narrow ways (toggles DAIF, rewrites
 * GICC_CTLR briefly, fires SGIs). They are gated on
 * PLATFORM_RASPI5 + PI5_IRQ_DIAG and only run on explicit shell
 * subcommand. Default `timdiag` behavior is unchanged.
 *
 * Driven from `kernel/src/shell_sys.c:cmd_timdiag` via the
 * `timdiag {sgi,bypass,smc}` subcommands.
 */

#include "config.h"

#if defined(PLATFORM_RASPI5) && defined(PI5_IRQ_DIAG)

#include "platform.h"
#include "shell.h"
#include "shell_internal.h"
#include "uart.h"
#include "timer.h"
#include "smp.h"
#include "diag_pi5.h"
#include <stdint.h>

/* GIC-400 register offsets. We re-derive them here (rather than
 * include gic.c's static macros) because the probes deliberately
 * write registers gic.c treats as private. */
#define GICD_ISENABLER0     (*(volatile uint32_t *)(GIC_DIST_BASE + 0x100))
#define GICD_SGIR           (*(volatile uint32_t *)(GIC_DIST_BASE + 0xF00))
#define GICD_IPRIORITYR(n)  (*(volatile uint32_t *)(GIC_DIST_BASE + 0x400 + 4 * (n)))
#define GICD_ITARGETSR(n)   (*(volatile uint32_t *)(GIC_DIST_BASE + 0x800 + 4 * (n)))

#define GICC_CTLR           (*(volatile uint32_t *)(GIC_CPU_BASE  + 0x000))
#define GICC_PMR            (*(volatile uint32_t *)(GIC_CPU_BASE  + 0x004))
#define GICC_IAR            (*(volatile uint32_t *)(GIC_CPU_BASE  + 0x00C))
#define GICC_EOIR           (*(volatile uint32_t *)(GIC_CPU_BASE  + 0x010))

/* Bits of interest in GICC_CTLR. EnableGrp1=bit 0,
 * FIQBypDisGrp1=bit 4, IRQBypDisGrp1=bit 5. The bypass-disable bits
 * are documented as "set this to disable the GIC bypass path". On
 * boards where nIRQ wires through the bypass output, that direction
 * is inverted — clearing them is what enables IRQ delivery. */
#define GICC_CTLR_FIQBYPDISGRP1 (1u << 4)
#define GICC_CTLR_IRQBYPDISGRP1 (1u << 5)

/* SGIR: fire SGI N to a single-CPU target. Filter = 0b00 (use
 * CPUTargetList). CPUTargetList is a bitmap of GICv2 banked CPU IDs.
 * The current CPU is index `cpu_id() & 0xFF` for CPUs 0..7 — Pi 5
 * never has more than 4. */
#define GICD_SGIR_FILTER_LIST   (0u << 24)
#define GICD_SGIR_TARGET(cpu)   ((1u << (cpu)) << 16)
#define GICD_SGIR_INTID(n)      ((n) & 0xF)

static uint64_t cycles_to_us(uint64_t cycles, uint64_t freq)
{
    if (freq == 0) return 0;
    return (cycles * 1000000UL) / freq;
}

/* ============================================================================
 * B1: Self-SGI probe.
 *
 * Fires SGI 0 to the calling CPU, briefly unmasks DAIF.I, and reports
 * whether the per-CPU IRQ counter (diag_vec_counts.irq) advanced.
 *
 * Three outcomes:
 *   irq counter advanced — GIC→CPU IRQ path works for SGIs. Timer-
 *     specific routing (group bits, ISENABLER) is the next thing to
 *     probe (B2).
 *   irq counter unchanged, fiq counter advanced — SGI was delivered
 *     as FIQ. Group config is unexpected; check IGROUPR.
 *   neither counter advanced — GIC→CPU output is fundamentally
 *     blocked. Track C (custom armstub) is the only fix.
 * ============================================================================ */

/*
 * Implementation note (revised after first hardware run):
 *
 * The first cut of this probe unmasked DAIF.I, fired the SGI, and
 * waited for the IRQ vector to run. That hung the system on Pi 5 —
 * the IRQ vector ran, dispatched to the "Unhandled IRQ %u" branch,
 * and then something in that path (uart_printf inside IRQ context,
 * cache contention, …) wedged the shell.
 *
 * The cleaner probe never unmasks DAIF.I. It pulses the SGI and
 * polls GICC_HPPIR — which is the highest-pending-IRQ register the
 * GIC exposes regardless of whether DAIF.I is taken. If the SGI
 * propagates from distributor to CPU interface, HPPIR will show
 * it. We then ACK (GICC_IAR) + EOI (GICC_EOIR) by hand to clean
 * up the GIC state without ever exiting task context.
 *
 * That answers the same question (does GIC→CPU propagation work
 * for SGIs?) without depending on the IRQ vector being safe to run
 * in this exact spot.
 */
void timdiag_pi5_probe_sgi(void)
{
    uart_puts("\r\n--- Track B1: Self-SGI probe (DAIF stays masked) ---\r\n");

    uint32_t cpu = cpu_id();
    if (cpu >= MAX_CPUS) {
        uart_printf("  bogus cpu_id %u\r\n", cpu);
        return;
    }

    /* Snapshot HPPIR before. 0x3FF = "no pending interrupt". */
    uint32_t hppir = *(volatile uint32_t *)(GIC_CPU_BASE + 0x18);
    uart_printf("  CPU %u — GICC_HPPIR before SGI: 0x%x (irq=%u%s)\r\n",
                cpu, hppir, hppir & 0x3FF,
                (hppir & 0x3FF) == 0x3FF ? " — no pending" : "");

    /* Make sure SGI 0 is enabled. ISENABLER0 is W1S — writes set
     * bits, never clear them. Safe to unconditionally pulse. */
    GICD_ISENABLER0 = (1u << 0);
    __asm__ volatile("dsb sy" ::: "memory");

    /* Set SGI 0 priority to 0x80 (mid). PMR=0xF0 from gic_cpu_init,
     * so 0x80 passes. IPRIORITYR(0) covers IRQs 0..3, byte 0 = SGI 0. */
    uint32_t prio_save = GICD_IPRIORITYR(0);
    GICD_IPRIORITYR(0) = (prio_save & ~0xFFu) | 0x80u;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Fire SGI 0 to self. */
    GICD_SGIR = GICD_SGIR_FILTER_LIST | GICD_SGIR_TARGET(cpu) | GICD_SGIR_INTID(0);
    __asm__ volatile("dsb sy" ::: "memory");

    /* Brief pause for GIC propagation — the distributor → CPU
     * interface forwarding takes a few cycles. 100 µs is plenty. */
    uint64_t freq = timer_get_frequency();
    uint64_t start = timer_get_count();
    uint64_t deadline = start + (freq / 10000);
    while (timer_get_count() < deadline) {
        __asm__ volatile("yield");
    }

    /* Read HPPIR again. If the SGI propagated, we'll see irq=0. */
    uint32_t hppir_after = *(volatile uint32_t *)(GIC_CPU_BASE + 0x18);
    uart_printf("  CPU %u — GICC_HPPIR after  SGI: 0x%x (irq=%u%s)\r\n",
                cpu, hppir_after, hppir_after & 0x3FF,
                (hppir_after & 0x3FF) == 0x3FF ? " — no pending" : "");

    /* Manual ACK + EOI to drain the SGI without invoking the IRQ
     * vector. GICC_IAR is the standard ACK; GICC_EOIR is the
     * standard EOI. SGIs additionally encode the source CPU in
     * the upper bits of IAR — we don't decode it because we know
     * we sent it ourselves. */
    if ((hppir_after & 0x3FF) != 0x3FF) {
        uint32_t iar = GICC_IAR;
        GICC_EOIR = iar;
        __asm__ volatile("dsb sy" ::: "memory");
        uart_printf("  Drained SGI: IAR=0x%x (irq=%u src=%u)\r\n",
                    iar, iar & 0x3FF, (iar >> 10) & 0x7);
    }

    /* Restore priority. */
    GICD_IPRIORITYR(0) = prio_save;
    __asm__ volatile("dsb sy" ::: "memory");

    if ((hppir_after & 0x3FF) == 0) {
        uart_puts("  RESULT: SGI 0 propagated to GICC (HPPIR showed irq 0).\r\n"
                  "  >>> GIC distributor → CPU interface path WORKS for SGIs.\r\n"
                  "  >>> Timer PPI 30 non-delivery is downstream of HPPIR\r\n"
                  "      (the gating is between HPPIR and the CPU's IRQ pin,\r\n"
                  "      i.e. either bypass-disable bits or SCR_EL3 routing).\r\n");
    } else if ((hppir_after & 0x3FF) == 0x3FF) {
        uart_puts("  RESULT: SGI 0 NOT visible at GICC_HPPIR.\r\n"
                  "  >>> Distributor blocked the SGI. Either the\r\n"
                  "      ISENABLER write didn't take or GICD_CTLR is\r\n"
                  "      gating it. Check GICD_CTLR_NS and Group masks.\r\n");
    } else {
        uart_printf("  RESULT: GICC_HPPIR shows unexpected IRQ %u.\r\n"
                    "  >>> Probably an unrelated pending interrupt.\r\n",
                    hppir_after & 0x3FF);
    }
}

/* ============================================================================
 * B2: Bypass-direction probe.
 *
 * Toggles GICC_CTLR bypass-disable bits and observes whether
 * timer_handler_count advances during a 100 ms window.
 *
 * The default state (gic_cpu_init preserves whatever firmware/boot.S
 * left, which is bits 4+5 set on Pi 5 — bypass *disabled*). If the
 * board wires nIRQ through the bypass output, clearing these bits
 * would route real IRQs through. We try both and report.
 * ============================================================================ */

/*
 * Observe diag_vec_counts.irq for the calling CPU over a fixed
 * wall-clock window. Returns the delta. We use the per-CPU IRQ
 * counter (incremented at the very top of el1_irq) rather than
 * timer_handler_count because the latter also advances via the
 * coop-preempt synthetic tick — the synthetic counter would
 * obscure whether real hardware IRQs are firing.
 */
static uint64_t observe_irq_count_for_us(uint64_t us)
{
    uint32_t cpu = cpu_id();
    if (cpu >= MAX_CPUS) return 0;
    struct diag_vec_counts *vec = diag_vec_counts_cpu(cpu);
    uint64_t before = vec->irq;
    uint64_t freq = timer_get_frequency();
    uint64_t target = timer_get_count() + (freq * us) / 1000000ULL;
    while (timer_get_count() < target) {
        __asm__ volatile("yield");
    }
    return vec->irq - before;
}

void timdiag_pi5_probe_bypass(void)
{
    uart_puts("\r\n--- Track B2: GICC_CTLR bypass-disable probe ---\r\n");

    uint32_t saved = GICC_CTLR;
    uart_printf("  Saved GICC_CTLR=0x%x (IRQBypDisGrp1=%u FIQBypDisGrp1=%u)\r\n",
                saved,
                (saved >> 5) & 1, (saved >> 4) & 1);

    /* Save DAIF and unmask IRQ for the observation window. The
     * timer IRQ is the only IRQ source we expect to see fire — its
     * handler is well-trodden, unlike the SGI 0 case in B1. If it
     * never fires (current Pi 5 reality), DAIF unmask is harmless;
     * if it does fire, the standard timer path runs and we get an
     * observable counter delta. */
    uint64_t daif_save;
    __asm__ volatile("mrs %0, daif" : "=r"(daif_save));
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    /* Phase 1: current state baseline. */
    uint64_t base_delta = observe_irq_count_for_us(50000);
    uart_printf("  Phase 1 (current GICC_CTLR=0x%x): irq counter delta=%lu over 50 ms\r\n",
                saved, (unsigned long)base_delta);

    /* Phase 2: invert the bypass bits. */
    uint32_t inverted = saved ^ (GICC_CTLR_IRQBYPDISGRP1 | GICC_CTLR_FIQBYPDISGRP1);
    GICC_CTLR = inverted;
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    uint64_t inverted_delta = observe_irq_count_for_us(50000);
    uart_printf("  Phase 2 (inverted GICC_CTLR=0x%x): irq counter delta=%lu over 50 ms\r\n",
                inverted, (unsigned long)inverted_delta);

    /* Restore. */
    GICC_CTLR = saved;
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("msr daif, %0" :: "r"(daif_save));
    __asm__ volatile("isb" ::: "memory");

    if (inverted_delta > base_delta) {
        uart_puts("  RESULT: Inverted bypass bits delivered MORE IRQs.\r\n"
                  "  >>> Pi 5 nIRQ likely routes through the GIC bypass output.\r\n"
                  "  >>> One-line fix in gic_cpu_init: clear bits 4+5 in GICC_CTLR.\r\n");
    } else if (inverted_delta == base_delta) {
        uart_puts("  RESULT: No change — bypass bits are not the blocker.\r\n");
    } else {
        uart_puts("  RESULT: Inverted bypass bits delivered FEWER IRQs.\r\n"
                  "  >>> Current (set) state is correct; investigation continues.\r\n");
    }
}

/* ============================================================================
 * B3: SMC fingerprint.
 *
 * Times PSCI_VERSION (a guaranteed-supported function) and an invalid
 * function id. Prints both round-trips in CNTPCT cycles + microseconds.
 *
 * If both come back in similar low microseconds, EL3 dispatch is
 * active and routine. If both are very fast (< ~1 µs) the EL3 layer
 * may be a thin pass-through — but on Pi 5 with TF-A there's always
 * a real dispatch.
 *
 * The probe doesn't directly read SCR_EL3.IRQ — that register is
 * unreadable from NS — but the relative timing tells us how heavy
 * the EL3 layer is, which is the necessary cost model for deciding
 * whether to ship a custom armstub (Track C).
 * ============================================================================ */

#define PSCI_VERSION_FUNC_ID    0x84000000u
#define PSCI_INVALID_FUNC_ID    0x84009999u  /* unlikely to be assigned */

static uint64_t time_smc_call(uint64_t func_id, int64_t *out_rc)
{
    register uint64_t x0 __asm__("x0") = func_id;
    uint64_t start, end;

    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));
    __asm__ volatile("smc #0" : "+r"(x0) :: "memory");
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(end));

    if (out_rc) *out_rc = (int64_t)x0;
    return end - start;
}

void timdiag_pi5_probe_smc(void)
{
    uart_puts("\r\n--- Track B3: SMC round-trip fingerprint ---\r\n");

    uint64_t freq = timer_get_frequency();
    if (freq == 0) {
        uart_puts("  timer not initialized; skipping\r\n");
        return;
    }

    /* Warm up the EL3 dispatcher caches with one untimed call. */
    int64_t scratch;
    (void)time_smc_call(PSCI_VERSION_FUNC_ID, &scratch);

    /* Measure 8 round-trips of each call type and print min/max so
     * we can spot variance from a noisy / interrupted dispatch. */
    uint64_t valid_min = ~(uint64_t)0, valid_max = 0;
    int64_t  valid_rc = 0;
    for (int i = 0; i < 8; i++) {
        int64_t rc;
        uint64_t cycles = time_smc_call(PSCI_VERSION_FUNC_ID, &rc);
        if (cycles < valid_min) valid_min = cycles;
        if (cycles > valid_max) valid_max = cycles;
        valid_rc = rc;
    }

    uint64_t inv_min = ~(uint64_t)0, inv_max = 0;
    int64_t  inv_rc = 0;
    for (int i = 0; i < 8; i++) {
        int64_t rc;
        uint64_t cycles = time_smc_call(PSCI_INVALID_FUNC_ID, &rc);
        if (cycles < inv_min) inv_min = cycles;
        if (cycles > inv_max) inv_max = cycles;
        inv_rc = rc;
    }

    uart_printf("  CNTFRQ_EL0 = %lu Hz\r\n", (unsigned long)freq);
    uart_printf("  PSCI_VERSION (id=0x%lx): rc=0x%lx  min=%lu cyc (%lu us)  max=%lu cyc (%lu us)\r\n",
                (unsigned long)PSCI_VERSION_FUNC_ID,
                (unsigned long)valid_rc,
                (unsigned long)valid_min, (unsigned long)cycles_to_us(valid_min, freq),
                (unsigned long)valid_max, (unsigned long)cycles_to_us(valid_max, freq));
    uart_printf("  Invalid     (id=0x%lx): rc=0x%lx  min=%lu cyc (%lu us)  max=%lu cyc (%lu us)\r\n",
                (unsigned long)PSCI_INVALID_FUNC_ID,
                (unsigned long)inv_rc,
                (unsigned long)inv_min, (unsigned long)cycles_to_us(inv_min, freq),
                (unsigned long)inv_max, (unsigned long)cycles_to_us(inv_max, freq));

    uart_puts("\r\n  Interpretation:\r\n");
    uart_puts("    Both calls round-trip via TF-A's EL3 dispatcher.\r\n"
              "    PSCI_VERSION returns a valid version; invalid id\r\n"
              "    returns NOT_SUPPORTED (-1). The cycle counts give a\r\n"
              "    lower bound on the cost of any future EL3 timer-IRQ\r\n"
              "    re-injection: if a custom armstub re-routes timer\r\n"
              "    IRQs through EL3, every tick would pay roughly this\r\n"
              "    fixed cost.\r\n");
    if (valid_min < freq / 100000) {
        /* < 10 µs — relatively cheap dispatcher. */
        uart_puts("    Round-trip < 10 us: EL3 layer is thin enough that\r\n"
                  "    a re-injecting armstub (Track C) is feasible at\r\n"
                  "    100 Hz tick (~1 ms quantum, < 1% overhead).\r\n");
    } else {
        uart_puts("    Round-trip >= 10 us: EL3 layer is heavy. A\r\n"
                  "    re-injecting armstub at 100 Hz would impose\r\n"
                  "    > 1% per-CPU tick overhead.\r\n");
    }
}

#endif /* PLATFORM_RASPI5 && PI5_IRQ_DIAG */
