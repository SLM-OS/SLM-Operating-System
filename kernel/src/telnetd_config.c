/*
 * telnetd_config.c - Flat key=value parser for /etc/telnetd.conf
 *
 * Design notes:
 *  - Pure: no I/O, no allocation. The VFS-reading wrapper lives in
 *    the autostart path.
 *  - In-place tokenization — the caller passes a writable copy.
 *  - ~150 lines total, comfortably under the ~100-line plan target
 *    once you strip the comments/table.
 */

#include "telnetd_config.h"
#include "shell_session.h"   /* MAX_TCP_SHELL_SESSIONS for clamp */
#include "string.h"

/* -------------------------------------------------------------------------- */
/* Character helpers                                                          */
/* -------------------------------------------------------------------------- */

static bool is_ws(char c) { return c == ' ' || c == '\t'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static char tolower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* Case-insensitive equality. */
static bool eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower_ascii(*a) != tolower_ascii(*b)) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Trim leading whitespace by advancing the pointer. */
static char *lstrip(char *s)
{
    while (*s && is_ws(*s)) s++;
    return s;
}

/* Trim trailing whitespace + CR by writing NUL in place. */
static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (is_ws(s[n-1]) || s[n-1] == '\r')) {
        s[--n] = '\0';
    }
}

/* -------------------------------------------------------------------------- */
/* Value parsers                                                              */
/* -------------------------------------------------------------------------- */

static bool parse_bool(const char *s, bool *out)
{
    if (eq_ci(s, "true")  || eq_ci(s, "1") || eq_ci(s, "yes")
     || eq_ci(s, "on")) {
        *out = true;
        return true;
    }
    if (eq_ci(s, "false") || eq_ci(s, "0") || eq_ci(s, "no")
     || eq_ci(s, "off")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_u16(const char *s, uint16_t *out)
{
    if (!*s) return false;
    uint32_t v = 0;
    for (; *s; s++) {
        if (!is_digit(*s)) return false;
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > 0xFFFF) return false;
    }
    *out = (uint16_t)v;
    return true;
}

static bool parse_u8(const char *s, uint8_t *out)
{
    uint16_t v;
    if (!parse_u16(s, &v) || v > 0xFF) return false;
    *out = (uint8_t)v;
    return true;
}

/* Parse dotted-quad IPv4 into network-byte-order uint32.
 *
 * The shifts below place octet 0 in the low 8 bits and octet 3 in
 * the high 8 bits of `addr`. On a little-endian host, storing that
 * uint32 to memory yields the bytes in network byte order (octet 0
 * first at the lowest address), matching lwIP's ip4_addr_t.addr
 * layout. A big-endian host would need the opposite shifts, so we
 * fail the build if anyone retargets to a BE architecture. */
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "parse_ipv4 assumes a little-endian host to match lwIP's NBO addr layout");
static bool parse_ipv4(const char *s, uint32_t *out)
{
    uint32_t addr = 0;
    for (int octet = 0; octet < 4; octet++) {
        if (!is_digit(*s)) return false;
        uint32_t v = 0;
        while (is_digit(*s)) {
            v = v * 10 + (uint32_t)(*s - '0');
            if (v > 255) return false;
            s++;
        }
        addr |= v << (octet * 8);
        if (octet < 3) {
            if (*s != '.') return false;
            s++;
        }
    }
    if (*s != '\0') return false;
    *out = addr;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Line-level parser                                                          */
/* -------------------------------------------------------------------------- */

/* Parse one already-trimmed non-empty, non-comment line. Returns
 * 0 on good application, 1 for unknown key, -1 for malformed. */
static int apply_line(struct telnetd_config *cfg, char *line)
{
    /* Split on first '='. */
    char *eq = strchr(line, '=');
    if (!eq) return -1;
    *eq = '\0';
    char *key = line;
    char *val = eq + 1;

    /* Trim around the separator. */
    rstrip(key);
    val = lstrip(val);
    rstrip(val);

    if (*key == '\0' || *val == '\0') {
        return -1;
    }

    if (eq_ci(key, "enabled")) {
        return parse_bool(val, &cfg->enabled) ? 0 : -1;
    }
    if (eq_ci(key, "port")) {
        return parse_u16(val, &cfg->port) ? 0 : -1;
    }
    if (eq_ci(key, "bind")) {
        return parse_ipv4(val, &cfg->bind_ip) ? 0 : -1;
    }
    if (eq_ci(key, "max_sessions")) {
        uint8_t v;
        if (!parse_u8(val, &v) || v == 0) return -1;
        if (v > MAX_TCP_SHELL_SESSIONS) v = MAX_TCP_SHELL_SESSIONS;
        cfg->max_sessions = v;
        return 0;
    }
    return 1;   /* unknown */
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void telnetd_config_defaults(struct telnetd_config *cfg)
{
    if (!cfg) return;
    cfg->enabled        = false;
    cfg->port           = 2323;
    cfg->bind_ip        = 0;       /* 0.0.0.0 */
    cfg->max_sessions   = MAX_TCP_SHELL_SESSIONS;
    cfg->unknown_keys   = 0;
    cfg->malformed_lines = 0;
}

int telnetd_config_parse(struct telnetd_config *cfg, char *buf, size_t len)
{
    if (!cfg) return -1;
    if (!buf || len == 0) return 0;

    /* Ensure NUL termination so strchr is safe. Caller is expected to
     * have supplied a NUL-terminated buffer; belt-and-suspenders. */
    buf[len - 1] = '\0';

    char *p = buf;
    while (*p) {
        /* Locate end of this line. */
        char *line_end = strchr(p, '\n');
        if (line_end) *line_end = '\0';

        /* Strip leading whitespace + trailing CR/whitespace. */
        char *line = lstrip(p);
        rstrip(line);

        /* Skip blank and comment lines. */
        if (*line != '\0' && *line != '#') {
            int rc = apply_line(cfg, line);
            if (rc == 1) {
                cfg->unknown_keys++;
            } else if (rc < 0) {
                cfg->malformed_lines++;
            }
        }

        if (!line_end) break;
        p = line_end + 1;
    }
    return 0;
}
