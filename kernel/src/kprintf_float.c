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

/* Cap on `v` for the %f path. Above this we hand off to scientific
 * to avoid overflowing the uint64 cast: UINT64_MAX ≈ 1.844e19, and
 * `v * scale_d + 0.5` in fmt_fixed_into_buf needs headroom for the
 * rounding bump and the precision multiplier. 1e18 leaves ~18× of
 * margin, easily safe. */
#define FIXED_PATH_MAX_VALUE 1e18

/* Cap on `scaled_d = v * scale_d + 0.5` post-multiply, before the
 * uint64 cast. Same UINT64_MAX rationale as above; this catches the
 * narrow case where v itself is below FIXED_PATH_MAX_VALUE but the
 * precision multiplier pushes the product past UINT64_MAX. */
#define SCALED_CAST_LIMIT    1.8e19

/* Required size for the `tmp[]` buffer that unsigned_to_decimal
 * writes into. The helper writes a trailing NUL at offset 23 and
 * fills backward, so the buffer MUST be ≥ 24 bytes; a smaller
 * buffer would overflow the local and corrupt adjacent stack
 * state. (uint64 max is 20 decimal digits + NUL + a few bytes of
 * slack, so 24 is the minimum that fits all paths.) The same
 * rule applies to every tmp[] in this file — both fmt_fixed and
 * fmt_sci helpers reuse the same constant. */
#define UNSIGNED_TO_DECIMAL_BUF_SIZE 24

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
 * Caller has already emitted any sign character.
 *
 * Recursion bound: this function may tail-call fmt_sci_into_buf as
 * an overflow fallback, and fmt_sci_into_buf calls back into us on
 * a normalized value in [1, 10). Since the normalized value is far
 * below FIXED_PATH_MAX_VALUE, the second entry never re-triggers
 * the fallback — recursion depth is bounded at exactly 1. */
static int fmt_fixed_into_buf(char *buf, size_t bufsize, double v,
                              int precision)
{
    if (precision < 0) precision = 6;
    if (precision > MAX_FLOAT_PREC) precision = MAX_FLOAT_PREC;
    if (bufsize == 0) return 0;

    /* Cap before the cast to uint64_t. See FIXED_PATH_MAX_VALUE
     * comment near the top of the file for the UINT64_MAX
     * derivation. */
    if (v >= FIXED_PATH_MAX_VALUE) {
        return fmt_sci_into_buf(buf, bufsize, v, precision, 'e');
    }

    double scale_d  = k_pow10[precision];
    double scaled_d = v * scale_d + 0.5;
    if (scaled_d >= SCALED_CAST_LIMIT) {
        return fmt_sci_into_buf(buf, bufsize, v, precision, 'e');
    }
    uint64_t scaled    = (uint64_t)scaled_d;
    uint64_t scale_u   = (uint64_t)scale_d;
    uint64_t int_part  = scale_u ? (scaled / scale_u) : scaled;
    uint64_t frac_part = scale_u ? (scaled % scale_u) : 0u;

    char tmp[UNSIGNED_TO_DECIMAL_BUF_SIZE];
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
    /* See UNSIGNED_TO_DECIMAL_BUF_SIZE comment at the top of the
     * file for the size requirement. Same rule as the tmp[] in
     * fmt_fixed_into_buf above. */
    char tmp[UNSIGNED_TO_DECIMAL_BUF_SIZE];
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

/* Returns true if the formatted output is one of nan / inf
 * (possibly with a leading '-' sign). C99 6.3.1.7 leaves zero-pad
 * behaviour with these values implementation-defined; we follow
 * glibc / musl and force space-pad to avoid the "0000000inf" form
 * which is more confusing than informative. The detection scans
 * the post-sign character — buf[0] for unsigned, buf[1] when the
 * formatter has emitted a leading '-'. */
static bool fmt_double_is_non_finite(const char *buf, int len)
{
    int i = (len > 0 && buf[0] == '-') ? 1 : 0;
    if (i >= len) return false;
    char c = buf[i];
    return c == 'n' || c == 'N' || c == 'i' || c == 'I';
}

/* Format-and-emit a known `double` to `out`, with width / pad / left-
 * justify handling. Split out from kprintf_float_emit so the test
 * seam (kprintf_float_test_format_bits_padded) can drive the same
 * padding code without having to fake a va_list with a specific
 * double in it — which is awkward because va_list internal layout is
 * implementation-defined. */
static void kprintf_float_emit_value(struct fmt_output *out,
                                     double v,
                                     int precision, char conv,
                                     int width, int left_justify,
                                     int zero_pad)
{
    char buf[48];
    int len = fmt_double_into_buf(buf, sizeof(buf), v, precision, conv);
    int pad = width > len ? width - len : 0;

    /* Don't zero-pad nan / inf — the "0000000inf" form glibc /
     * musl avoid is more confusing than informative. Sign-then-
     * zeros only makes sense for numeric output. */
    int eff_zero_pad = zero_pad && !fmt_double_is_non_finite(buf, len);

    if (!left_justify && pad > 0) {
        if (eff_zero_pad && len > 0 && buf[0] == '-') {
            out->putc(out, '-');
            emit_padding(out, '0', pad);
            for (int i = 1; i < len; i++) out->putc(out, buf[i]);
        } else {
            emit_padding(out, eff_zero_pad ? '0' : ' ', pad);
            for (int i = 0; i < len; i++) out->putc(out, buf[i]);
        }
    } else {
        for (int i = 0; i < len; i++) out->putc(out, buf[i]);
        if (left_justify && pad > 0) {
            emit_padding(out, ' ', pad);
        }
    }
}

void kprintf_float_emit(struct fmt_output *out,
                        int precision, char conv,
                        int width, int left_justify, int zero_pad,
                        va_list *ap)
{
    double v = va_arg(*ap, double);
    kprintf_float_emit_value(out, v, precision, conv,
                             width, left_justify, zero_pad);
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

/* Tiny buffer-backed fmt_output for the padded-test seam. Mirrors
 * the buf_out_putc behaviour from kprintf.c — writes one char at a
 * time to a caller-supplied buffer, leaving room for a trailing NUL. */
static void test_buf_putc(struct fmt_output *out, char c)
{
    out->count++;
    if (out->pos > 1) {
        *out->buf++ = c;
        out->pos--;
    }
}

size_t kprintf_float_test_format_bits_padded(char *buf, size_t bufsize,
                                             uint64_t bits, int precision,
                                             char conv, int width,
                                             int left_justify, int zero_pad)
{
    if (!buf || bufsize == 0) return 0;
    union { double d; uint64_t u; } cv;
    cv.u = bits;
    char *cursor = buf;
    struct fmt_output out;
    out.putc = test_buf_putc;
    out.buf = cursor;
    out.pos = bufsize;
    out.size = bufsize;
    out.count = 0;
    out.crlf = 0;
    kprintf_float_emit_value(&out, cv.d, precision, conv,
                             width, left_justify, zero_pad);
    /* Null-terminate. test_buf_putc reserves the last byte for the
     * NUL by checking pos > 1 before writing. */
    if (bufsize > 0) {
        size_t written = (size_t)out.count;
        if (written >= bufsize) written = bufsize - 1;
        buf[written] = '\0';
    }
    return (size_t)out.count;
}

/* End-to-end test seam (#554 review warning): exercise the va_list
 * crossing from this FP-enabled TU into `kprintf.c`'s `fmt_vprintf`
 * (which is `-mgeneral-regs-only`) and back into `kprintf_float_emit`
 * here. The literal `double` pulled off the variadic ABI's FP slot
 * is what `kprintf_float_emit` reads via `va_arg(*ap, double)`.
 * Without this test, a future GCC change to FP arg spilling could
 * silently regress real Lua format calls while every other test
 * stays green.
 *
 * Routes through `snprintf` (the lua_stubs.c wrapper, FP-enabled)
 * rather than `uart_snprintf` (which lives in kprintf.c and is
 * compiled `-mgeneral-regs-only`). Reason: `va_start` in a
 * variadic function compiled with `-mgeneral-regs-only` doesn't
 * reliably save FP arg registers, so calling `uart_snprintf` with
 * an FP arg from any FP-enabled TU silently drops the value. The
 * production path (Lua `string.format("%.4f", x)`) goes through
 * `snprintf` — the wrapper does `va_start` in an FP-enabled TU and
 * forwards the populated `va_list` into `uart_vsnprintf`, which
 * passes it through to `fmt_vprintf` and finally to us. This test
 * mirrors exactly that chain.
 *
 * `snprintf` is declared inline so we don't drag the lua_stubs
 * header up; the prototype matches the C standard and lua_stubs.c
 * exports the symbol. */
extern int snprintf(char *str, size_t size, const char *fmt, ...);

size_t kprintf_float_test_e2e_uart_snprintf(char *buf, size_t bufsize,
                                            uint64_t bits, int precision,
                                            char conv)
{
    if (!buf || bufsize == 0) return 0;
    union { double d; uint64_t u; } cv;
    cv.u = bits;

    /* Build "%.Nf" / "%g" / "%e" etc. */
    char fmt[16];
    int n;
    if (precision < 0) {
        n = snprintf(fmt, sizeof(fmt), "%%%c", conv);
    } else {
        n = snprintf(fmt, sizeof(fmt), "%%.%d%c", precision, conv);
    }
    if (n <= 0 || n >= (int)sizeof(fmt)) return 0;

    int written = snprintf(buf, bufsize, fmt, cv.d);
    return written < 0 ? 0 : (size_t)written;
}
