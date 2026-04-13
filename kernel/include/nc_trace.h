/*
 * nc_trace.h - Non-cacheable diagnostic trace for scheduler bring-up.
 *
 * Writes a per-CPU tag to NC memory (instantly visible to CPU 0 without
 * cache maintenance). Enabled only when SCHED_DEBUG_NC_TRACE is defined —
 * otherwise the helper compiles to nothing.
 *
 * Layout: NC_MEM_BASE + NC_MEM_SIZE - 256 + cpu*4 (per-CPU tag slot).
 * Reserved range avoids collision with run queues and task table at the
 * front of NC memory. Panic-diagnostic writes (e.g., SCHED-C1 timeout's
 * 0xDEAD0001) bypass this helper and remain unconditional.
 */

#ifndef NC_TRACE_H
#define NC_TRACE_H

#include <stdint.h>
#include "ncmem.h"

#if defined(SCHED_DEBUG_NC_TRACE) && defined(PLATFORM_HAS_NC_MEMORY)

static inline void nc_trace(uint32_t cpu, uint32_t tag)
{
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + cpu * 4) = tag;
}

static inline void nc_trace_at(uintptr_t slot_off, uint32_t cpu, uint32_t tag)
{
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - slot_off + cpu * 4) = tag;
}

#else

static inline void nc_trace(uint32_t cpu, uint32_t tag)
{
    (void)cpu;
    (void)tag;
}

static inline void nc_trace_at(uintptr_t slot_off, uint32_t cpu, uint32_t tag)
{
    (void)slot_off;
    (void)cpu;
    (void)tag;
}

#endif

#endif /* NC_TRACE_H */
