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
 * Size: 272 bytes (31 GPRs + ELR + SPSR = 33 * 8 = 264 + 8 padding)
 * Matches the save_regs macro in vectors.S exactly.
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

/* Struct size: 264 bytes. Assembly allocates 272 (16-byte aligned). */
#define TRAP_FRAME_SIZE     264
#define TRAP_FRAME_ALLOC    272  /* SP allocation in vectors.S */

#endif /* TRAP_H */
