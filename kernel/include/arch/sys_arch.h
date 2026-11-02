/**
 * lwIP System Architecture for SLM-OS
 *
 * This file defines the OS abstraction layer types for lwIP.
 * Since we use NO_SYS=1 (single-threaded mode), most of this is empty.
 * lwIP only needs us to provide a sys_now() function for timeouts.
 */

#ifndef ARCH_SYS_ARCH_H
#define ARCH_SYS_ARCH_H

#include <stdbool.h>
#include <stdint.h>
#include "arch/cc.h"    /* sys_prot_t for lwip_core_lock() */

/* In NO_SYS mode, these types are not used but must be defined */
typedef int sys_sem_t;
typedef int sys_mutex_t;
typedef int sys_mbox_t;
typedef int sys_thread_t;

/* Note: sys_prot_t is defined in cc.h (included before sys.h) */

/* Get current time in milliseconds - required for timeouts */
uint32_t sys_now(void);

/* Coarse lwIP core lock. Every lwIP raw-API entry from task context
 * and net_poll's core-processing sections must run between these two
 * calls. irq_save-based: nestable (flags round-trip) and safe under
 * lwIP-callback recursion. See the locking-rule comment block in
 * kernel/net/sys_arch.c for the full model. */
sys_prot_t lwip_core_lock(void);
void lwip_core_unlock(sys_prot_t token);

/* Core-lock checking (#1065). lwIP calls LWIP_ASSERT_CORE_LOCKED()
 * at the top of every raw-API entry point; lwipopts.h routes it to
 * lwip_core_assert_locked(), which counts (and WARNs on the first
 * few) calls made with IRQs unmasked or off CPU 0. The check is off
 * until armed: net_pump arms it when it starts, because before then
 * nothing can preempt an lwIP caller (boot, and the test kernel,
 * which has no net_pump, call lwIP directly). */
void lwip_core_check_arm(bool armed);
uint32_t lwip_core_violation_count(void);

#endif /* ARCH_SYS_ARCH_H */
