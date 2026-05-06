/*
 * armstub8-2712.S - EL3 stub for Raspberry Pi 5 (Track C of #134)
 *
 * The Pi 5 firmware loads this as "armstub8-2712.bin" and executes it
 * at EL3 before jumping to the kernel at 0x80000. By default the Pi 5
 * firmware ships TF-A's BL31 as the armstub; replacing it with this
 * minimal stub trades TF-A's services (PSCI, SDEI, fault handling)
 * for control over SCR_EL3 — which is the exact register that gates
 * NS IRQ delivery (see Track B findings in PR #639).
 *
 * Purpose (in order of importance):
 *   1. Clear SCR_EL3.IRQ and SCR_EL3.FIQ so non-secure interrupts
 *      deliver to EL1/EL2 instead of being trapped to EL3. This is
 *      the single change that should unblock the Pi 5 hardware
 *      timer-IRQ path (PR #639's `timdiag sgi` confirmed timer PPI
 *      30 IS visible at GICC_HPPIR — the gating is between HPPIR
 *      and the CPU's IRQ pin, which is exactly what SCR_EL3 routes).
 *   2. Configure GIC-400 interrupt groups (GICD_IGROUPR) from secure
 *      state, since these registers are RAZ/WI from non-secure EL2/EL1.
 *   3. Set up system registers for a clean EL3→EL2 transition.
 *
 * KNOWN GAP (intentionally not solved here):
 *   This stub does NOT provide a PSCI implementation. TF-A's BL31
 *   handles CPU_ON / CPU_OFF / SYSTEM_RESET; replacing it with this
 *   stub leaves SMC instructions trapping to a non-existent EL3
 *   handler. SMP secondary CPU bring-up (which uses psci_cpu_on)
 *   will fail. Two paths forward:
 *
 *   (a) Fork ARM-software/arm-trusted-firmware/plat/rpi/rpi5 and add
 *       the SCR_EL3.IRQ/FIQ clear to TF-A's bl31_setup. Build a
 *       custom bl31.bin and ship that as armstub8-2712.bin. PSCI
 *       remains intact.
 *   (b) Add a minimal PSCI 0.2 implementation to this stub
 *       (CPU_ON, CPU_OFF, AFFINITY_INFO, SYSTEM_RESET — about
 *       200 lines of EL3 code per the proposal). Keeps this file
 *       self-contained but is more code than (a).
 *
 * (a) is the cleaner path because TF-A's PSCI is well-tested.
 * docs/pi5-armstub-track-c.md tracks the work.
 *
 * The stub does NOT enable GIC forwarding (GICD_CTLR, GICC_CTLR) —
 * that is handled by the kernel's gic_init() after exception vectors
 * are set up.
 *
 * Build:
 *   make armstub-pi5    (writes build/armstub/armstub8-2712.bin)
 *
 * Deploy:
 *   Copy build/armstub/armstub8-2712.bin to the Pi 5 boot partition
 *   AND add `armstub=armstub8-2712.bin` to config.txt.
 *
 * WARNING: Deploying this binary as-is will break SMP because PSCI
 * stops working. Use only with the matching TF-A path (a) or PSCI
 * implementation (b) ready.
 */

#define GIC_DISTB       0x107FFF9000
#define GICD_CTRLR      0x000
#define GICD_TYPER      0x004
#define GICD_IGROUPR    0x080
#define KERNEL_ENTRY    0x80000

.globl _start
_start:
    /* Save DTB pointer (firmware passes it in x0) */
    mov     x19, x0

    /* Check if we're at EL3 */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #3
    b.ne    .Lnot_el3

    /*
     * EL3: Configure GIC groups and system registers, then drop to EL2.
     */

    /* --- GIC Group Configuration (requires EL3 secure access) --- */

    /* Only primary core sets GICD_IGROUPR (distributor is shared) */
    mrs     x0, mpidr_el1
    tst     x0, #0x3
    b.ne    .Lel3_sysregs

    /* Set all GICD_IGROUPR to 0xFFFFFFFF (all interrupts Group 1) */
    ldr     x0, =GIC_DISTB
    ldr     w1, [x0, #GICD_TYPER]
    and     w1, w1, #0x1F
    add     w1, w1, #1
    add     x2, x0, #GICD_IGROUPR
    mov     w3, #0
    mov     w4, #0xFFFFFFFF
1:  str     w4, [x2, x3, lsl #2]
    add     w3, w3, #1
    cmp     w3, w1
    b.lt    1b

    /* Ensure GIC writes complete */
    dsb     sy

.Lel3_sysregs:
    /* --- System Register Setup for EL3→EL2 Transition --- */

    /*
     * CPTR_EL3 = 0: Do not trap FP/SIMD/SVE access to EL3.
     * Without this, any FP/SIMD instruction at EL2/EL1 traps to EL3
     * which no longer has a handler after ERET → system hang.
     */
    msr     cptr_el3, xzr

    /*
     * SCR_EL3: read-modify-write to preserve IMPLEMENTATION-DEFINED
     * bits but explicitly fix the IRQ-routing problem from #134.
     *
     *   NS  [0]  = 1  Non-secure (EL2/EL1 run in non-secure world)
     *   IRQ [1]  = 0  Route NS IRQs to EL1/EL2 (the bit Track B
     *                 isolated as the likely Pi 5 blocker — see
     *                 PR #639 `timdiag sgi` showing HPPIR=30 with
     *                 zero IRQs delivered).
     *   FIQ [2]  = 0  Route NS FIQs to EL1/EL2 (symmetry with IRQ;
     *                 the kernel's el1_fiq_handler is ready for
     *                 GICC_AIAR-based dispatch under PI5_FIQ_TIMER).
     *   SMD [7]  = 0  ALLOW SMC — required by any PSCI implementation
     *                 we may chain to. SMD=1 would brick SMP.
     *   HCE [8]  = 1  Enable HVC instruction
     *   RW  [10] = 1  EL2 is AArch64
     *
     * Leave bits 4 (RES1), 5 (RES1), 6 (EA), and high bits as TF-A
     * set — those are platform-implementation-defined and clobbering
     * them risks subtle regressions (Pi 5's TF-A relies on EA=1 for
     * synchronous error abort routing for example).
     */
    mrs     x0, scr_el3
    orr     x0, x0, #(1 << 0)               /* NS = 1 */
    bic     x0, x0, #(1 << 1)               /* IRQ = 0 — NS IRQ → EL2/EL1 */
    bic     x0, x0, #(1 << 2)               /* FIQ = 0 — NS FIQ → EL2/EL1 */
    orr     x0, x0, #(1 << 8)               /* HCE = 1 */
    orr     x0, x0, #(1 << 10)              /* RW = 1 */
    bic     x0, x0, #(1 << 7)               /* SMD = 0 (allow SMC for PSCI) */
    msr     scr_el3, x0

    /*
     * SCTLR_EL2: Safe initial value for EL2.
     * Matches the standard RPi firmware armstub value.
     *   - MMU disabled
     *   - Caches disabled
     *   - Alignment check enabled
     *   - Various RES1 bits set per ARM ARM
     */
    ldr     x0, =0x30c50838
    msr     sctlr_el2, x0

    /*
     * ACTLR_EL3: Allow EL2 access to implementation-defined features.
     * Set to all-1s to avoid trapping implementation-defined registers.
     */
    mov     x0, #0xFFFFFFFF
    msr     actlr_el3, x0

    /*
     * MDCR_EL3 = 0: Don't trap debug/PMU access to EL3.
     */
    msr     mdcr_el3, xzr

    /* SPSR_EL3: Return to EL2h with all exceptions masked */
    mov     x0, #0x3c9
    msr     spsr_el3, x0

    /* ELR_EL3: Return address is the kernel entry point */
    ldr     x0, =KERNEL_ENTRY
    msr     elr_el3, x0

    /* Restore DTB pointer for kernel */
    mov     x0, x19

    /* Synchronize all register writes and pending MMIO */
    dsb     sy
    isb
    eret

.Lnot_el3:
    /*
     * Not at EL3 — can't configure GIC groups (IGROUPR is RAZ/WI).
     * Just jump to the kernel at the current EL, preserving DTB.
     */
    mov     x0, x19
    ldr     x1, =KERNEL_ENTRY
    br      x1
