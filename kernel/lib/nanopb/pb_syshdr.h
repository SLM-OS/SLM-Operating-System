/*
 * pb_syshdr.h — SLM-OS freestanding system header for nanopb.
 *
 * Set PB_SYSTEM_HEADER="pb_syshdr.h" (quoted, via -DPB_SYSTEM_HEADER)
 * at build time so nanopb uses this instead of <string.h>, <stdlib.h>,
 * and <limits.h>. Those aren't freestanding-safe in the SLM-OS kernel.
 *
 * Provides everything nanopb needs:
 *   - uint*_t / int*_t / size_t / bool from the C23 freestanding set.
 *   - memcpy / memset / strlen from kernel/src/string.c.
 *   - A bounded INT_MAX (nanopb uses it in a few buffer-length guards).
 *
 * PB_ENABLE_MALLOC is left undefined — SLM-OS routes allocations
 * through pb_callback_t backed by kernel/mm/pmm.c, not nanopb's
 * internal realloc helper. If a future consumer needs malloc-style
 * nanopb, add `-DPB_ENABLE_MALLOC=1` and wire `pb_realloc`/`pb_free`
 * hooks here; see pb.h §"Memory allocation".
 */

#ifndef PB_SYSHDR_H
#define PB_SYSHDR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* INT_MAX — nanopb's bounds checks on varint lengths use this and a
 * handful of other limits. The rest (INT_MIN, SIZE_MAX, ...) come
 * from <stdint.h> / the default promotion rules, which are fine in
 * C23 freestanding mode. */
#ifndef INT_MAX
#define INT_MAX  0x7FFFFFFF
#endif
#ifndef INT_MIN
#define INT_MIN  (-INT_MAX - 1)
#endif
#ifndef UINT_MAX
#define UINT_MAX 0xFFFFFFFFu
#endif
#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif

/* SLM-OS string functions — defined in kernel/src/string.c. */
size_t strlen(const char *s);
void  *memcpy(void *dest, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
int    memcmp(const void *s1, const void *s2, size_t n);

#endif /* PB_SYSHDR_H */
