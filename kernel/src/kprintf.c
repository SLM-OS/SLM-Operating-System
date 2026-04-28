/*
 * kprintf.c - Kernel printf implementation
 *
 * Centralized formatting functions for kernel output. Uses the platform-specific
 * UART driver (uart_putc) for actual character output.
 *
 * Architecture: A common format parser (fmt_vprintf) uses an output abstraction
 * (struct fmt_output) with a putc callback. Two backends exist:
 *   - UART backend: writes characters to the hardware UART
 *   - Buffer backend: writes characters to a memory buffer with bounds checking
 *
 * Thread Safety:
 *   uart_puts() and uart_printf() are synchronized with a spinlock to prevent
 *   interleaved output from multiple CPUs. Use the _unlocked variants for
 *   panic handlers or very early boot before the lock is safe.
 */

#include "kprintf.h"
#include "kprintf_float.h"
#include "uart.h"
#include "spinlock.h"
#include "ncmem.h"
#include "string.h"
#include <stdint.h>

/* UART lock for thread-safe printf.
 *
 * Two locking strategies, selected by PLATFORM_HAS_NC_MEMORY (Pi 5, Jetson):
 *
 *   PLATFORM_HAS_NC_MEMORY: IRQ-disable only, no cross-CPU lock.
 *     On these platforms per-core L2 caches are incoherent (no SMPEN), so
 *     standard ldaxr/stxr spinlocks on cacheable memory deadlock under
 *     cross-CPU contention — each CPU sees its own stale "unlocked" state.
 *     This is safe because secondary CPUs only print during early boot
 *     (before scheduler start). After scheduler start, all UART output
 *     comes from CPU 0 (shell, tests, INFO logs). See smp.c:secondary_init.
 *     If secondary CPU printing is needed later, an NC-memory-based lock
 *     must be added.
 *
 *   Default (QEMU, x86-64): standard spinlock with IRQ save/restore. */
#if defined(PLATFORM_HAS_NC_MEMORY)

#define UART_LOCK_IRQSAVE() \
    irq_flags_t _uart_flags; \
    do { \
        __asm__ volatile("mrs %0, daif" : "=r"(_uart_flags)); \
        __asm__ volatile("msr daifset, #2" ::: "memory"); \
    } while(0)

#define UART_UNLOCK_IRQRESTORE() \
    do { \
        __asm__ volatile("msr daif, %0" :: "r"(_uart_flags) : "memory"); \
    } while(0)
#else
static spinlock_t uart_lock = SPINLOCK_INIT;

#define UART_LOCK_IRQSAVE() \
    irq_flags_t _uart_flags = spin_lock_irqsave(&uart_lock)

#define UART_UNLOCK_IRQRESTORE() \
    spin_unlock_irqrestore(&uart_lock, _uart_flags)
#endif

void kprintf_init_nc_lock(void)
{
    /* On Pi 5/Jetson: UART lock is IRQ-disable only (no cross-CPU lock).
     * Nothing to initialize. See comment above UART_LOCK_IRQSAVE. */
}

/* ========================================================================
 * Output Abstraction
 * ======================================================================== */

/*
 * Output abstraction for format functions.
 *
 * The putc callback writes a single character to the output target.
 * For UART output, it calls uart_putc directly.
 * For buffer output, it writes to the buffer with bounds checking.
 *
 * The crlf flag controls \n -> \r\n conversion for bare text and %s output.
 * This is enabled for UART (terminal needs \r\n) but not for buffer output.
 */
struct fmt_output {
    void (*putc)(struct fmt_output *out, char c);
    char *buf;      /* For buffer output: current write position */
    size_t pos;     /* For buffer output: space remaining (including null) */
    size_t size;    /* For buffer output: original buffer size */
    int count;      /* Total characters written (or that would be written) */
    int crlf;       /* Convert \n to \r\n in bare text and %s output */
};

/* UART backend: write character to hardware UART */
static void uart_out_putc(struct fmt_output *out, char c)
{
    uart_putc(c);
    out->count++;
}

/* Buffer backend: write character to buffer with bounds checking */
static void buf_out_putc(struct fmt_output *out, char c)
{
    out->count++;
    if (out->pos > 1) {
        *out->buf++ = c;
        out->pos--;
    }
}

/* Initialize output for UART */
static struct fmt_output fmt_output_uart(void)
{
    struct fmt_output out;
    out.putc = uart_out_putc;
    out.buf = NULL;
    out.pos = 0;
    out.size = 0;
    out.count = 0;
    out.crlf = 1;
    return out;
}

/* Initialize output for buffer */
static struct fmt_output fmt_output_buf(char *buf, size_t size)
{
    struct fmt_output out;
    out.putc = buf_out_putc;
    out.buf = buf;
    out.pos = size;
    out.size = size;
    out.count = 0;
    out.crlf = 0;
    return out;
}

/* ========================================================================
 * String Output Functions
 * ======================================================================== */

/*
 * Send a null-terminated string (unlocked version).
 * Use for panic handlers or very early boot.
 * Converts \n to \r\n for proper terminal display.
 */
void uart_puts_unlocked(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

/*
 * Send a null-terminated string (synchronized).
 * Acquires lock to prevent interleaved output from multiple CPUs.
 * Converts \n to \r\n for proper terminal display.
 */
void uart_puts(const char *s)
{
    UART_LOCK_IRQSAVE();
    uart_puts_unlocked(s);
    UART_UNLOCK_IRQRESTORE();
}

/* ========================================================================
 * Common Format Helpers
 * ======================================================================== */

/* Helper: output padding characters */
static void fmt_padding(struct fmt_output *out, char pad_char, int count)
{
    while (count-- > 0) {
        out->putc(out, pad_char);
    }
}

/* Helper: output a single character with width (no \r\n conversion for %c) */
static void fmt_char_width(struct fmt_output *out, char c, int width,
                           int left_justify)
{
    int pad = width > 1 ? width - 1 : 0;

    if (!left_justify && pad > 0) {
        fmt_padding(out, ' ', pad);
    }
    out->putc(out, c);
    if (left_justify && pad > 0) {
        fmt_padding(out, ' ', pad);
    }
}

/* Helper: output a string with width (\r\n conversion if crlf flag is set) */
static void fmt_string_width(struct fmt_output *out, const char *s, int width,
                             int left_justify)
{
    int len = (int)strlen(s);
    int pad = width > len ? width - len : 0;

    if (!left_justify && pad > 0) {
        fmt_padding(out, ' ', pad);
    }
    while (*s) {
        if (out->crlf && *s == '\n') {
            out->putc(out, '\r');
        }
        out->putc(out, *s++);
    }
    if (left_justify && pad > 0) {
        fmt_padding(out, ' ', pad);
    }
}

/* Helper: format unsigned integer to temp buffer, return digit count */
static int fmt_format_unsigned(char *buf, uint64_t value, int base,
                               int uppercase)
{
    char *p = buf + 23;  /* Work backwards from end */
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    int len = 0;

    *p = '\0';

    if (value == 0) {
        *--p = '0';
        len = 1;
    } else {
        while (value > 0) {
            *--p = digits[value % base];
            value /= base;
            len++;
        }
    }

    /* Move to start of buffer */
    for (int i = 0; i < len; i++) {
        buf[i] = p[i];
    }
    buf[len] = '\0';
    return len;
}

/* Helper: output a string from a temp buffer (no \r\n conversion needed) */
static void fmt_puts_raw(struct fmt_output *out, const char *s)
{
    while (*s) {
        out->putc(out, *s++);
    }
}

/* Helper: output unsigned integer with width and flags */
static void fmt_unsigned_width(struct fmt_output *out, uint64_t value,
                               int base, int uppercase, int width,
                               int left_justify, int zero_pad)
{
    char buf[24];
    int len = fmt_format_unsigned(buf, value, base, uppercase);
    int pad = width > len ? width - len : 0;
    char pad_char = zero_pad ? '0' : ' ';

    if (!left_justify && pad > 0) {
        fmt_padding(out, pad_char, pad);
    }
    fmt_puts_raw(out, buf);
    if (left_justify && pad > 0) {
        fmt_padding(out, ' ', pad);  /* Always space-pad on right */
    }
}

/* Helper: output signed integer with width and flags */
static void fmt_signed_width(struct fmt_output *out, int64_t value,
                             int base, int width, int left_justify,
                             int zero_pad)
{
    char buf[24];
    int negative = value < 0;
    uint64_t abs_val = negative ? (uint64_t)(-value) : (uint64_t)value;
    int len = fmt_format_unsigned(buf, abs_val, base, 0);
    int total_len = len + (negative ? 1 : 0);
    int pad = width > total_len ? width - total_len : 0;

    if (!left_justify) {
        if (zero_pad) {
            /* Sign before zeros */
            if (negative) {
                out->putc(out, '-');
            }
            fmt_padding(out, '0', pad);
        } else {
            /* Spaces before sign */
            fmt_padding(out, ' ', pad);
            if (negative) {
                out->putc(out, '-');
            }
        }
    } else {
        if (negative) {
            out->putc(out, '-');
        }
    }

    fmt_puts_raw(out, buf);

    if (left_justify && pad > 0) {
        fmt_padding(out, ' ', pad);
    }
}

/* ========================================================================
 * Common Format Parser
 * ======================================================================== */

/*
 * Common formatted output with va_list and output abstraction.
 *
 * Supported format: %[flags][width][.precision][length]specifier
 *   Flags:      - (left-justify), 0 (zero-pad)
 *   Width:      minimum field width (decimal number)
 *   Precision:  `.N` decimal digits — affects %f / %F / %e / %E / %g / %G;
 *               ignored for the integer / string specifiers.
 *   Length:     l (long), ll (long long), z (size_t) — collapses to 64-bit
 *               on both kernel targets.
 *   Specifiers: c, s, d, i, u, x, X, p, %, f, F, e, E, g, G.
 *
 * Bare text characters (outside format specifiers) get \r\n conversion
 * when out->crlf is set (UART output). %s strings also get conversion.
 * %c does NOT get \r\n conversion (preserving existing behavior).
 *
 * Float formatting is freestanding (no libc / libm) and uses '.' as the
 * decimal — no locale plumbing. NaN renders as "nan"/"NAN", ±Inf as
 * "inf"/"INF" matching the case of the conversion letter.
 */
static void fmt_vprintf(struct fmt_output *out, const char *fmt, va_list args)
{
    while (*fmt) {
        if (*fmt != '%') {
            if (out->crlf && *fmt == '\n') {
                out->putc(out, '\r');
            }
            out->putc(out, *fmt++);
            continue;
        }

        fmt++;  /* Skip '%' */

        /* Parse flags */
        int left_justify = 0;
        int zero_pad = 0;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left_justify = 1;
            if (*fmt == '0') zero_pad = 1;
            fmt++;
        }
        /* Left-justify overrides zero-pad */
        if (left_justify) zero_pad = 0;

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Parse precision (`.N`). -1 means "default for the conversion"
         * (e.g., 6 decimals for %f). 0 is a real value (no fractional
         * part for %f). Only the float specifiers honor this — %d /
         * %s / %x ignore it for now. */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Parse length modifier. Accept one *or two* 'l's so that %llu /
         * %lld / %llx parse as 64-bit (and don't fall through to the
         * default-case "print literally" path, which previously made
         * `netstat` and other %llu callers print "%lu" + "lu" verbatim).
         * On both kernel targets (aarch64-elf, x86_64-elf) `long` and
         * `long long` are 64-bit, so collapsing them is correct.
         * `z` accepts size_t for the same reason.
         *
         * The flag is named for what it controls — extracting a 64-bit
         * va_arg — rather than the literal `l` modifier, because three
         * paths (%l, %ll, %z) feed into it. */
        int arg_is_64bit = 0;
        if (*fmt == 'l') {
            arg_is_64bit = 1;
            fmt++;
            if (*fmt == 'l') fmt++;  /* consume the second l of `%ll<spec>` */
        } else if (*fmt == 'z') {
            arg_is_64bit = (sizeof(size_t) == sizeof(uint64_t));
            fmt++;
        }

        /* Handle specifier */
        switch (*fmt) {
        case 'c':
            fmt_char_width(out, (char)va_arg(args, int), width, left_justify);
            break;

        case 's': {
            const char *s = va_arg(args, const char *);
            if (s == NULL) s = "(null)";
            fmt_string_width(out, s, width, left_justify);
            break;
        }

        case 'd':
        case 'i':
            if (arg_is_64bit) {
                fmt_signed_width(out, va_arg(args, int64_t), 10, width,
                                 left_justify, zero_pad);
            } else {
                fmt_signed_width(out, va_arg(args, int32_t), 10, width,
                                 left_justify, zero_pad);
            }
            break;

        case 'u':
            if (arg_is_64bit) {
                fmt_unsigned_width(out, va_arg(args, uint64_t), 10, 0,
                                   width, left_justify, zero_pad);
            } else {
                fmt_unsigned_width(out, va_arg(args, uint32_t), 10, 0,
                                   width, left_justify, zero_pad);
            }
            break;

        case 'x':
            if (arg_is_64bit) {
                fmt_unsigned_width(out, va_arg(args, uint64_t), 16, 0,
                                   width, left_justify, zero_pad);
            } else {
                fmt_unsigned_width(out, va_arg(args, uint32_t), 16, 0,
                                   width, left_justify, zero_pad);
            }
            break;

        case 'X':
            if (arg_is_64bit) {
                fmt_unsigned_width(out, va_arg(args, uint64_t), 16, 1,
                                   width, left_justify, zero_pad);
            } else {
                fmt_unsigned_width(out, va_arg(args, uint32_t), 16, 1,
                                   width, left_justify, zero_pad);
            }
            break;

        case 'p':
            out->putc(out, '0');
            out->putc(out, 'x');
            /* Pointers: use width-2 to account for "0x" prefix */
            fmt_unsigned_width(out, (uint64_t)(uintptr_t)va_arg(args, void *),
                               16, 0, width > 2 ? width - 2 : 0,
                               left_justify, zero_pad);
            break;

        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
            /* Float formatting lives in kprintf_float.c, compiled
             * without -mgeneral-regs-only so it can do real `double`
             * arithmetic. We pass our va_list by pointer and let it
             * pull the FP arg off the variadic ABI's FP slot —
             * kprintf.c itself never references `double`. */
            (void)arg_is_64bit;
            kprintf_float_emit(out, precision, *fmt,
                               width, left_justify, zero_pad, &args);
            break;

        case '%':
            out->putc(out, '%');
            break;

        case '\0':
            /* Trailing % at end of format string */
            return;

        default:
            /* Unknown format, print literally */
            out->putc(out, '%');
            out->putc(out, *fmt);
            break;
        }

        fmt++;
    }
}

/* ========================================================================
 * Public API - Printf Functions
 * ======================================================================== */

/*
 * Formatted output with va_list.
 *
 * Supported format: %[flags][width][length]specifier
 *   Flags:  - (left-justify), 0 (zero-pad)
 *   Width:  minimum field width (decimal number)
 *   Length: l (long)
 *   Specifiers: c, s, d, i, u, x, X, p, %
 */
int uart_vprintf(const char *fmt, va_list args)
{
    struct fmt_output out = fmt_output_uart();
    fmt_vprintf(&out, fmt, args);
    return out.count;
}

/*
 * Formatted output (unlocked version).
 * Use for panic handlers or very early boot.
 */
int uart_printf_unlocked(const char *fmt, ...)
{
    va_list args;
    int count;

    va_start(args, fmt);
    count = uart_vprintf(fmt, args);
    va_end(args);

    return count;
}

/*
 * Formatted output (synchronized).
 * Acquires lock to prevent interleaved output from multiple CPUs.
 */
int uart_printf(const char *fmt, ...)
{
    va_list args;
    int count;

    UART_LOCK_IRQSAVE();
    va_start(args, fmt);
    count = uart_vprintf(fmt, args);
    va_end(args);
    UART_UNLOCK_IRQRESTORE();

    return count;
}

/* ========================================================================
 * Public API - snprintf Functions
 * ======================================================================== */

/*
 * Formatted output to buffer with va_list.
 *
 * Supported format: %[flags][width][length]specifier
 *   Flags:  - (left-justify), 0 (zero-pad)
 *   Width:  minimum field width (decimal number)
 *   Length: l (long)
 *   Specifiers: c, s, d, i, u, x, X, p, %
 */
int uart_vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    if (!buf || size == 0) {
        return -1;
    }

    struct fmt_output out = fmt_output_buf(buf, size);
    fmt_vprintf(&out, fmt, args);

    /* Always null-terminate */
    if (out.pos > 0) {
        *out.buf = '\0';
    } else if (size > 0) {
        buf[size - 1] = '\0';
    }

    /* out.count is an int; if a pathological format drove it past INT_MAX it
     * would have wrapped to negative. Clamp so callers never see a negative
     * length. __INT_MAX__ is a GCC built-in so this works in the -nostdinc
     * integrated x86 build without pulling in <limits.h>. */
    if (out.count < 0) {
        return __INT_MAX__;
    }
    return out.count;
}

/*
 * Formatted output to buffer.
 */
int uart_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    int count;

    va_start(args, fmt);
    count = uart_vsnprintf(buf, size, fmt, args);
    va_end(args);

    return count;
}
