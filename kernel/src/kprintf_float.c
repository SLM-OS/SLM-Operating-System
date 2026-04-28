/*
 * kprintf_float.c - Floating-point format helpers for kprintf.
 *
 * Compiled WITHOUT -mgeneral-regs-only so it can do real `double`
 * arithmetic. The rest of the kernel still bans FP register usage
 * (no FP context save/restore on task switch / IRQ entry by default);
 * this TU is reachable only from kprintf's %f / %F / %e / %E / %g /
 * %G specifiers, which are themselves only fired during diagnostic
 * output (shell commands, tests, log lines). FP register state at
 * the call boundary is not preserved beyond what the C ABI promises.
 *
 * Done from scratch instead of pulling in libc / libm so the
 * freestanding build stays self-contained. Trade-offs:
 *   - Decimal point is always '.'. No locale plumbing — same
 *     rationale as luaconf's lua_getlocaledecpoint override.
 *   - Rounding uses round-half-away-from-zero (scale, +0.5,
 *     truncate), not the IEEE-754 round-half-to-even default.
 *     Acceptable for shell diagnostics and the Lua workloads
 *     that pushed for this support.
 *   - The %f path uses uint64_t for the integer side, so
 *     anything past ~1.8e19 hands off to scientific. %e covers
 *     the rest of the double range up to about ±1e308.
 *   - Subnormals print correctly as their nominal decimal value.
 *
 * Out of scope (rare in our usage; add if you hit them):
 *   - The '+' / ' ' sign flags.
 *   - The '#' alternate-form flag (preserve trailing zeros /
 *     decimal in %g, etc.).
 *   - Hexadecimal float (%a / %A).
 */

#include "kprintf_float.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Mirror of the fmt_output struct in kprintf.c. We need the layout
 * to call out->putc; the structure is otherwise opaque to us. */
struct fmt_output {
    void (*putc)(struct fmt_output *out, char c);
    char *buf;
    size_t pos;
    size_t size;
    size_t count;
    int crlf;
};

/* IEEE-754 bit-pattern classification. Returns 0 for finite values
 * (including ±0 and subnormals), 1 for ±Inf, 2 for NaN. *is_neg is
 * filled from the sign bit regardless of the return value. */
static int classify_double(double v, int *is_neg)
{
    union { double d; uint64_t u; } cv;
    cv.d = v;
    *is_neg = (int)((cv.u >> 63) & 1u);
    uint64_t exp_bits  = (cv.u >> 52) & 0x7FFULL;
    uint64_t mant_bits = cv.u & 0x000FFFFFFFFFFFFFULL;
    if (exp_bits == 0x7FFULL) {
        return mant_bits ? 2 : 1;
    }
    return 0;
}

/* Powers of 10 up to MAX_FLOAT_PREC, indexed by precision. */
#define MAX_FLOAT_PREC 17
static const double k_pow10[MAX_FLOAT_PREC + 1] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,
    1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17,
};

/* Local copy of the unsigned-int-to-decimal helper. We can't share
 * with kprintf.c because that's a static function in a different TU. */
static int unsigned_to_decimal(char *buf, uint64_t value)
{
    char *p = buf + 23;
    int len = 0;
    *p = '\0';
    if (value == 0) {
        *--p = '0';
        len = 1;
    } else {
        while (value > 0) {
            *--p = (char)('0' + (value % 10u));
            value /= 10u;
            len++;
        }
    }
    for (int i = 0; i < len; i++) buf[i] = p[i];
    buf[len] = '\0';
    return len;
}

/* Forward declaration — fmt_fixed_into_buf falls back to scientific
 * when the integer part would overflow uint64_t. */
static int fmt_sci_into_buf(char *buf, size_t bufsize, double v,
                            int precision, char e_char);

/* Append a non-negative finite double in fixed-point ("%f") form to
 * `buf`. Returns characters written, not including any trailing NUL.
 * Caller has already emitted any sign character. */
static int fmt_fixed_into_buf(char *buf, size_t bufsize, double v,
                              int precision)
{
    if (precision < 0) precision = 6;
    if (precision > MAX_FLOAT_PREC) precision = MAX_FLOAT_PREC;
    if (bufsize == 0) return 0;

    /* Cap before the cast to uint64_t. UINT64_MAX ≈ 1.84e19; leave
     * headroom for the +0.5 rounding bump and the precision scale.
     * Anything past 1e18 hands off to scientific instead. */
    if (v >= 1e18) {
        return fmt_sci_into_buf(buf, bufsize, v, precision, 'e');
    }

    double scale_d  = k_pow10[precision];
    double scaled_d = v * scale_d + 0.5;
    if (scaled_d >= 1.8e19) {
        return fmt_sci_into_buf(buf, bufsize, v, precision, 'e');
    }
    uint64_t scaled    = (uint64_t)scaled_d;
    uint64_t scale_u   = (uint64_t)scale_d;
    uint64_t int_part  = scale_u ? (scaled / scale_u) : scaled;
    uint64_t frac_part = scale_u ? (scaled % scale_u) : 0u;

    char tmp[24];
    int int_len = unsigned_to_decimal(tmp, int_part);

    int written = 0;
    for (int i = 0; i < int_len && written < (int)bufsize - 1; i++) {
        buf[written++] = tmp[i];
    }
    if (precision > 0 && written < (int)bufsize - 1) {
        buf[written++] = '.';
        int frac_len = unsigned_to_decimal(tmp, frac_part);
        for (int i = frac_len; i < precision && written < (int)bufsize - 1; i++) {
            buf[written++] = '0';
        }
        for (int i = 0; i < frac_len && written < (int)bufsize - 1; i++) {
            buf[written++] = tmp[i];
        }
    }
    if (written < (int)bufsize) buf[written] = '\0';
    return written;
}

/* Append a non-negative finite double in scientific ("%e" / "%E")
 * form. `e_char` selects 'e' or 'E'. */
static int fmt_sci_into_buf(char *buf, size_t bufsize, double v,
                            int precision, char e_char)
{
    if (precision < 0) precision = 6;
    if (precision > MAX_FLOAT_PREC) precision = MAX_FLOAT_PREC;
    if (bufsize == 0) return 0;

    /* Normalize v to [1, 10). Iterative because we have no log10. */
    int exp = 0;
    if (v != 0.0) {
        while (v >= 10.0) { v /= 10.0; exp++; }
        while (v <  1.0)  { v *= 10.0; exp--; }
    }

    int written = fmt_fixed_into_buf(buf, bufsize, v, precision);

    if (written < (int)bufsize - 1) buf[written++] = e_char;
    if (exp < 0) {
        if (written < (int)bufsize - 1) buf[written++] = '-';
        exp = -exp;
    } else {
        if (written < (int)bufsize - 1) buf[written++] = '+';
    }
    /* Must be at least 24 bytes — unsigned_to_decimal writes the
     * trailing NUL at buf[23] and walks backward from there. A
     * smaller tmp would silently corrupt the caller's saved x30
     * on the stack, breaking the return path. */
    char tmp[24];
    int elen = unsigned_to_decimal(tmp, (uint64_t)exp);
    if (elen < 2 && written < (int)bufsize - 1) {
        buf[written++] = '0';
    }
    for (int i = 0; i < elen && written < (int)bufsize - 1; i++) {
        buf[written++] = tmp[i];
    }
    if (written < (int)bufsize) buf[written] = '\0';
    return written;
}

/* Format `v` per `conv` ('f', 'F', 'e', 'E', 'g', 'G') into `buf`.
 * Sign + special-value handling lives here; the f / e helpers take
 * a non-negative finite value. */
static int fmt_double_into_buf(char *buf, size_t bufsize, double v,
                               int precision, char conv)
{
    if (bufsize == 0) return 0;
    int written = 0;
    int is_neg;
    int sp = classify_double(v, &is_neg);

    if (is_neg) {
        if (written < (int)bufsize - 1) buf[written++] = '-';
        v = -v;
    }
    if (sp == 2) {
        const char *s = (conv >= 'A' && conv <= 'Z') ? "NAN" : "nan";
        for (; *s && written < (int)bufsize - 1; s++) buf[written++] = *s;
        if (written < (int)bufsize) buf[written] = '\0';
        return written;
    }
    if (sp == 1) {
        const char *s = (conv >= 'A' && conv <= 'Z') ? "INF" : "inf";
        for (; *s && written < (int)bufsize - 1; s++) buf[written++] = *s;
        if (written < (int)bufsize) buf[written] = '\0';
        return written;
    }

    char e_char = (conv == 'E' || conv == 'G') ? 'E' : 'e';

    if (conv == 'f' || conv == 'F') {
        return written + fmt_fixed_into_buf(buf + written, bufsize - written,
                                            v, precision);
    }
    if (conv == 'e' || conv == 'E') {
        return written + fmt_sci_into_buf(buf + written, bufsize - written,
                                          v, precision, e_char);
    }

    /* %g / %G: pick the form per C99 6.3.1.8.
     *   if X < -4 || X >= P  → use %e with precision P-1
     *   else                 → use %f with precision P-1-X
     * where X is the decimal exponent and P is the (>=1) precision. */
    int p = precision < 0 ? 6 : precision;
    if (p == 0) p = 1;
    int exp = 0;
    if (v != 0.0) {
        double tmp = v;
        while (tmp >= 10.0) { tmp /= 10.0; exp++; }
        while (tmp <  1.0)  { tmp *= 10.0; exp--; }
    }
    int gw;
    if (exp < -4 || exp >= p) {
        gw = fmt_sci_into_buf(buf + written, bufsize - written, v,
                              p - 1, e_char);
    } else {
        int eff_prec = p - 1 - exp;
        if (eff_prec < 0) eff_prec = 0;
        gw = fmt_fixed_into_buf(buf + written, bufsize - written, v,
                                eff_prec);
    }
    int total = written + gw;

    /* C99 %g (no '#' flag) trims trailing zeros after the decimal,
     * and the decimal itself if it ends up bare. The exponent suffix
     * (when present) shifts down to close the gap. */
    int e_pos = -1;
    int frac_end = total;
    for (int i = written; i < total; i++) {
        if (buf[i] == 'e' || buf[i] == 'E') {
            e_pos = i;
            frac_end = i;
            break;
        }
    }
    int dot_pos = -1;
    for (int i = written; i < frac_end; i++) {
        if (buf[i] == '.') { dot_pos = i; break; }
    }
    if (dot_pos >= 0) {
        int trim_end = frac_end - 1;
        while (trim_end > dot_pos && buf[trim_end] == '0') trim_end--;
        if (trim_end == dot_pos) trim_end--;
        if (e_pos >= 0) {
            int shift = (frac_end - 1) - trim_end;
            for (int i = e_pos; i < total; i++) {
                buf[i - shift] = buf[i];
            }
            total -= shift;
        } else {
            total = trim_end + 1;
        }
    }
    if (total < (int)bufsize) buf[total] = '\0';
    return total;
}

static void emit_padding(struct fmt_output *out, char pad_char, int count)
{
    while (count-- > 0) {
        out->putc(out, pad_char);
    }
}

void kprintf_float_emit(struct fmt_output *out,
                        int precision, char conv,
                        int width, int left_justify, int zero_pad,
                        va_list *ap)
{
    double v = va_arg(*ap, double);
    char buf[48];
    int len = fmt_double_into_buf(buf, sizeof(buf), v, precision, conv);
    int pad = width > len ? width - len : 0;

    if (!left_justify && pad > 0) {
        if (zero_pad && len > 0 && buf[0] == '-') {
            out->putc(out, '-');
            emit_padding(out, '0', pad);
            for (int i = 1; i < len; i++) out->putc(out, buf[i]);
        } else {
            emit_padding(out, zero_pad ? '0' : ' ', pad);
            for (int i = 0; i < len; i++) out->putc(out, buf[i]);
        }
    } else {
        for (int i = 0; i < len; i++) out->putc(out, buf[i]);
        if (left_justify && pad > 0) {
            emit_padding(out, ' ', pad);
        }
    }
}

size_t kprintf_float_test_format_bits(char *buf, size_t bufsize,
                                      uint64_t bits, int precision,
                                      char conv)
{
    if (!buf || bufsize == 0) return 0;
    union { double d; uint64_t u; } cv;
    cv.u = bits;
    return (size_t)fmt_double_into_buf(buf, bufsize, cv.d, precision, conv);
}
