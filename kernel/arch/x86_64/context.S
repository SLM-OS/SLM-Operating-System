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
/* 0x48: 8-byte pad to align fxsave area (task.h's _pad0). */
#define CTX_FXSAVE  0x50    /* 512-byte FXSAVE area (D2 / P1-6) */

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

    /* Save x87 + MMX + XMM state (D2 / P1-6). fxsave requires a
     * 16-byte aligned destination — task.h forces the fxsave array
     * to 16-byte alignment and _pad0 ensures the offset 0x50 is
     * 16-aligned within cpu_context (which itself starts at
     * task offset 0x20, 16-aligned).
     * Note: CR4.OSFXSR is set in trampoline32.S / ap_trampoline.S
     * (C2) so fxsave executes at CPL=0 without #UD. */
    fxsave  CTX_FXSAVE(%rdx)

.Lrestore:
    /* Restore new task's context */
    lea     TASK_CONTEXT_OFFSET(%rsi), %rdx

    /* Restore x87 + MMX + XMM state BEFORE GPRs — fxrstor clobbers
     * no GPRs and the incoming xmm state must be live before the
     * task resumes executing its own code. */
    fxrstor CTX_FXSAVE(%rdx)

    mov     CTX_RBX(%rdx), %rbx
    mov     CTX_RBP(%rdx), %rbp
    mov     CTX_R12(%rdx), %r12
    mov     CTX_R13(%rdx), %r13
    mov     CTX_R14(%rdx), %r14
    mov     CTX_R15(%rdx), %r15
    mov     CTX_RSP(%rdx), %rsp

    /* Restore rflags (may re-enable IRQs via IF bit).
     * Reentrant schedule() is prevented by per-CPU guard flag. */
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
 * Tail-calls task_entry_trampoline(entry, arg) (kernel/sched/task.c),
 * which clears preempt_disabled[cpu] — set to 1 by the outgoing
 * schedule()/scheduler_start() before switch_to — then invokes the
 * task's entry function and task_exit() on return. Without this path
 * a new task's first timeslice would run with preemption disabled
 * until the outgoing task is resumed (fixes #91).
 */
.global task_entry_wrapper
.type task_entry_wrapper, @function
task_entry_wrapper:
    /* Enable interrupts (new tasks start with IF=0) */
    sti

    /* Tail-call task_entry_trampoline(entry=rbx, arg=r12) */
    mov     %rbx, %rdi      /* entry → first parameter */
    mov     %r12, %rsi      /* arg   → second parameter */
    jmp     task_entry_trampoline

.size task_entry_wrapper, . - task_entry_wrapper

.section .note.GNU-stack, "", @progbits
