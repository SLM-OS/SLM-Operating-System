/*
 * bzimage_entry.S — 64-bit entry stub for the Linux bzImage boot protocol.
 *
 * Only compiled when the kernel is built as a Linux-bzImage wrapper
 * (CMake option BZIMAGE_BUILD=1). Placed by kernel-x86_64-bzimage.ld
 * at phys `KERNEL_PHYS + 0x200` = 0x20000200 — kexec-tools'
 * bzImage64 loader jumps there after it copies the payload to
 * pref_address (0x20000000).
 *
 * State on entry (per Documentation/arch/x86/boot.rst §64-bit Boot Protocol):
 *   CPU    : 64-bit long mode, paging on, interrupts masked
 *   CS     : 0x10 — flat 4 GB exec/read, L=1 (kexec boot GDT)
 *   DS/ES/SS: 0x18 — flat 4 GB r/w (kexec boot GDT)
 *   %rsi   : phys(boot_params) — we ignore it, SLM-OS doesn't use it
 *   %rbx   : 0
 *   others : undefined, including %rsp
 *
 * Unlike the Multiboot2 path (which starts in 32-bit protected mode and
 * uses trampoline32.S to transition to long mode), kexec hands us a
 * fully-initialised 64-bit environment. We only need to:
 *   1. Disable interrupts (already cli, but we'll redo for safety).
 *   2. Reinit COM1 and emit "BZ\r\n" as an early-entry breadcrumb — the
 *      silence we saw with the Multiboot2 loader vs. a visible "BZ"
 *      here is the signal that this entry path actually got control.
 *   3. Load OUR GDT (gdt64_ptr from trampoline32.S), so downstream C
 *      code sees the same segment layout it sees in the bare-metal
 *      boot.
 *   4. Clear multiboot_ptr (we have no MB info here).
 *   5. Fall through to entry64 — which sets up the stack, clears BSS,
 *      and invokes kernel_main_x86.
 *
 * There is no direct caller of kernel_main_x86 from here; reusing
 * entry64 keeps the kexec and GRUB paths converged at the earliest
 * possible point.
 */

.section .bzimage_entry, "ax", @progbits
.code64
.global _start_bzimage

/* Imports from trampoline32.S / entry64.S */
.extern gdt64_ptr
.extern entry64
.extern multiboot_ptr

_start_bzimage:
    cli

    /* ------ 16550 UART reinit (mirrors trampoline32.S) ----------- */
    mov $0x3F9, %dx          /* IER: no interrupts */
    mov $0x00, %al
    out %al, %dx
    mov $0x3FB, %dx          /* LCR: DLAB=1 */
    mov $0x80, %al
    out %al, %dx
    mov $0x3F8, %dx          /* DLL: 1 → 115200 baud */
    mov $0x01, %al
    out %al, %dx
    mov $0x3F9, %dx          /* DLM: 0 */
    mov $0x00, %al
    out %al, %dx
    mov $0x3FB, %dx          /* LCR: 8N1, DLAB=0 */
    mov $0x03, %al
    out %al, %dx
    mov $0x3FA, %dx          /* FCR: enable+clear FIFO */
    mov $0xC7, %al
    out %al, %dx
    mov $0x3FC, %dx          /* MCR: DTR+RTS+OUT2 */
    mov $0x0B, %al
    out %al, %dx

    /* ------ Emit "BZ\r\n" — bzImage entry breadcrumb ------------- */
    mov $0x3F8, %dx          /* THR */
    mov $0x42, %al           /* 'B' */
    out %al, %dx
    mov $0x5A, %al           /* 'Z' */
    out %al, %dx
    mov $0x0D, %al           /* '\r' */
    out %al, %dx
    mov $0x0A, %al           /* '\n' */
    out %al, %dx

    /* ------ Load our GDT + far-jump to our Code64 selector (0x08)  */
    lgdt gdt64_ptr(%rip)

    /* 64-bit far jump: push CS, push target, lretq. The direct
     * ljmp $imm,$imm form isn't encodable in 64-bit mode. */
    pushq $0x08
    leaq  .Lbzimage_on_our_gdt(%rip), %rax
    pushq %rax
    lretq

.Lbzimage_on_our_gdt:
    /* Clear multiboot_ptr — no Multiboot info in kexec handoff. */
    movl $0, multiboot_ptr(%rip)

    /* Jump to shared 64-bit entry. It sets DS/ES/FS/GS/SS to 0x10,
     * establishes the stack, clears BSS, and calls kernel_main_x86. */
    jmp entry64

.section .note.GNU-stack, "", @progbits
