/*
 * context.S - x86-64 context switch
 *
 * void switch_to(struct task *old, struct task *new)
 *
 * Saves callee-saved registers of the old task (if non-NULL) and
 * restores the new task's registers. For a new task, rip is set to
 * task_entry_wrapper (which reads entry from rbx, arg from r12).
 *
 * Register usage (System V AMD64 ABI):
 *   rdi = old task pointer (first argument)
 *   rsi = new task pointer (second argument)
 */

.section .text
.code64

/* struct task context offset (must match task.h) */
#define TASK_CONTEXT_OFFSET  0x20

/* struct cpu_context field offsets (must match task.h) */
#define CTX_RBX     0x00
#define CTX_RBP     0x08
#define CTX_R12     0x10
#define CTX_R13     0x18
#define CTX_R14     0x20
#define CTX_R15     0x28
#define CTX_RSP     0x30
#define CTX_RIP     0x38
#define CTX_RFLAGS  0x40

.global switch_to
.type switch_to, @function
switch_to:
    /* If old == NULL, skip save (first switch from idle) */
    test    %rdi, %rdi
    jz      .Lrestore

    /* Save old task's context */
    lea     TASK_CONTEXT_OFFSET(%rdi), %rdx

    mov     %rbx, CTX_RBX(%rdx)
    mov     %rbp, CTX_RBP(%rdx)
    mov     %r12, CTX_R12(%rdx)
    mov     %r13, CTX_R13(%rdx)
    mov     %r14, CTX_R14(%rdx)
    mov     %r15, CTX_R15(%rdx)
    mov     %rsp, CTX_RSP(%rdx)

    /* Save return address — when switched back, resume at .Lreturn */
    lea     .Lreturn(%rip), %rax
    mov     %rax, CTX_RIP(%rdx)

    /* Save rflags (preserves IF state) */
    pushfq
    pop     %rax
    mov     %rax, CTX_RFLAGS(%rdx)

.Lrestore:
    /* Restore new task's context */
    lea     TASK_CONTEXT_OFFSET(%rsi), %rdx

    mov     CTX_RBX(%rdx), %rbx
    mov     CTX_RBP(%rdx), %rbp
    mov     CTX_R12(%rdx), %r12
    mov     CTX_R13(%rdx), %r13
    mov     CTX_R14(%rdx), %r14
    mov     CTX_R15(%rdx), %r15
    mov     CTX_RSP(%rdx), %rsp

    /* Restore rflags */
    mov     CTX_RFLAGS(%rdx), %rax
    push    %rax
    popfq

    /* Jump to new task's saved RIP */
    jmp     *CTX_RIP(%rdx)

.Lreturn:
    /* Old task resumes here after being switched back */
    ret

.size switch_to, . - switch_to

/*
 * task_entry_wrapper - Entry point for new tasks
 *
 * When a new task is first scheduled, switch_to jumps here.
 * Callee-saved registers were set by task_create:
 *   rbx = task entry function pointer
 *   r12 = task argument
 *
 * Calls the entry function, then task_exit().
 */
.global task_entry_wrapper
.type task_entry_wrapper, @function
task_entry_wrapper:
    /* Enable interrupts (new tasks start with IF=0) */
    sti

    /* Call entry(arg) */
    mov     %r12, %rdi      /* arg → first parameter */
    call    *%rbx            /* call entry function */

    /* Entry function returned — clean up */
    call    task_exit
    /* Should not return */
    ud2

.size task_entry_wrapper, . - task_entry_wrapper

.section .note.GNU-stack, "", @progbits
