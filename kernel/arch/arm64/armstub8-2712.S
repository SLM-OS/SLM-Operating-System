/*
 * armstub8-2712.S - Minimal EL3 stub for Raspberry Pi 5
 *
 * The Pi 5 firmware loads this as "armstub8-2712.bin" and executes it at EL3
 * before jumping to the kernel. Its purpose is to configure the GIC-400
 * interrupt groups so that all interrupts are Group 1 (delivered as IRQ to
 * non-secure EL1/EL2).
 *
 * Without this, GICD_IGROUPR defaults to 0 (Group 0 = FIQ), and non-secure
 * code cannot change it (RAZ/WI from non-secure state).
 *
 * Based on Raspberry Pi foundation's armstub8.S from:
 * https://github.com/raspberrypi/tools/tree/master/armstubs
 *
 * Build:
 *   aarch64-none-elf-gcc -nostdlib -nostartfiles -Wl,--section-start=.text=0 \
 *       -o armstub8-2712.elf armstub8-2712.S
 *   aarch64-none-elf-objcopy -O binary armstub8-2712.elf armstub8-2712.bin
 */

/* GIC-400 addresses for BCM2712 (Pi 5) */
#define GIC_DISTB       0x107FFF9000
#define GIC_CPUB        0x107FFFA000

/* GIC register offsets */
#define GICD_CTRLR      0x000
#define GICD_TYPER      0x004
#define GICD_IGROUPR    0x080
#define GICC_CTRLR      0x000
#define GICC_PMR        0x004

.globl _start
_start:
    /*
     * Entry from Pi 5 firmware at EL3.
     * x0 = DTB address (preserve for kernel)
     */

    /* Save DTB address */
    mov     x4, x0

    /* Only primary core configures the GIC distributor */
    mrs     x5, mpidr_el1
    tst     x5, #0x3
    b.ne    .Lcpu_init

    /*
     * Primary core: configure GIC distributor
     */
    ldr     x6, =GIC_DISTB

    /* Read GICD_TYPER to get number of interrupt groups */
    ldr     w7, [x6, #GICD_TYPER]
    and     w7, w7, #0x1F
    add     w7, w7, #1              /* Number of 32-bit IGROUPR registers */

    /* Set all interrupts to Group 1 (non-secure) */
    add     x8, x6, #GICD_IGROUPR
    mov     w9, #0
.Lgrp_loop:
    mov     w10, #0xFFFFFFFF
    str     w10, [x8, x9, lsl #2]
    add     w9, w9, #1
    cmp     w9, w7
    b.lt    .Lgrp_loop

    /* Enable distributor: Group 0 + Group 1 */
    mov     w7, #3
    str     w7, [x6, #GICD_CTRLR]

.Lcpu_init:
    /*
     * All cores: configure GIC CPU interface
     */
    ldr     x6, =GIC_CPUB

    /* GICC_PMR = 0xFF (allow all priorities) */
    mov     w7, #0xFF
    str     w7, [x6, #GICC_PMR]

    /*
     * GICC_CTLR = 0x1E7:
     *   bit 0: EnableGrp0 = 1
     *   bit 1: EnableGrp1 = 1
     *   bit 2: AckCtl = 1
     *   bit 5: FIQBypDisGrp0 = 1
     *   bit 6: IRQBypDisGrp1 = 1
     *   bit 7: FIQBypDisGrp1 = 1
     *   bit 8: EOImodeNS = 1
     */
    ldr     w7, =0x1E7
    str     w7, [x6, #GICC_CTRLR]

    /*
     * Configure EL2/EL1 for the kernel.
     * Set SCR_EL3 to route IRQs to non-secure EL1/EL2.
     */

    /* Disable coprocessor traps */
    mov     x7, #0x33ff
    msr     cptr_el3, x7

    /* SCR_EL3: NS=1 (non-secure), RW=1 (EL2 is AArch64), HCE=1 (HVC enabled) */
    mov     x7, #0x5b1
    msr     scr_el3, x7

    /* SCTLR_EL2: safe initial value */
    ldr     x7, =0x30c50830
    msr     sctlr_el2, x7

    /* Return to EL2 with DAIF masked */
    mov     x7, #0x3c9             /* EL2h with DAIF masked */
    msr     spsr_el3, x7

    /* Jump to kernel load address (0x80000) */
    ldr     x7, =0x80000
    msr     elr_el3, x7

    /* Restore DTB address in x0 */
    mov     x0, x4

    eret
