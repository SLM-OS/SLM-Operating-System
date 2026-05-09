/*
 * trap.h - Trap Frame Definition for SLM-OS
 *
 * Shared between exception handlers and syscall dispatch.
 * Layout must match save_regs/restore_regs macros in vectors.S.
 */

#ifndef TRAP_H
#define TRAP_H

#include <stdint.h>

/*
 * Trap frame saved on the kernel stack during exceptions.
 *
 * The C view exposes the GPR + ELR + SPSR portion (264 bytes) used by
 * exception handlers. The assembly save_regs/restore_regs macros in
 * vectors.S allocate 800 bytes total — the additional 528 bytes hold
 * the full FP/SIMD register file (q0-q31 + FPCR + FPSR), invisible to
 * C and accessed only by the asm macros. The FP/SIMD save is required
 * to preserve interrupted-task state across el1_irq_handler /
 * resched_trampoline / schedule(): C callees may freely clobber the
 * caller-saved q-regs (q0-q7, q16-q31) per AAPCS64, so without saving
 * them on entry the IRQ-return path leaves the task with corrupted
 * FPU state.
 */
struct trap_frame {
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;
    uint64_t x16, x17, x18, x19, x20, x21, x22, x23;
    uint64_t x24, x25, x26, x27, x28, x29;
    uint64_t x30;       /* Link register */
    uint64_t elr;       /* Exception Link Register (return address) */
    uint64_t spsr;      /* Saved Program Status Register */
};

/* Struct size: 264 bytes (GPR + ELR/SPSR view).
 * Assembly allocates 800 bytes total — see save_regs in vectors.S:
 *   0..263   GPRs + ELR + SPSR (this struct's view)
 *   264..271 padding (keeps q-region 16-byte aligned)
 *   272..783 q0-q31 (asm-only)
 *   784..799 FPCR + FPSR (asm-only)
 */
#define TRAP_FRAME_SIZE     264
#define TRAP_FRAME_ALLOC    800  /* SP allocation in vectors.S */

#endif /* TRAP_H */
