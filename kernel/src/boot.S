/*
 * boot.S - SLM-OS entry point for ARM64
 *
 * This is the first code executed when the kernel is loaded.
 * QEMU loads us at 0x40000000 and jumps here with:
 *   - x0 = DTB address (ignored for now)
 *   - EL1 exception level
 *   - MMU disabled
 *   - Caches disabled
 *   - SP undefined
 */

.section .text.boot
.global _start

_start:
    /*
     * Ensure we're running on the primary CPU (core 0).
     * Secondary cores will spin in a wait loop.
     * MPIDR_EL1 bits [7:0] contain the CPU ID (Aff0).
     */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbz     x1, primary_cpu
    b       secondary_spin

primary_cpu:
    /*
     * Set up the stack pointer.
     * Stack grows downward, so point to top of stack area.
     */
    ldr     x1, =__stack_top
    mov     sp, x1

    /*
     * Clear the BSS section.
     * BSS contains uninitialized global/static variables.
     */
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
bss_clear:
    cmp     x1, x2
    b.ge    bss_done
    str     xzr, [x1], #8
    b       bss_clear
bss_done:

    /*
     * Disable interrupts while we initialize.
     * DAIF bits: D=Debug, A=SError, I=IRQ, F=FIQ
     * Setting these bits masks (disables) the corresponding exceptions.
     */
    msr     daifset, #0xF

    /*
     * Jump to C code.
     * kernel_main() should not return, but if it does,
     * we fall through to the halt loop.
     */
    bl      kernel_main

    /* Fall through to halt if kernel_main returns */

halt:
    /*
     * Halt the CPU.
     * WFI = Wait For Interrupt, puts CPU in low-power state.
     */
    wfi
    b       halt

secondary_spin:
    /*
     * Secondary CPUs wait here.
     * In Month 2, we'll implement proper multi-core bring-up.
     */
    wfe                         /* Wait For Event */
    b       secondary_spin

/*
 * Vector table placeholder.
 * Full exception handling will be implemented in Week 4.
 * For now, any exception will hang in an infinite loop.
 */
.balign 0x800
.global vectors
vectors:
    /* Current EL with SP0 */
    b       halt                /* Synchronous */
    .balign 0x80
    b       halt                /* IRQ */
    .balign 0x80
    b       halt                /* FIQ */
    .balign 0x80
    b       halt                /* SError */

    /* Current EL with SPx */
    .balign 0x80
    b       halt                /* Synchronous */
    .balign 0x80
    b       halt                /* IRQ */
    .balign 0x80
    b       halt                /* FIQ */
    .balign 0x80
    b       halt                /* SError */

    /* Lower EL using AArch64 */
    .balign 0x80
    b       halt                /* Synchronous */
    .balign 0x80
    b       halt                /* IRQ */
    .balign 0x80
    b       halt                /* FIQ */
    .balign 0x80
    b       halt                /* SError */

    /* Lower EL using AArch32 */
    .balign 0x80
    b       halt                /* Synchronous */
    .balign 0x80
    b       halt                /* IRQ */
    .balign 0x80
    b       halt                /* FIQ */
    .balign 0x80
    b       halt                /* SError */
