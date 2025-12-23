/*
 * boot.S - SLM-OS entry point for ARM64
 *
 * This is the first code executed when the kernel is loaded.
 * QEMU loads us at 0x40000000 and jumps here with:
 *   - x0 = DTB address (preserved and passed to kernel_main)
 *   - EL1 exception level
 *   - MMU disabled
 *   - Caches disabled
 *   - SP undefined
 */

.section .text.boot
.global _start

_start:
    /*
     * Save DTB address (x0) to callee-saved register.
     * QEMU passes DTB pointer in x0, which we preserve for kernel_main.
     */
    mov     x19, x0

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
     * Enable FPU and SIMD (NEON).
     * CPACR_EL1 bits [21:20] control access to SIMD/FP:
     *   00 = trap all access
     *   01 = trap EL0 access only
     *   10 = trap all access
     *   11 = no trapping
     * We set bits [21:20] = 11 to enable full access.
     */
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

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
     * Set up exception vector table.
     * VBAR_EL1 holds the base address of the vector table.
     */
    ldr     x1, =exception_vectors
    msr     vbar_el1, x1
    isb

    /*
     * Jump to C code.
     * Pass DTB address as first argument (x0).
     * kernel_main() should not return, but if it does,
     * we fall through to the halt loop.
     */
    mov     x0, x19             /* DTB address from saved register */
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
     * In Phase 2, we'll implement proper multi-core bring-up.
     */
    wfe                         /* Wait For Event */
    b       secondary_spin

/*
 * Note: Exception vector table is now in vectors.S
 */
