/*
 * telnetd_config.h - /etc/telnetd.conf parser
 *
 * Flat key=value parser used by the Phase 3 auto-start path to decide
 * whether and how to bring the telnetd listener up at boot.
 *
 * Grammar:
 *   line        := ( comment | pair | blank ) newline
 *   comment     := '#' any*
 *   pair        := key '=' value
 *   blank       := whitespace*
 *
 * Keys (see plan §3.5):
 *   enabled      = true | false | 1 | 0 | yes | no | on | off
 *   port         = decimal 0..65535
 *   bind         = IPv4 dotted-quad (currently advisory; listener
 *                  always binds 0.0.0.0 — bind field recorded for
 *                  observability but not applied)
 *   max_sessions = decimal 1..MAX_TCP_SHELL_SESSIONS
 *
 * Unknown keys: recorded as a warning count; parser keeps going.
 * Malformed lines: same.
 *
 * Parser is pure (no I/O, no allocations). The caller reads the file
 * and passes a NUL-terminated buffer. Safe to call from boot or
 * from a test harness with a string literal.
 */

#ifndef TELNETD_CONFIG_H
#define TELNETD_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct telnetd_config {
    bool     enabled;          /* default: false */
    uint16_t port;             /* default: 2323 */
    uint32_t bind_ip;          /* network byte order; default: 0.0.0.0 */
    uint8_t  max_sessions;     /* default: MAX_TCP_SHELL_SESSIONS */

    /* Diagnostic counts — populated by the parser so callers can log
     * what was skipped. */
    uint16_t unknown_keys;
    uint16_t malformed_lines;
};

/* Initialize `cfg` to the compile-time defaults. */
void telnetd_config_defaults(struct telnetd_config *cfg);

/* Parse a NUL-terminated buffer into `cfg`. The buffer is modified
 * in place (trimming, tokenization — the parser rewrites it), so
 * pass a writable copy. Returns 0 on success (config may still have
 * unknown_keys/malformed_lines > 0), -1 if cfg is NULL. */
int telnetd_config_parse(struct telnetd_config *cfg, char *buf, size_t len);

#endif /* TELNETD_CONFIG_H */
