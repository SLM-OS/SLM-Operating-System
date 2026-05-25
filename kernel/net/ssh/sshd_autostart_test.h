/*
 * sshd_autostart_test.h - Test-only hooks into sshd_autostart.c's
 *                        parser internals.
 *
 * Consumed only by kernel/tests/test_sshd_autostart.c. The parsers
 * are file-local statics in production; the test build exposes them
 * via this header by way of a small wrapper block at the bottom of
 * sshd_autostart.c (compiled whenever NET_SSHD is on, dead-stripped
 * from production kernels via --gc-sections).
 *
 * Mirrors the sshd_test.h pattern from #199a.
 */

#ifndef SSHD_AUTOSTART_TEST_H
#define SSHD_AUTOSTART_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sshd_autostart_cfg_view {
    bool     enabled;
    uint16_t port;
};

/* Parse a single decimal-u16 value-terminator field. Returns 0 on
 * success and writes the value to `*out`. */
int  sshd_autostart_test_parse_decimal_u16(const char *s, uint16_t *out);

/* True iff `s` exactly equals `want` followed by a terminator
 * (NUL / CR / LF / space / tab). Rejects prefix matches. */
bool sshd_autostart_test_token_equals(const char *s, const char *want);

/* Lenient bool parser: "1", "true", "yes", "on" (each with a
 * proper terminator) are true; anything else is false. */
bool sshd_autostart_test_parse_bool(const char *s);

/* Run the full line-oriented config parser over `buf` (which the
 * function mutates in place; pass a writable copy). `cfg` is
 * initialised by the caller to the default; the parser overlays
 * any `enabled=` or `port=` directives it finds. */
void sshd_autostart_test_parse_config(struct sshd_autostart_cfg_view *cfg,
                                      char *buf, size_t len);

#endif /* SSHD_AUTOSTART_TEST_H */
