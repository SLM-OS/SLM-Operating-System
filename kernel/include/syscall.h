/*
 * syscall.h - System Call Interface for SLM-OS
 *
 * Defines the syscall ABI for user-mode (EL0) components.
 *
 * Calling convention (AArch64):
 *   x8  = syscall number
 *   x0-x5 = arguments
 *   x0  = return value (negative = error)
 *   Invoked via SVC #0
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* ============================================================================
 * Syscall Numbers
 * ============================================================================ */

#define SYS_EXIT        0       /* Terminate component: sys_exit(code) */
#define SYS_YIELD       1       /* Yield CPU: sys_yield() */
#define SYS_SEND        2       /* Send message: sys_send(topic, data, len) */
#define SYS_RECV        3       /* Receive message: sys_recv(topic_out, buf, len, timeout) */
#define SYS_INFER       4       /* Run inference: sys_infer(model, in, in_len, out, out_len) */
#define SYS_SLEEP       5       /* Sleep: sys_sleep(ms) */
#define SYS_LOG         6       /* Log to UART: sys_log(str, len) */
#define SYS_TOUCH_BLOCK 7       /* Touch model block: sys_touch_block(handle) (#123) */
#define SYS_MAX         8       /* Sentinel (must be last + 1) */

/* SPSR value for EL0t (EL0, SP_EL0, all interrupts enabled) */
#define SPSR_EL0T       0x0

/* ============================================================================
 * Syscall Dispatch (called from exception handler)
 * ============================================================================ */

struct trap_frame;  /* Forward declaration */

/*
 * Dispatch a syscall from the trap frame.
 *
 * Called by el0_sync_handler when EC == 0x15 (SVC from AArch64).
 * Reads syscall number from frame->x8, dispatches to handler.
 * Return value is written to frame->x0.
 */
void syscall_dispatch(struct trap_frame *frame);

#endif /* SYSCALL_H */
