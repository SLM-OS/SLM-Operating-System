/**
 * lwIP System Architecture for SLM-OS
 *
 * This file defines the OS abstraction layer types for lwIP.
 * Since we use NO_SYS=1 (single-threaded mode), most of this is empty.
 * lwIP only needs us to provide a sys_now() function for timeouts.
 */

#ifndef ARCH_SYS_ARCH_H
#define ARCH_SYS_ARCH_H

#include <stdint.h>

/* In NO_SYS mode, these types are not used but must be defined */
typedef int sys_sem_t;
typedef int sys_mutex_t;
typedef int sys_mbox_t;
typedef int sys_thread_t;

/* Note: sys_prot_t is defined in cc.h (included before sys.h) */

/* Get current time in milliseconds - required for timeouts */
uint32_t sys_now(void);

#endif /* ARCH_SYS_ARCH_H */
