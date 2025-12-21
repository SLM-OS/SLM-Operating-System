/*
 * smp_boot.S - Secondary CPU boot code for SLM-OS
 *
 * This is the entry point for secondary CPUs when woken via PSCI CPU_ON.
 * PSCI places the context_id (logical CPU ID) in x0.
 */

.section .text
.global secondary_entry

/*
 * Secondary CPU entry point.
 *
 * On entry (from PSCI CPU_ON):
 *   x0 = context_id (we pass logical CPU ID)
 *   MMU = off (QEMU) or on (depends on firmware)
 *   Caches = off (typically)
 *   Interrupts = masked
 *
 * Our job:
 *   1. Set up stack for this CPU
 *   2. Enable FPU/SIMD
 *   3. Set exception vectors
 *   4. Jump to C code (secondary_init)
 */
secondary_entry:
    /* Save logical CPU ID (passed in x0 from PSCI context_id) */
    mov     x19, x0

    /*
     * Set up stack for this CPU.
     * Stack = cpu_stacks[cpu_id] + STACK_SIZE
     *
     * cpu_stacks is an array: uint8_t cpu_stacks[MAX_CPUS][STACK_SIZE]
     * STACK_SIZE = 0x4000 (16 KB) = 1 << 14
     *
     * Stack top for CPU n = &cpu_stacks[n][STACK_SIZE]
     *                     = cpu_stacks + (n * STACK_SIZE) + STACK_SIZE
     *                     = cpu_stacks + ((n + 1) * STACK_SIZE)
     *
     * We compute: sp = cpu_stacks + ((cpu_id + 1) << 14)
     */
    ldr     x1, =cpu_stacks
    add     x2, x19, #1             /* x2 = cpu_id + 1 */
    lsl     x2, x2, #14             /* x2 = (cpu_id + 1) * 16KB */
    add     sp, x1, x2              /* sp = cpu_stacks + offset */

    /*
     * Enable FPU and SIMD (NEON).
     * CPACR_EL1 bits [21:20] = 11 enables full access.
     */
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

    /*
     * Set up exception vector table.
     * Use same vectors as primary CPU.
     */
    ldr     x1, =exception_vectors
    msr     vbar_el1, x1
    isb

    /*
     * Jump to C initialization.
     * Pass logical CPU ID as argument.
     */
    mov     x0, x19
    bl      secondary_init

    /*
     * secondary_init should not return.
     * If it does, enter a halt loop.
     */
1:
    wfi
    b       1b

/*
 * Reference to cpu_stacks (defined in smp.c).
 * We need this to be visible to the linker.
 */
.extern cpu_stacks
.extern exception_vectors
.extern secondary_init
