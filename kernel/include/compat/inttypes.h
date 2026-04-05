/*
 * inttypes.h - Minimal freestanding replacement
 *
 * Provides format macros needed by LittleFS (PRIu32, PRIx32, etc.).
 */

#ifndef COMPAT_INTTYPES_H
#define COMPAT_INTTYPES_H

#include <stdint.h>

#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  "ld"

#define PRIu8   "u"
#define PRIu16  "u"
#define PRIu32  "u"
#define PRIu64  "lu"

#define PRIx8   "x"
#define PRIx16  "x"
#define PRIx32  "x"
#define PRIx64  "lx"

#define PRIX8   "X"
#define PRIX16  "X"
#define PRIX32  "X"
#define PRIX64  "lX"

#endif /* COMPAT_INTTYPES_H */
