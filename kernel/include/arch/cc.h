/**
 * lwIP Architecture Configuration for SLM-OS
 *
 * Compiler and platform-specific definitions for the lwIP TCP/IP stack.
 * This file is included by lwip/arch.h before any lwIP headers.
 */

#ifndef ARCH_CC_H
#define ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include "debug.h"  /* For uart_printf */

/* -------------------------------------------------------------------------- */
/* Byte Order                                                                  */
/* -------------------------------------------------------------------------- */

/* Both AArch64 and x86-64 are little-endian */
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* -------------------------------------------------------------------------- */
/* Diagnostics                                                                 */
/* -------------------------------------------------------------------------- */

/* Platform-specific debug output using our UART printf */
#define LWIP_PLATFORM_DIAG(x) do { uart_printf x; } while(0)

/* Platform-specific assertion - use our kernel panic */
#if defined(PLATFORM_X86_64)
#define LWIP_PLATFORM_ASSERT(x) do { \
    uart_printf("lwIP ASSERT: %s at %s:%d\n", (x), __FILE__, __LINE__); \
    while(1) { __asm__ volatile("cli; hlt"); } \
} while(0)
#else
#define LWIP_PLATFORM_ASSERT(x) do { \
    uart_printf("lwIP ASSERT: %s at %s:%d\n", (x), __FILE__, __LINE__); \
    while(1) { __asm__ volatile("wfi"); } \
} while(0)
#endif

/* -------------------------------------------------------------------------- */
/* Standard Library Headers                                                    */
/* -------------------------------------------------------------------------- */

/* We have stddef.h and stdint.h (freestanding headers) */
#define LWIP_NO_STDDEF_H  0
#define LWIP_NO_STDINT_H  0

/* We don't have inttypes.h with PRId32 etc - define format strings manually */
#define LWIP_NO_INTTYPES_H 1

/* Format strings for printf (our uart_printf supports these) */
#define X8_F   "02x"
#define U16_F  "u"
#define S16_F  "d"
#define X16_F  "x"
#define U32_F  "u"
#define S32_F  "d"
#define X32_F  "x"
#define SZT_F  "zu"

/* We have limits.h (freestanding) */
#define LWIP_NO_LIMITS_H  0

/* We don't have unistd.h - provide ssize_t ourselves. Use ptrdiff_t so the
 * type matches the toolchain's libc typedef on both AArch64 and x86-64. */
#define LWIP_NO_UNISTD_H  1
typedef ptrdiff_t ssize_t;

/* newlib stdio.h provides ssize_t but not SSIZE_MAX, so publish the
 * limit explicitly and keep lwIP's arch.h from typedef'ing ssize_t to int. */
#ifndef SSIZE_MAX
#define SSIZE_MAX LONG_MAX
#endif

/* We don't have ctype.h - use lwIP's built-in implementations */
#define LWIP_NO_CTYPE_H   1

/* Let lwIP provide errno and error codes */
#define LWIP_PROVIDE_ERRNO 1

/* -------------------------------------------------------------------------- */
/* Random Number Generator                                                     */
/* -------------------------------------------------------------------------- */

/* Use timer count as entropy source (good enough for non-crypto purposes) */
uint32_t lwip_rand_slm(void);
#define LWIP_RAND() lwip_rand_slm()

/* -------------------------------------------------------------------------- */
/* Memory Alignment                                                            */
/* -------------------------------------------------------------------------- */

/* Default alignment for memory pools (4 bytes is minimum, 8 for 64-bit) */
#ifndef MEM_ALIGNMENT
#define MEM_ALIGNMENT 8
#endif

/* -------------------------------------------------------------------------- */
/* Compiler Specifics                                                          */
/* -------------------------------------------------------------------------- */

/* GCC/Clang packed struct support is handled by arch.h defaults */

/* Suppress unused argument warnings */
#ifndef LWIP_UNUSED_ARG
#define LWIP_UNUSED_ARG(x) (void)(x)
#endif

/* -------------------------------------------------------------------------- */
/* System Protection (NO_SYS mode)                                             */
/* -------------------------------------------------------------------------- */

/* Protection type for critical sections - not used in NO_SYS=1 mode
 * but must be defined for lwIP sys.h */
typedef int sys_prot_t;

#endif /* ARCH_CC_H */
