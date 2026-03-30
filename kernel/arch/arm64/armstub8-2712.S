/*
 * armstub8-2712.S - Minimal EL3 stub for Raspberry Pi 5
 *
 * The Pi 5 firmware loads this as "armstub8-2712.bin" and executes it
 * before jumping to the kernel at 0x80000. Its purpose is to configure
 * the GIC-400 interrupt groups from secure state (EL3).
 *
 * Build:
 *   aarch64-none-elf-gcc -nostdlib -nostartfiles -Wl,--section-start=.text=0 \
 *       -o armstub8-2712.elf armstub8-2712.S
 *   aarch64-none-elf-objcopy -O binary armstub8-2712.elf armstub8-2712.bin
 */

#define GIC_DISTB       0x107FFF9000
#define GIC_CPUB        0x107FFFA000
#define GICD_CTRLR      0x000
#define GICD_TYPER      0x004
#define GICD_IGROUPR    0x080
#define GICC_CTRLR      0x000
#define GICC_PMR        0x004
#define KERNEL_ENTRY    0x80000

/*
 * Marker location: write the current EL and a magic value to 0x7FF00
 * (just below the kernel at 0x80000). The kernel can read this to
 * confirm the armstub ran and at what EL.
 */
#define MARKER_ADDR     0x7FF00
#define MARKER_MAGIC    0x53544233  /* "STB3" */

.globl _start
_start:
    /* Save DTB pointer */
    mov     x19, x0

    /* Write marker: current EL + magic + DTB address */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2             /* Extract EL number (bits [3:2]) */
    ldr     x1, =MARKER_ADDR
    str     w0, [x1]               /* marker[0] = current EL */
    ldr     w2, =MARKER_MAGIC
    str     w2, [x1, #4]           /* marker[1] = magic */
    str     x19, [x1, #8]          /* marker[2..3] = DTB address (64-bit) */

    /* Check if we're at EL3 */
    cmp     x0, #3
    b.ne    .Lnot_el3

    /*
     * EL3: Configure GIC and drop to EL2.
     */

    /* Only primary core does distributor setup */
    mrs     x0, mpidr_el1
    tst     x0, #0x3
    b.ne    .Lel3_per_cpu

    /* Set all GICD_IGROUPR to Group 1 */
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

    /* Enable distributor: Group 0 + Group 1 */
    mov     w1, #3
    str     w1, [x0, #GICD_CTRLR]

.Lel3_per_cpu:
    /* Configure GIC CPU interface from secure state */
    ldr     x0, =GIC_CPUB
    mov     w1, #0xFF
    str     w1, [x0, #GICC_PMR]
    ldr     w1, =0x1E7
    str     w1, [x0, #GICC_CTRLR]

    /* SCR_EL3: NS=1, RW=1 (AArch64 EL2), HCE=1, SMD=1 */
    ldr     x0, =0x5b1
    msr     scr_el3, x0

    /* Drop to EL2h with exceptions masked */
    mov     x0, #0x3c9
    msr     spsr_el3, x0
    ldr     x0, =KERNEL_ENTRY
    msr     elr_el3, x0
    mov     x0, x19             /* DTB address */
    isb
    eret

.Lnot_el3:
    /*
     * Not at EL3 — can't configure GIC groups (IGROUPR is RAZ/WI).
     * Just jump to the kernel at the current EL, preserving DTB.
     */
    mov     x0, x19             /* DTB address */
    ldr     x1, =KERNEL_ENTRY
    br      x1
