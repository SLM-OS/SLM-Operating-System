/*
 * debug.h - Debug macros for SLM-OS
 *
 * Provides DEBUG_PRINT, ASSERT, and panic functionality.
 * Debug output can be disabled at compile time with -DNDEBUG.
 */

#ifndef DEBUG_H
#define DEBUG_H

#include "uart.h"

/*
 * Panic - halt the system with an error message.
 * This function does not return.
 */
void panic(const char *fmt, ...) __attribute__((noreturn));

/*
 * Debug print macro.
 * Only active when DEBUG is defined (not NDEBUG).
 * Automatically prefixes with [DEBUG] and file:line info.
 */
#if defined(DEBUG) && !defined(NDEBUG)
    #define DEBUG_PRINT(fmt, ...) \
        uart_printf("[DEBUG] %s:%d: " fmt "\n", __FILE__, __LINE__ __VA_OPT__(,) __VA_ARGS__)
#else
    #define DEBUG_PRINT(fmt, ...) ((void)0)
#endif

/*
 * Info print macro.
 * Always active, used for important status messages.
 */
#define INFO(fmt, ...) \
    uart_printf("[INFO] " fmt "\n" __VA_OPT__(,) __VA_ARGS__)

/*
 * Warning print macro.
 * Always active, used for non-fatal warnings.
 */
#define WARN(fmt, ...) \
    uart_printf("[WARN] " fmt "\n" __VA_OPT__(,) __VA_ARGS__)

/*
 * Error print macro.
 * Always active, used for error conditions.
 */
#define ERROR(fmt, ...) \
    uart_printf("[ERROR] %s:%d: " fmt "\n", __FILE__, __LINE__ __VA_OPT__(,) __VA_ARGS__)

/*
 * Assert macro.
 * Panics if condition is false.
 * Can be disabled with -DNDEBUG.
 */
#ifndef NDEBUG
    #define ASSERT(cond) \
        do { \
            if (!(cond)) { \
                panic("ASSERT failed: %s\n  at %s:%d", #cond, __FILE__, __LINE__); \
            } \
        } while (0)
#else
    #define ASSERT(cond) ((void)0)
#endif

/*
 * Static assert (compile-time).
 * Use for checking sizes, alignments, etc.
 */
#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

/*
 * Unreachable code marker.
 * Panics if reached; helps compiler optimization.
 */
#define UNREACHABLE() \
    panic("UNREACHABLE at %s:%d", __FILE__, __LINE__)

#endif /* DEBUG_H */
