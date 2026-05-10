/*
 * trap.h - Trap Frame Definition for SLM-OS
 *
 * Shared between exception handlers and syscall dispatch. Also #included
 * by kernel/arch/arm64/vectors.S so the asm save_regs/restore_regs
 * macros can reference the same TRAP_FRAME_OFF_* / TRAP_FRAME_ALLOC
 * literals as the C side — drift between the two is a compile-time
 * error rather than a silent ABI mismatch.
 */

#ifndef TRAP_H
#define TRAP_H

/*
 * Frame layout — shared with vectors.S
 *
 *   0..263   GPRs (x0..x30) + ELR + SPSR (this is the C view via struct trap_frame)
 *   264..271 padding (keeps q-region 16-byte aligned for stp q,q)
 *   272..783 q0-q31 (asm-only — saved by save_regs, restored by restore_regs)
 *   784..791 FPCR (asm-only)
 *   792..799 FPSR (asm-only)
 *
 * Total allocation: 800 bytes. Must be a multiple of 16 for SP alignment.
 *
 * The FP/SIMD region exists because every IRQ entry must preserve the
 * interrupted task's q-regs and FP control regs — see the rationale
 * block above save_regs in vectors.S.
 */
#define TRAP_FRAME_OFF_QREGS    272
#define TRAP_FRAME_OFF_FPCR     784
#define TRAP_FRAME_OFF_FPSR     792
#define TRAP_FRAME_SIZE         264   /* C-visible portion (GPR + ELR/SPSR) */
#define TRAP_FRAME_ALLOC        800   /* full SP allocation (incl. FP/SIMD) */

#ifndef __ASSEMBLER__
#include <stdint.h>

/*
 * Trap frame saved on the kernel stack during exceptions.
 *
 * The C view exposes the GPR + ELR + SPSR portion (264 bytes) used by
 * exception handlers. The assembly save_regs/restore_regs macros in
 * vectors.S allocate TRAP_FRAME_ALLOC bytes total — the additional
 * 528 bytes hold the full FP/SIMD register file (q0-q31 + FPCR +
 * FPSR), invisible to C and accessed only by the asm macros.
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

/* Pin the layout: a future addition to struct trap_frame that grows
 * past the GPR-view region would silently overlap the FP/SIMD slots.
 * Catch any drift at compile time. */
_Static_assert(sizeof(struct trap_frame) == TRAP_FRAME_SIZE,
               "struct trap_frame must be exactly TRAP_FRAME_SIZE bytes — "
               "vectors.S save_regs assumes the GPR-view ends at offset 264");
_Static_assert(TRAP_FRAME_OFF_QREGS >= TRAP_FRAME_SIZE,
               "FP/SIMD region must start past the C-visible struct view");
_Static_assert(TRAP_FRAME_OFF_QREGS % 16 == 0,
               "stp q,q requires 16-byte alignment of base offset");
_Static_assert(TRAP_FRAME_OFF_FPCR  == TRAP_FRAME_OFF_QREGS + 32 * 16,
               "FPCR must immediately follow the q0..q31 file (16 stp pairs)");
_Static_assert(TRAP_FRAME_OFF_FPSR  == TRAP_FRAME_OFF_FPCR  + 8,
               "FPSR must immediately follow FPCR");
_Static_assert(TRAP_FRAME_ALLOC     >= TRAP_FRAME_OFF_FPSR  + 8,
               "TRAP_FRAME_ALLOC must cover FPSR slot");
_Static_assert(TRAP_FRAME_ALLOC % 16 == 0,
               "SP allocation must preserve 16-byte stack alignment");

#endif /* !__ASSEMBLER__ */

#endif /* TRAP_H */
