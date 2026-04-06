/*
 * ncmem.c - Non-cacheable shared memory bump allocator (Pi 5 only)
 *
 * Provides permanent, boot-time allocations from the 2MB NC region at the
 * top of physical RAM. Memory is never freed — these are kernel-lifetime
 * structures like scheduler run queues.
 */

#include "ncmem.h"
#include "uart.h"
#include "debug.h"
#include <stdint.h>

#if defined(PLATFORM_RASPI5)

static uintptr_t nc_next_free;
static size_t nc_total_allocated;

void ncmem_init(void)
{
    nc_next_free = NC_MEM_BASE;
    nc_total_allocated = 0;

    /* Don't zero entire 2MB region — callers zero their allocations.
     * NC memory writes bypass cache and are slow for large fills. */

    INFO("NC memory: 0x%lx - 0x%lx (%lu KB)",
         (unsigned long)NC_MEM_BASE,
         (unsigned long)(NC_MEM_BASE + NC_MEM_SIZE),
         (unsigned long)(NC_MEM_SIZE / 1024));
}

void *ncmem_alloc(size_t size, size_t align)
{
    if (size == 0 || align == 0)
        return NULL;

    /* Round up to alignment */
    uintptr_t aligned = (nc_next_free + align - 1) & ~(align - 1);

    if (aligned + size > NC_MEM_BASE + NC_MEM_SIZE) {
        WARN("ncmem_alloc: out of NC memory (requested %zu, used %zu/%lu)",
             size, nc_total_allocated, (unsigned long)NC_MEM_SIZE);
        return NULL;
    }

    nc_next_free = aligned + size;
    nc_total_allocated += size + (aligned - (nc_next_free - size - (aligned - nc_next_free)));
    nc_total_allocated = nc_next_free - NC_MEM_BASE;

    return (void *)aligned;
}

size_t ncmem_used(void)
{
    return nc_total_allocated;
}

#endif /* PLATFORM_RASPI5 */
