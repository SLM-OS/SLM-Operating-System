/*
 * sshd_autostart.c - Boot-time entry that brings sshd up from config.
 *
 * Mirrors `kernel/src/telnetd_autostart.c`. Reads
 * `/mnt/files/etc/sshd.conf` if present (flat `key=value` lines):
 *
 *   enabled=true | false
 *   port=<decimal 0..65535>
 *
 * Falls back to the compile-time `NET_SSHD_AUTOSTART` default when
 * the file is absent. Brings lwIP up via `net_init()` if it isn't
 * already, then calls `sshd_start(port)`.
 *
 * Bootstrap safety: this function does NOT bypass any user-auth
 * gating. When #199d (#896) lands, `sshd_start` itself refuses
 * connections until the operator runs `passwd root` on the console.
 * Until then, an `enable=1` config on a fresh kernel exposes an
 * accept-anything daemon — DO NOT flip the default-on knob in
 * CMakeLists.txt until the bootstrap gate is in place.
 */

#include "sshd_autostart.h"
#include "sshd.h"

#include "net.h"
#include "string.h"
#include "uart.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SSHD_CONF_PATH    "/mnt/files/etc/sshd.conf"
#define SSHD_CONF_MAX_SZ  256

struct sshd_autostart_cfg {
    bool     enabled;
    uint16_t port;
};

static int parse_decimal_u16(const char *s, uint16_t *out)
{
    if (!s || !*s) return -1;
    uint32_t v = 0;
    while (*s != '\0' && *s != '\n' && *s != '\r' && *s != ' ' && *s != '\t') {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10u + (uint32_t)(*s - '0');
        if (v > 0xFFFFu) return -1;
        s++;
    }
    *out = (uint16_t)v;
    return 0;
}

/* True iff `s` (with optional trailing whitespace / CR) exactly equals
 * `want`. Avoids strncmp's prefix-matching trap where "1abc" would
 * compare equal to "1". */
static bool token_equals(const char *s, const char *want)
{
    size_t i = 0;
    while (want[i] != '\0') {
        if (s[i] != want[i]) return false;
        i++;
    }
    char trailing = s[i];
    return trailing == '\0' || trailing == '\r' || trailing == '\n'
           || trailing == ' ' || trailing == '\t';
}

static bool parse_bool(const char *s)
{
    if (!s) return false;
    return token_equals(s, "1")
        || token_equals(s, "true")
        || token_equals(s, "yes")
        || token_equals(s, "on");
}

static void parse_config(struct sshd_autostart_cfg *cfg, char *buf, size_t len)
{
    /* Tokenize line by line. */
    size_t i = 0;
    while (i < len) {
        char *line = buf + i;
        while (i < len && buf[i] != '\n' && buf[i] != '\0') {
            i++;
        }
        if (i < len) {
            buf[i++] = '\0';
        }

        /* Skip leading whitespace + comments. */
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (*line == '#' || *line == '\0' || *line == '\r') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') {
            val++;
        }

        if (strcmp(key, "enabled") == 0) {
            cfg->enabled = parse_bool(val);
        } else if (strcmp(key, "port") == 0) {
            uint16_t p;
            if (parse_decimal_u16(val, &p) == 0 && p > 0u) cfg->port = p;
        }
    }
}

void sshd_autostart(void)
{
    struct sshd_autostart_cfg cfg;
    cfg.enabled = false;
    cfg.port    = SSHD_DEFAULT_PORT;

#if defined(NET_SSHD_AUTOSTART) && NET_SSHD_AUTOSTART
    /* Compile-time opt-in. The config file (if present) can still
     * flip it back off. */
    cfg.enabled = true;
#endif

    char buf[SSHD_CONF_MAX_SZ];
    int n = vfs_read_path(SSHD_CONF_PATH, buf, sizeof(buf) - 1u, 0);
    if (n > 0) {
        buf[n] = '\0';
        parse_config(&cfg, buf, (size_t)n + 1u);
    }

    if (!cfg.enabled) {
        /* Silent no-op — the default. */
        return;
    }

    /* Bring lwIP up if it isn't already (same pattern as telnetd). */
    if (!net_is_up()) {
        if (net_init() != 0) {
            uart_printf("[SSHD] autostart: net_init failed — skipping\r\n");
            return;
        }
    }

    int rc = sshd_start(cfg.port);
    if (rc == SSHD_OK) {
        uart_printf("[SSHD] autostart: listening on port %u\r\n",
                    (unsigned)cfg.port);
#if defined(NET_SSHD_DEMO_ALLOW_ALL) && NET_SSHD_DEMO_ALLOW_ALL
        /* Loud on every boot — the auth gate is the #199c demo stub
         * that accepts every user/password. #199d (#896) replaces
         * this with a real scrypt-against-/etc/passwd check; until
         * then this banner makes the exposure unmissable in the log. */
        uart_printf("[SSHD] WARNING: user-auth is allow-all "
                    "(#199c demo stub) — do not enable on a "
                    "production network until #199d lands\r\n");
#endif
    } else {
        uart_printf("[SSHD] autostart: sshd_start failed (%d)\r\n", rc);
    }
}
