/*
 * telnetd_autostart.c - Boot-time telnetd autostart
 *
 * See telnetd_autostart.h. Reads /etc/telnetd.conf (or falls back to
 * the NET_TELNETD_AUTOSTART compile-time default) and, if the
 * resolved config enables telnetd, brings lwIP up and starts the
 * listener.
 */

#include "telnetd_autostart.h"
#include "telnetd_config.h"
#include "tcp_shell_server.h"
#include "net.h"
#include "uart.h"
#include "vfs.h"

#include <stddef.h>
#include <stdint.h>

#define TELNETD_CONF_PATH  "/etc/telnetd.conf"
#define TELNETD_CONF_MAX   512   /* bytes read from VFS */

void telnetd_autostart(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);

#if defined(NET_TELNETD_AUTOSTART) && NET_TELNETD_AUTOSTART
    /* Compile-time opt-in: enable-by-default, but the config file
     * (if present) can still flip it back off. */
    cfg.enabled = true;
#endif

    char buf[TELNETD_CONF_MAX];
    int n = vfs_read_path(TELNETD_CONF_PATH, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        telnetd_config_parse(&cfg, buf, (size_t)n + 1);
        if (cfg.unknown_keys || cfg.malformed_lines) {
            uart_printf("[TELNETD] %s: %u unknown key(s), %u malformed line(s)\r\n",
                        TELNETD_CONF_PATH,
                        (unsigned)cfg.unknown_keys,
                        (unsigned)cfg.malformed_lines);
        }
    }

    if (!cfg.enabled) {
        return;   /* Silent no-op — default for an unconfigured build. */
    }

    /* Bring lwIP up if it isn't already. A user who ran `net init`
     * from an init script or Lua before shell_init finished will
     * satisfy this check, and we skip the redundant call. */
    if (!net_is_up()) {
        if (net_init() != 0) {
            uart_printf("[TELNETD] autostart: net_init failed — skipping\r\n");
            return;
        }
    }

    int rc = tcp_shell_server_start(cfg.port);
    if (rc != 0) {
        uart_printf("[TELNETD] autostart: listen on port %u failed (%d)\r\n",
                    (unsigned)cfg.port, rc);
    } else {
        uart_printf("[TELNETD] autostart: listening on port %u\r\n",
                    (unsigned)cfg.port);
    }
}
