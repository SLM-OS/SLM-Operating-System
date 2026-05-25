/*
 * sshd_shell.c - `sshd` shell command.
 *
 * Subcommands:
 *
 *   sshd                    -> status (alias of `sshd status`)
 *   sshd status             -> running state, port, KEX counters
 *   sshd start [port]       -> bring up the listener; default port 2222
 *   sshd stop               -> tear down the listener + open sessions
 *
 * `sshd_autostart.c` (added in #199c) wraps `sshd_start` behind a
 * config-file driven boot hook parallel to `telnetd_autostart`.
 * Until then this command is the only way to bring the daemon up.
 */

#include "sshd.h"

#include "net.h"
#include "shell.h"
#include "shell_internal.h"
#include "string.h"
#include "uart.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static int parse_decimal_u16(const char *s, uint16_t *out)
{
    if (s == NULL || *s == '\0') return -1;
    uint32_t v = 0;
    while (*s != '\0') {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10u + (uint32_t)(*s - '0');
        if (v > 0xFFFFu) return -1;
        s++;
    }
    *out = (uint16_t)v;
    return 0;
}

static void print_status(void)
{
    struct sshd_stats st;
    sshd_get_stats(&st);
    uart_printf("sshd: %s port=%u accepted=%u kex_ok=%u kex_fail=%u active=%u\r\n",
                st.running ? "running" : "stopped",
                (unsigned)st.port,
                (unsigned)st.connections_accepted,
                (unsigned)st.kex_completed,
                (unsigned)st.kex_failed,
                (unsigned)st.active);
}

static int do_start(int argc, char **argv)
{
    uint16_t port = SSHD_DEFAULT_PORT;
    if (argc >= 3) {
        if (parse_decimal_u16(argv[2], &port) != 0 || port == 0) {
            uart_printf("usage: sshd start [port]\r\n");
            return -1;
        }
    }

    if (!net_is_up()) {
        int rc = net_init();
        if (rc != 0) {
            uart_printf("sshd start: net_init failed (%d)\r\n", rc);
            return rc;
        }
    }

    int rc = sshd_start(port);
    switch (rc) {
    case SSHD_OK:            uart_printf("sshd: listening on port %u\r\n", (unsigned)port); break;
    case SSHD_E_ALREADY_UP:  uart_printf("sshd: already running on a different port (sshd stop first)\r\n"); break;
    case SSHD_E_NO_HOSTKEY:  uart_printf("sshd: host-key generation failed\r\n"); break;
    case SSHD_E_LISTEN_FAIL: uart_printf("sshd: listen on port %u failed\r\n", (unsigned)port); break;
    case SSHD_E_WOLF_INIT:   uart_printf("sshd: wolfSSH init failed\r\n"); break;
    default:                 uart_printf("sshd start: rc=%d\r\n", rc); break;
    }
    return rc;
}

int cmd_sshd(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        print_status();
        return 0;
    }
    if (strcmp(argv[1], "start") == 0) {
        return do_start(argc, argv);
    }
    if (strcmp(argv[1], "stop") == 0) {
        return sshd_stop();
    }
    uart_printf("usage: sshd [status|start [port]|stop]\r\n");
    return -1;
}
