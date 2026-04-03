/*
 * armstub8-2712.S - EL3 stub for Raspberry Pi 5
 *
 * The Pi 5 firmware loads this as "armstub8-2712.bin" and executes it
 * at EL3 before jumping to the kernel at 0x80000.
 *
 * Purpose:
 *   1. Configure GIC-400 interrupt groups (GICD_IGROUPR) from secure state,
 *      since these registers are RAZ/WI from non-secure EL2/EL1.
 *   2. Set up system registers for a clean EL3→EL2 transition.
 *
 * The stub does NOT enable GIC forwarding (GICD_CTLR, GICC_CTLR) —
 * that is handled by the kernel's gic_init() after exception vectors
 * are set up.
 *
 * System registers configured here match the standard Raspberry Pi
 * armstub8.S from the firmware. Missing any of these causes intermittent
 * boot failures (~30-60%) due to traps to EL3 with no handler, or
 * stale system register state from the firmware.
 *
 * Build:
 *   aarch64-none-elf-gcc -nostdlib -nostartfiles -Wl,--section-start=.text=0 \
 *       -o armstub8-2712.elf armstub8-2712.S
 *   aarch64-none-elf-objcopy -O binary armstub8-2712.elf armstub8-2712.bin
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
     * SCR_EL3: Configure security state for EL2.
     *   NS  [0]  = 1  Non-secure (EL2/EL1 run in non-secure world)
     *   IRQ [1]  = 0  Don't route IRQ to EL3
     *   FIQ [2]  = 0  Don't route FIQ to EL3
     *   EA  [3]  = 0  Don't route external aborts to EL3
     *   RES1[4]  = 1  Reserved, must be 1
     *   RES1[5]  = 1  Reserved, must be 1
     *   SMD [7]  = 1  Disable SMC instruction
     *   HCE [8]  = 1  Enable HVC instruction
     *   RW  [10] = 1  EL2 is AArch64
     *   = 0x5b1
     */
    ldr     x0, =0x5b1
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
