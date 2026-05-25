/*
 * wolf_heap.h - Public surface of the dedicated wolfssl heap.
 *
 * See `kernel/net/ssh/wolf_heap.c` for the rationale (scrypt at the
 * RFC 7914 floor needs a 32 MB working buffer that the shared
 * lua_stubs allocator can't satisfy).
 *
 * `wolf_os.c` routes XMALLOC / XFREE / XREALLOC through these.
 * Nothing else in the kernel should call them directly — the wolfssl
 * heap is intentionally isolated from the rest of the kernel's
 * byte-level allocations to keep fragmentation in one subsystem from
 * starving another.
 */

#ifndef SLMOS_WOLF_HEAP_H
#define SLMOS_WOLF_HEAP_H

#include <stddef.h>

void *wolf_heap_alloc(size_t size);
void  wolf_heap_free(void *p);
void *wolf_heap_realloc(void *p, size_t size);

#endif /* SLMOS_WOLF_HEAP_H */
