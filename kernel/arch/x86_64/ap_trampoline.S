/*
 * ap_trampoline.S - Application Processor startup trampoline for x86-64 SMP
 *
 * When an AP receives a SIPI (Startup IPI), it begins execution in 16-bit
 * real mode at the physical address specified in the SIPI vector field
 * (vector × 4096). This code transitions the AP from real mode through
 * protected mode to long mode, loads the BSP's page tables, GDT, and IDT,
 * sets up a per-CPU stack, then jumps to the 64-bit AP entry point.
 *
 * Memory layout (configured by BSP before sending SIPI):
 *   AP_TRAMPOLINE_BASE (0x8000):
 *     [0x000 - 0xF00]  This code (copied here by BSP)
 *     [0xF00]          ap_boot_params structure (filled by BSP):
 *       +0x00: cr3      (uint64_t) — BSP's PML4 physical address
 *       +0x08: gdt_ptr  (10 bytes) — GDT pointer (limit + base)
 *       +0x12: idt_ptr  (10 bytes) — IDT pointer (limit + base)
 *       +0x1C: stack    (uint64_t) — per-CPU stack top
 *       +0x24: cpu_id   (uint32_t) — logical CPU ID
 *       +0x28: entry    (uint64_t) — 64-bit entry point address
 *       +0x30: flag     (uint32_t) — AP sets to 1 when running
 *
 * Compiled with: gcc -m16 (or as .code16) and linked to run at 0x8000.
 * The BSP copies this blob to physical address 0x8000 before sending SIPI.
 *
 * SIPI vector = 0x8000 / 4096 = 0x08.
 */

/* Trampoline base address — must match AP_TRAMPOLINE_BASE in platform_x86.c */
#define TRAMP_BASE  0x8000

/* Offsets into ap_boot_params (at TRAMP_BASE + 0xF00) */
#define PARAMS_OFF  0xF00
#define PARAM_CR3   (TRAMP_BASE + PARAMS_OFF + 0x00)
#define PARAM_GDT   (TRAMP_BASE + PARAMS_OFF + 0x08)
#define PARAM_IDT   (TRAMP_BASE + PARAMS_OFF + 0x12)
#define PARAM_STACK (TRAMP_BASE + PARAMS_OFF + 0x1C)
#define PARAM_CPUID (TRAMP_BASE + PARAMS_OFF + 0x24)
#define PARAM_ENTRY (TRAMP_BASE + PARAMS_OFF + 0x28)
#define PARAM_FLAG  (TRAMP_BASE + PARAMS_OFF + 0x30)

.section .text
.code16
.global ap_trampoline_start
.global ap_trampoline_end

ap_trampoline_start:
    /*
     * 16-bit real mode entry point.
     * CS:IP = SIPI_vector:0000 (CS = 0x0800, IP = 0x0000 for base 0x8000).
     * Interrupts are disabled (INIT clears IF).
     */
    cli

    /* Set up real-mode segments to access our data area */
    xor     %ax, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss

    /* Load a temporary 32-bit GDT so we can enter protected mode */
    lgdt    (ap_gdt32_ptr - ap_trampoline_start + TRAMP_BASE)

    /* Enable protected mode (CR0.PE = 1) */
    mov     %cr0, %eax
    or      $0x1, %eax
    mov     %eax, %cr0

    /*
     * Far jump to 32-bit protected mode code.
     * Selector 0x08 = first GDT entry (32-bit code segment).
     */
    .byte   0x66, 0xea                          /* ljmpl prefix */
    .long   ap_pm_entry - ap_trampoline_start + TRAMP_BASE
    .word   0x08

.code32
ap_pm_entry:
    /* Set up 32-bit data segments */
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss

    /* Enable PAE (CR4 bit 5) — required for long mode */
    mov     %cr4, %eax
    or      $0x20, %eax
    mov     %eax, %cr4

    /* Load BSP's PML4 into CR3 */
    mov     PARAM_CR3, %eax
    mov     %eax, %cr3

    /* Enable long mode in EFER MSR (bit 8 = LME) */
    mov     $0xC0000080, %ecx
    rdmsr
    or      $0x100, %eax
    wrmsr

    /* Enable paging (CR0.PG = 1) — activates long mode */
    mov     %cr0, %eax
    or      $0x80000000, %eax
    mov     %eax, %cr0

    /* Load the BSP's 64-bit GDT */
    lgdt    PARAM_GDT

    /*
     * Far jump to 64-bit long mode.
     * Selector 0x08 = 64-bit code segment in BSP's GDT.
     */
    .byte   0xea                                /* ljmpl */
    .long   ap_lm_entry - ap_trampoline_start + TRAMP_BASE
    .word   0x08

.code64
ap_lm_entry:
    /* Set up 64-bit data segments */
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    /* Load IDT */
    lidt    PARAM_IDT

    /* Load per-CPU stack */
    mov     PARAM_STACK, %rsp

    /* Signal BSP: AP is running in long mode */
    movl    $1, PARAM_FLAG

    /* Load logical CPU ID as first argument (RDI) */
    mov     PARAM_CPUID, %edi

    /* Jump to 64-bit AP entry point */
    mov     PARAM_ENTRY, %rax
    jmp     *%rax

    /* Should never reach here */
    ud2

/*
 * Temporary 32-bit GDT for the real→protected mode transition.
 * Only used briefly; replaced by BSP's 64-bit GDT before entering long mode.
 */
.align 16
ap_gdt32:
    .quad   0                                   /* Null descriptor */
    /* 32-bit code: base=0, limit=4GB, DPL=0, code, read, 32-bit */
    .quad   0x00CF9A000000FFFF
    /* 32-bit data: base=0, limit=4GB, DPL=0, data, write */
    .quad   0x00CF92000000FFFF

.align 4
ap_gdt32_ptr:
    .word   ap_gdt32_ptr - ap_gdt32 - 1        /* limit */
    .long   ap_gdt32 - ap_trampoline_start + TRAMP_BASE  /* base (physical) */

ap_trampoline_end:

.section .note.GNU-stack, "", @progbits
