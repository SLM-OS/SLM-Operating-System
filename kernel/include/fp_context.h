/*
 * fp_context.h - FP/SIMD state save/restore for AI scheduler
 *
 * Provides macros to save and restore FP register state around
 * AI inference calls. This prevents corruption of FP registers
 * when inference is called from interrupt context (e.g., timer IRQ
 * triggering scheduler_add_task for a waking task).
 *
 * The save/restore functions are implemented in fp_context.S
 * (in the ai_sched library, compiled without -mgeneral-regs-only).
 *
 * Calling paths for ai_mlp_assign_cpu():
 *   1. scheduler_add_task() from task_create context (non-IRQ) — safe
 *   2. scheduler_add_task() from timer IRQ waking a sleeping task — needs FP save
 *   3. sched_set_task_affinity() migration path — non-IRQ, safe
 *
 * Since path 2 is possible, FP state must always be saved around inference.
 */

#ifndef FP_CONTEXT_H
#define FP_CONTEXT_H

#ifdef CONFIG_AI_SCHEDULER

#include <stdint.h>

/*
 * FP/SIMD state buffer.
 *
 * ARM64: 32 × 128-bit SIMD registers (V0-V31) + FPCR + FPSR = 520 bytes
 * x86-64: FXSAVE area = 512 bytes
 *
 * Must be 16-byte aligned for stp/ldp q-register pairs (ARM64)
 * and for FXSAVE/FXRSTOR (x86-64).
 */
struct fp_state {
    uint8_t regs[32 * 16];     /* V0-V31 (ARM64) or FXSAVE area (x86-64) */
    uint32_t fpcr;             /* Floating-point control register */
    uint32_t fpsr;             /* Floating-point status register */
} __attribute__((aligned(16)));

/*
 * Save FP/SIMD state to buffer.
 * Implemented in fp_context.S.
 */
void fp_save(struct fp_state *state);

/*
 * Restore FP/SIMD state from buffer.
 * Implemented in fp_context.S.
 */
void fp_restore(const struct fp_state *state);

/*
 * Stack-allocated FP context save/restore macros.
 *
 * Usage:
 *   FP_CONTEXT_SAVE();
 *   // ... FP-using code (inference) ...
 *   FP_CONTEXT_RESTORE();
 *
 * The struct is ~520 bytes on stack. This is acceptable for kernel
 * stack sizes (16 KB per task).
 */
#define FP_CONTEXT_SAVE() \
    struct fp_state __fp_ctx __attribute__((aligned(16))); \
    fp_save(&__fp_ctx)

#define FP_CONTEXT_RESTORE() \
    fp_restore(&__fp_ctx)

#endif /* CONFIG_AI_SCHEDULER */

#endif /* FP_CONTEXT_H */
