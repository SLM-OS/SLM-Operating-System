/*
 * kprintf_float.h - Floating-point format helpers for kprintf.
 *
 * Lives outside kprintf.c so kprintf can stay under
 * `-mgeneral-regs-only` (no FP register usage in the kernel core)
 * while %f / %g / %e formatting can use real `double` arithmetic.
 *
 * The kernel main printf passes a va_list* into here when it sees
 * a float specifier; this TU pulls the double off the FP-arg slot
 * and emits the formatted value through the supplied fmt_output
 * via its putc callback. No FP types cross the kprintf.c boundary.
 */

#ifndef KPRINTF_FLOAT_H
#define KPRINTF_FLOAT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Mirrors `struct fmt_output` from kprintf.c. Kept here as an opaque
 * forward decl so this header doesn't pull kprintf.c internals in. */
struct fmt_output;

/*
 * Pull a `double` off `*ap`, format it per `conv`
 * ('f', 'F', 'e', 'E', 'g', 'G') with precision/width/flags, and
 * emit it through `out->putc`. Supports the same flag set as the
 * integer side: `width` is the minimum field width (space-padded,
 * or zero-padded with -mzero-pad — sign stays leading).
 *
 * `precision == -1` requests the conversion's default (6 for
 * %f / %e / %g).
 *
 * Safe to call with extreme values: NaN renders as "nan"/"NAN",
 * ±Inf as "inf"/"INF" matching the conversion's case. Magnitudes
 * past the %f path's uint64 range fall back to scientific.
 */
void kprintf_float_emit(struct fmt_output *out,
                        int precision, char conv,
                        int width, int left_justify, int zero_pad,
                        va_list *ap);

/*
 * Test-only: format a double given its IEEE-754 bit pattern (rather
 * than as a `double` directly) into `buf`. Returns characters
 * written, not including any trailing NUL.
 *
 * Takes the bit pattern as `uint64_t` so the test caller can stay
 * inside `-mgeneral-regs-only` — passing a `double` argument or
 * even storing one in a local would force FP register touches the
 * test build can't tolerate. The bit-cast happens inside this TU,
 * which is compiled with FP allowed.
 */
size_t kprintf_float_test_format_bits(char *buf, size_t bufsize,
                                      uint64_t bits, int precision,
                                      char conv);

#endif /* KPRINTF_FLOAT_H */
