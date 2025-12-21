/*
 * context.S - Context switch for SLM-OS (ARM64)
 *
 * Saves and restores CPU state for task switching.
 */

.section .text

/*
 * switch_to(old, new)
 *
 * Arguments:
 *   x0 = pointer to old task (struct task*), or NULL if first switch
 *   x1 = pointer to new task (struct task*)
 *
 * Saves callee-saved registers and FPU/SIMD state to old task's context,
 * then restores from new task's context.
 *
 * struct cpu_context layout (offsets in bytes):
 *   0x00: x19
 *   0x08: x20
 *   0x10: x21
 *   0x18: x22
 *   0x20: x23
 *   0x28: x24
 *   0x30: x25
 *   0x38: x26
 *   0x40: x27
 *   0x48: x28
 *   0x50: x29 (fp)
 *   0x58: x30 (lr)
 *   0x60: sp
 *   0x68: v0-v31 (32 * 16 = 512 bytes)
 *   0x268: fpcr
 *   0x270: fpsr
 *
 * struct task layout:
 *   0x00: id (4 bytes)
 *   0x04: name (16 bytes)
 *   0x14: state (4 bytes)
 *   0x18: next (8 bytes)
 *   0x20: context starts here
 */

#define TASK_CONTEXT_OFFSET     0x20

#define CTX_X19     0x00
#define CTX_X20     0x08
#define CTX_X21     0x10
#define CTX_X22     0x18
#define CTX_X23     0x20
#define CTX_X24     0x28
#define CTX_X25     0x30
#define CTX_X26     0x38
#define CTX_X27     0x40
#define CTX_X28     0x48
#define CTX_X29     0x50
#define CTX_X30     0x58
#define CTX_SP      0x60
/* 8 bytes padding here due to alignas(16) on v[] */
#define CTX_V0      0x70
#define CTX_FPCR    0x270
#define CTX_FPSR    0x278

.global switch_to
.type switch_to, @function
switch_to:
    /*
     * If old task is NULL, skip saving (first switch)
     */
    cbz     x0, .Lrestore

    /*
     * Save old task's context
     */
    add     x2, x0, #TASK_CONTEXT_OFFSET    /* x2 = &old->context */

    /* Save callee-saved general purpose registers */
    stp     x19, x20, [x2, #CTX_X19]
    stp     x21, x22, [x2, #CTX_X21]
    stp     x23, x24, [x2, #CTX_X23]
    stp     x25, x26, [x2, #CTX_X25]
    stp     x27, x28, [x2, #CTX_X27]
    stp     x29, x30, [x2, #CTX_X29]

    /* Save stack pointer */
    mov     x3, sp
    str     x3, [x2, #CTX_SP]

    /* Save FPU/SIMD registers (eager save for SLM workloads) */
    add     x3, x2, #CTX_V0
    stp     q0,  q1,  [x3, #0x00]
    stp     q2,  q3,  [x3, #0x20]
    stp     q4,  q5,  [x3, #0x40]
    stp     q6,  q7,  [x3, #0x60]
    stp     q8,  q9,  [x3, #0x80]
    stp     q10, q11, [x3, #0xA0]
    stp     q12, q13, [x3, #0xC0]
    stp     q14, q15, [x3, #0xE0]
    stp     q16, q17, [x3, #0x100]
    stp     q18, q19, [x3, #0x120]
    stp     q20, q21, [x3, #0x140]
    stp     q22, q23, [x3, #0x160]
    stp     q24, q25, [x3, #0x180]
    stp     q26, q27, [x3, #0x1A0]
    stp     q28, q29, [x3, #0x1C0]
    stp     q30, q31, [x3, #0x1E0]

    /* Save FPCR and FPSR */
    mrs     x3, fpcr
    str     x3, [x2, #CTX_FPCR]
    mrs     x3, fpsr
    str     x3, [x2, #CTX_FPSR]

.Lrestore:
    /*
     * Restore new task's context
     */
    add     x2, x1, #TASK_CONTEXT_OFFSET    /* x2 = &new->context */

    /* Restore FPCR and FPSR first */
    ldr     x3, [x2, #CTX_FPCR]
    msr     fpcr, x3
    ldr     x3, [x2, #CTX_FPSR]
    msr     fpsr, x3

    /* Restore FPU/SIMD registers */
    add     x3, x2, #CTX_V0
    ldp     q0,  q1,  [x3, #0x00]
    ldp     q2,  q3,  [x3, #0x20]
    ldp     q4,  q5,  [x3, #0x40]
    ldp     q6,  q7,  [x3, #0x60]
    ldp     q8,  q9,  [x3, #0x80]
    ldp     q10, q11, [x3, #0xA0]
    ldp     q12, q13, [x3, #0xC0]
    ldp     q14, q15, [x3, #0xE0]
    ldp     q16, q17, [x3, #0x100]
    ldp     q18, q19, [x3, #0x120]
    ldp     q20, q21, [x3, #0x140]
    ldp     q22, q23, [x3, #0x160]
    ldp     q24, q25, [x3, #0x180]
    ldp     q26, q27, [x3, #0x1A0]
    ldp     q28, q29, [x3, #0x1C0]
    ldp     q30, q31, [x3, #0x1E0]

    /* Restore stack pointer */
    ldr     x3, [x2, #CTX_SP]
    mov     sp, x3

    /* Restore callee-saved general purpose registers */
    ldp     x19, x20, [x2, #CTX_X19]
    ldp     x21, x22, [x2, #CTX_X21]
    ldp     x23, x24, [x2, #CTX_X23]
    ldp     x25, x26, [x2, #CTX_X25]
    ldp     x27, x28, [x2, #CTX_X27]
    ldp     x29, x30, [x2, #CTX_X29]

    /* Return to new task (x30 holds return address) */
    ret

.size switch_to, . - switch_to
