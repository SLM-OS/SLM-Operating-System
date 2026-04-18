/*
 * telnetd_autostart.h - Boot-time entry for the TCP shell daemon
 *
 * Consulted once per boot from shell_init (see kernel/src/shell.c).
 * Decides whether to bring up the telnetd listener automatically
 * based on (in order of precedence):
 *
 *   1. /etc/telnetd.conf in the VFS — parsed via telnetd_config_parse.
 *      Missing file is not an error.
 *   2. NET_TELNETD_AUTOSTART build define — default if no config file
 *      is present.
 *
 * If the resolved config has enabled=true, this function will
 * net_init() (if networking isn't already up) and call
 * tcp_shell_server_start(port). Returns silently when telnetd stays
 * off — a boot with no config file and NET_TELNETD_AUTOSTART=OFF
 * (the default) is a no-op.
 *
 * Logs a one-liner to UART on either outcome so the operator can
 * see what happened without running `telnetd status`.
 */

#ifndef TELNETD_AUTOSTART_H
#define TELNETD_AUTOSTART_H

void telnetd_autostart(void);

#endif /* TELNETD_AUTOSTART_H */
