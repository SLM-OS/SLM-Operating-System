/*
 * kprintf.c - Kernel printf implementation
 *
 * Centralized formatting functions for kernel output. Uses the platform-specific
 * UART driver (uart_putc) for actual character output.
 *
 * Thread Safety:
 *   uart_puts() and uart_printf() are synchronized with a spinlock to prevent
 *   interleaved output from multiple CPUs. Use the _unlocked variants for
 *   panic handlers or very early boot before the lock is safe.
 */

#include "kprintf.h"
#include "uart.h"
#include "spinlock.h"
#include <stdint.h>

/* Global lock for synchronized UART output */
static spinlock_t uart_lock = SPINLOCK_INIT;

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
    irq_flags_t flags = spin_lock_irqsave(&uart_lock);
    uart_puts_unlocked(s);
    spin_unlock_irqrestore(&uart_lock, flags);
}

/* ========================================================================
 * Printf Implementation - Helpers
 * ======================================================================== */

/* Helper: print padding characters */
static int print_padding(char pad_char, int count)
{
    int printed = 0;
    while (count-- > 0) {
        uart_putc(pad_char);
        printed++;
    }
    return printed;
}

/* Helper: get string length */
static int str_len(const char *s)
{
    int len = 0;
    while (*s++) len++;
    return len;
}

/* Helper: print a single character with width */
static int print_char_width(char c, int width, int left_justify)
{
    int count = 0;
    int pad = width > 1 ? width - 1 : 0;

    if (!left_justify && pad > 0) {
        count += print_padding(' ', pad);
    }
    uart_putc(c);
    count++;
    if (left_justify && pad > 0) {
        count += print_padding(' ', pad);
    }
    return count;
}

/* Helper: print a string with width */
static int print_string_width(const char *s, int width, int left_justify)
{
    int count = 0;
    int len = str_len(s);
    int pad = width > len ? width - len : 0;

    if (!left_justify && pad > 0) {
        count += print_padding(' ', pad);
    }
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
        count++;
    }
    if (left_justify && pad > 0) {
        count += print_padding(' ', pad);
    }
    return count;
}

/* Helper: format unsigned integer to buffer, return length */
static int format_unsigned(char *buf, uint64_t value, int base, int uppercase)
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

/* Helper: print unsigned integer with width and flags */
static int print_unsigned_width(uint64_t value, int base, int uppercase,
                                 int width, int left_justify, int zero_pad)
{
    char buf[24];
    int len = format_unsigned(buf, value, base, uppercase);
    int count = 0;
    int pad = width > len ? width - len : 0;
    char pad_char = zero_pad ? '0' : ' ';

    if (!left_justify && pad > 0) {
        count += print_padding(pad_char, pad);
    }
    for (int i = 0; i < len; i++) {
        uart_putc(buf[i]);
        count++;
    }
    if (left_justify && pad > 0) {
        count += print_padding(' ', pad);  /* Always space-pad on right */
    }
    return count;
}

/* Helper: print signed integer with width and flags */
static int print_signed_width(int64_t value, int base, int width,
                               int left_justify, int zero_pad)
{
    char buf[24];
    int negative = value < 0;
    uint64_t abs_val = negative ? (uint64_t)(-value) : (uint64_t)value;
    int len = format_unsigned(buf, abs_val, base, 0);
    int total_len = len + (negative ? 1 : 0);
    int count = 0;
    int pad = width > total_len ? width - total_len : 0;

    if (!left_justify) {
        if (zero_pad) {
            /* Sign before zeros */
            if (negative) {
                uart_putc('-');
                count++;
            }
            count += print_padding('0', pad);
        } else {
            /* Spaces before sign */
            count += print_padding(' ', pad);
            if (negative) {
                uart_putc('-');
                count++;
            }
        }
    } else {
        if (negative) {
            uart_putc('-');
            count++;
        }
    }

    for (int i = 0; i < len; i++) {
        uart_putc(buf[i]);
        count++;
    }

    if (left_justify && pad > 0) {
        count += print_padding(' ', pad);
    }
    return count;
}

/* ========================================================================
 * Printf Implementation - Main Functions
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
    int count = 0;

    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n') {
                uart_putc('\r');
            }
            uart_putc(*fmt++);
            count++;
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

        /* Parse length modifier */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        /* Handle specifier */
        switch (*fmt) {
        case 'c':
            count += print_char_width((char)va_arg(args, int), width, left_justify);
            break;

        case 's': {
            const char *s = va_arg(args, const char *);
            if (s == NULL) s = "(null)";
            count += print_string_width(s, width, left_justify);
            break;
        }

        case 'd':
        case 'i':
            if (is_long) {
                count += print_signed_width(va_arg(args, int64_t), 10, width,
                                            left_justify, zero_pad);
            } else {
                count += print_signed_width(va_arg(args, int32_t), 10, width,
                                            left_justify, zero_pad);
            }
            break;

        case 'u':
            if (is_long) {
                count += print_unsigned_width(va_arg(args, uint64_t), 10, 0,
                                              width, left_justify, zero_pad);
            } else {
                count += print_unsigned_width(va_arg(args, uint32_t), 10, 0,
                                              width, left_justify, zero_pad);
            }
            break;

        case 'x':
            if (is_long) {
                count += print_unsigned_width(va_arg(args, uint64_t), 16, 0,
                                              width, left_justify, zero_pad);
            } else {
                count += print_unsigned_width(va_arg(args, uint32_t), 16, 0,
                                              width, left_justify, zero_pad);
            }
            break;

        case 'X':
            if (is_long) {
                count += print_unsigned_width(va_arg(args, uint64_t), 16, 1,
                                              width, left_justify, zero_pad);
            } else {
                count += print_unsigned_width(va_arg(args, uint32_t), 16, 1,
                                              width, left_justify, zero_pad);
            }
            break;

        case 'p':
            uart_putc('0');
            uart_putc('x');
            count += 2;
            /* Pointers: use width-2 to account for "0x" prefix */
            count += print_unsigned_width((uint64_t)(uintptr_t)va_arg(args, void *),
                                          16, 0, width > 2 ? width - 2 : 0,
                                          left_justify, zero_pad);
            break;

        case '%':
            uart_putc('%');
            count++;
            break;

        case '\0':
            /* Trailing % at end of format string */
            return count;

        default:
            /* Unknown format, print literally */
            uart_putc('%');
            uart_putc(*fmt);
            count += 2;
            break;
        }

        fmt++;
    }

    return count;
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

    irq_flags_t flags = spin_lock_irqsave(&uart_lock);
    va_start(args, fmt);
    count = uart_vprintf(fmt, args);
    va_end(args);
    spin_unlock_irqrestore(&uart_lock, flags);

    return count;
}

/* ========================================================================
 * snprintf Implementation - Helpers
 * ======================================================================== */

/*
 * Context for buffer-based output.
 */
struct snprintf_ctx {
    char *buf;          /* Current write position */
    size_t remaining;   /* Space remaining (including null) */
    int total;          /* Total chars that would be written */
};

/* Helper: write char to buffer context */
static void buf_putc(struct snprintf_ctx *ctx, char c)
{
    ctx->total++;
    if (ctx->remaining > 1) {
        *ctx->buf++ = c;
        ctx->remaining--;
    }
}

/* Helper: write padding to buffer context */
static void buf_padding(struct snprintf_ctx *ctx, char pad_char, int count)
{
    while (count-- > 0) {
        buf_putc(ctx, pad_char);
    }
}

/* Helper: write string to buffer context */
static void buf_puts(struct snprintf_ctx *ctx, const char *s)
{
    while (*s) {
        buf_putc(ctx, *s++);
    }
}

/* Helper: write string with width to buffer context */
static void buf_puts_width(struct snprintf_ctx *ctx, const char *s,
                           int width, int left_justify)
{
    int len = str_len(s);
    int pad = width > len ? width - len : 0;

    if (!left_justify && pad > 0) {
        buf_padding(ctx, ' ', pad);
    }
    buf_puts(ctx, s);
    if (left_justify && pad > 0) {
        buf_padding(ctx, ' ', pad);
    }
}

/* Helper: format unsigned integer to temp buffer, return length */
static int buf_format_unsigned(char *tmp, uint64_t value, int base, int uppercase)
{
    char *p = tmp + 23;
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
        tmp[i] = p[i];
    }
    tmp[len] = '\0';
    return len;
}

/* Helper: write unsigned integer with width to buffer context */
static void buf_unsigned_width(struct snprintf_ctx *ctx, uint64_t value,
                               int base, int uppercase, int width,
                               int left_justify, int zero_pad)
{
    char tmp[24];
    int len = buf_format_unsigned(tmp, value, base, uppercase);
    int pad = width > len ? width - len : 0;
    char pad_char = zero_pad ? '0' : ' ';

    if (!left_justify && pad > 0) {
        buf_padding(ctx, pad_char, pad);
    }
    buf_puts(ctx, tmp);
    if (left_justify && pad > 0) {
        buf_padding(ctx, ' ', pad);
    }
}

/* Helper: write signed integer with width to buffer context */
static void buf_signed_width(struct snprintf_ctx *ctx, int64_t value,
                             int base, int width, int left_justify, int zero_pad)
{
    char tmp[24];
    int negative = value < 0;
    uint64_t abs_val = negative ? (uint64_t)(-value) : (uint64_t)value;
    int len = buf_format_unsigned(tmp, abs_val, base, 0);
    int total_len = len + (negative ? 1 : 0);
    int pad = width > total_len ? width - total_len : 0;

    if (!left_justify) {
        if (zero_pad) {
            if (negative) buf_putc(ctx, '-');
            buf_padding(ctx, '0', pad);
        } else {
            buf_padding(ctx, ' ', pad);
            if (negative) buf_putc(ctx, '-');
        }
    } else {
        if (negative) buf_putc(ctx, '-');
    }

    buf_puts(ctx, tmp);

    if (left_justify && pad > 0) {
        buf_padding(ctx, ' ', pad);
    }
}

/* ========================================================================
 * snprintf Implementation - Main Functions
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
    struct snprintf_ctx ctx;

    if (!buf || size == 0) {
        return -1;
    }

    ctx.buf = buf;
    ctx.remaining = size;
    ctx.total = 0;

    while (*fmt) {
        if (*fmt != '%') {
            buf_putc(&ctx, *fmt++);
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
        if (left_justify) zero_pad = 0;

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Parse length modifier */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        /* Handle specifier */
        switch (*fmt) {
        case 'c': {
            char c = (char)va_arg(args, int);
            int pad = width > 1 ? width - 1 : 0;
            if (!left_justify && pad > 0) buf_padding(&ctx, ' ', pad);
            buf_putc(&ctx, c);
            if (left_justify && pad > 0) buf_padding(&ctx, ' ', pad);
            break;
        }

        case 's': {
            const char *s = va_arg(args, const char *);
            if (s == NULL) s = "(null)";
            buf_puts_width(&ctx, s, width, left_justify);
            break;
        }

        case 'd':
        case 'i':
            if (is_long) {
                buf_signed_width(&ctx, va_arg(args, int64_t), 10, width,
                                 left_justify, zero_pad);
            } else {
                buf_signed_width(&ctx, va_arg(args, int32_t), 10, width,
                                 left_justify, zero_pad);
            }
            break;

        case 'u':
            if (is_long) {
                buf_unsigned_width(&ctx, va_arg(args, uint64_t), 10, 0,
                                   width, left_justify, zero_pad);
            } else {
                buf_unsigned_width(&ctx, va_arg(args, uint32_t), 10, 0,
                                   width, left_justify, zero_pad);
            }
            break;

        case 'x':
            if (is_long) {
                buf_unsigned_width(&ctx, va_arg(args, uint64_t), 16, 0,
                                   width, left_justify, zero_pad);
            } else {
                buf_unsigned_width(&ctx, va_arg(args, uint32_t), 16, 0,
                                   width, left_justify, zero_pad);
            }
            break;

        case 'X':
            if (is_long) {
                buf_unsigned_width(&ctx, va_arg(args, uint64_t), 16, 1,
                                   width, left_justify, zero_pad);
            } else {
                buf_unsigned_width(&ctx, va_arg(args, uint32_t), 16, 1,
                                   width, left_justify, zero_pad);
            }
            break;

        case 'p':
            buf_puts(&ctx, "0x");
            buf_unsigned_width(&ctx, (uint64_t)(uintptr_t)va_arg(args, void *),
                               16, 0, width > 2 ? width - 2 : 0,
                               left_justify, zero_pad);
            break;

        case '%':
            buf_putc(&ctx, '%');
            break;

        case '\0':
            /* Trailing % at end of format string */
            goto done;

        default:
            /* Unknown format, print literally */
            buf_putc(&ctx, '%');
            buf_putc(&ctx, *fmt);
            break;
        }

        fmt++;
    }

done:
    /* Always null-terminate */
    if (ctx.remaining > 0) {
        *ctx.buf = '\0';
    } else if (size > 0) {
        buf[size - 1] = '\0';
    }

    return ctx.total;
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
