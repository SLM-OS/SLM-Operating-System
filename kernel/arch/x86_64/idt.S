/*
 * idt.S - x86-64 interrupt/exception entry stubs
 *
 * Each CPU exception (0-31) gets a stub that pushes a dummy error code
 * (if the CPU doesn't push one), then the vector number, then jumps to
 * a common handler that saves all registers and calls the C handler.
 *
 * ISR stack frame (grows downward):
 *   [SS, RSP, RFLAGS, CS, RIP]   <- pushed by CPU on interrupt
 *   [error_code]                   <- pushed by CPU or dummy 0
 *   [vector]                       <- pushed by stub
 *   [all GPRs]                     <- pushed by common handler
 */

.section .text
.code64

.extern exception_handler

/*
 * Common entry point — saves all GPRs, calls C handler, restores, iretq.
 */
isr_common:
    /* Save all general-purpose registers */
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    /* Pass pointer to saved state as first argument */
    mov %rsp, %rdi
    call exception_handler

    /* Restore all general-purpose registers */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    /* Remove vector and error code from stack */
    add $16, %rsp

    iretq

/*
 * ISR stubs for exceptions 0-31.
 * Exceptions 8, 10-14, 17, 21, 29, 30 push an error code;
 * all others need a dummy 0 pushed to keep the stack layout uniform.
 */

/* Macro for exceptions WITHOUT error code */
.macro ISR_NOERR vector
.global isr_\vector
isr_\vector:
    push $0             /* dummy error code */
    push $\vector
    jmp isr_common
.endm

/* Macro for exceptions WITH error code (CPU pushes it) */
.macro ISR_ERR vector
.global isr_\vector
isr_\vector:
    /* error code already on stack */
    push $\vector
    jmp isr_common
.endm

/* Exceptions 0-31 */
ISR_NOERR 0    /* #DE Divide Error */
ISR_NOERR 1    /* #DB Debug */
ISR_NOERR 2    /* NMI */
ISR_NOERR 3    /* #BP Breakpoint */
ISR_NOERR 4    /* #OF Overflow */
ISR_NOERR 5    /* #BR Bound Range */
ISR_NOERR 6    /* #UD Invalid Opcode */
ISR_NOERR 7    /* #NM No Math Coprocessor */
ISR_ERR   8    /* #DF Double Fault */
ISR_NOERR 9    /* Coprocessor Segment Overrun */
ISR_ERR   10   /* #TS Invalid TSS */
ISR_ERR   11   /* #NP Segment Not Present */
ISR_ERR   12   /* #SS Stack Fault */
ISR_ERR   13   /* #GP General Protection */
ISR_ERR   14   /* #PF Page Fault */
ISR_NOERR 15   /* Reserved */
ISR_NOERR 16   /* #MF x87 FPU Error */
ISR_ERR   17   /* #AC Alignment Check */
ISR_NOERR 18   /* #MC Machine Check */
ISR_NOERR 19   /* #XM SIMD Exception */
ISR_NOERR 20   /* #VE Virtualization */
ISR_ERR   21   /* #CP Control Protection */
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29   /* #VC VMM Communication */
ISR_ERR   30   /* #SX Security Exception */
ISR_NOERR 31

/* IRQ stubs (vectors 32-47) for PIC interrupts */
.macro ISR_IRQ vector
.global isr_\vector
isr_\vector:
    push $0
    push $\vector
    jmp isr_common
.endm

ISR_IRQ 32     /* IRQ 0: PIT Timer */
ISR_IRQ 33     /* IRQ 1: Keyboard */
ISR_IRQ 34
ISR_IRQ 35
ISR_IRQ 36
ISR_IRQ 37
ISR_IRQ 38
ISR_IRQ 39
ISR_IRQ 40
ISR_IRQ 41
ISR_IRQ 42
ISR_IRQ 43
ISR_IRQ 44
ISR_IRQ 45
ISR_IRQ 46
ISR_IRQ 47

.section .note.GNU-stack, "", @progbits
