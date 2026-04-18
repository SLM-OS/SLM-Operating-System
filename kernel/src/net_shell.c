/**
 * Network Shell Commands for SLM-OS
 *
 * Implements ping, ifconfig, and netstat commands for the debug shell.
 */

#include "net.h"
#include "shell.h"
#include "shell_session.h"   /* MAX_TCP_SHELL_SESSIONS */
#include "tcp_shell_server.h"
#include "shell_io_tcp.h"
#include "debug.h"
#include "timer.h"
#include "arch/sys_arch.h"

#include <stddef.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Ping Command                                                                */
/* -------------------------------------------------------------------------- */

static volatile bool ping_done;
static volatile bool ping_success;
static volatile uint32_t ping_rtt;

static void ping_callback(uint16_t seq, uint32_t addr, uint32_t rtt_ms,
                         bool success, void *user) {
    (void)seq;
    (void)addr;
    (void)user;

    ping_done = true;
    ping_success = success;
    ping_rtt = rtt_ms;
}

/**
 * ping <ip> [count] - Send ICMP echo requests
 */
static int cmd_ping(int argc, char *argv[]) {
    if (argc < 2) {
        shell_printf("Usage: ping <ip_address> [count]\n");
        shell_printf("  Example: ping 10.0.2.2\n");
        return -1;
    }

    if (!net_is_up()) {
        shell_printf("Network not initialized\n");
        return -1;
    }

    uint32_t ip_addr;
    if (net_str_to_ip(argv[1], &ip_addr) < 0) {
        shell_printf("Invalid IP address: %s\n", argv[1]);
        return -1;
    }

    int count = 4;  /* Default ping count */
    if (argc >= 3) {
        count = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) {
            count = count * 10 + (*p - '0');
        }
        if (count <= 0 || count > 100) {
            count = 4;
        }
    }

    char ip_str[16];
    net_ip_to_str(ip_addr, ip_str);
    shell_printf("PING %s: %d packets\n", ip_str, count);

    int sent = 0;
    int received = 0;
    uint32_t min_rtt = 0xFFFFFFFF;
    uint32_t max_rtt = 0;
    uint32_t total_rtt = 0;

    for (int i = 0; i < count; i++) {
        ping_done = false;
        ping_success = false;
        ping_rtt = 0;

        if (net_ping(ip_addr, i + 1, ping_callback, NULL) < 0) {
            shell_printf("Failed to send ping\n");
            continue;
        }
        sent++;

        /* Wait for response (poll network) */
        uint32_t start = sys_now();
        while (!ping_done && (sys_now() - start) < 2000) {
            net_poll();
        }

        if (ping_success) {
            shell_printf("Reply from %s: seq=%d time=%u ms\n",
                       ip_str, i + 1, ping_rtt);
            received++;
            total_rtt += ping_rtt;
            if (ping_rtt < min_rtt) min_rtt = ping_rtt;
            if (ping_rtt > max_rtt) max_rtt = ping_rtt;
        } else {
            shell_printf("Request timeout for seq=%d\n", i + 1);
        }

        /* Wait 1 second between pings */
        if (i < count - 1) {
            uint32_t wait_start = sys_now();
            while (sys_now() - wait_start < 1000) {
                net_poll();
            }
        }
    }

    /* Print summary */
    shell_printf("\n--- %s ping statistics ---\n", ip_str);
    shell_printf("%d packets transmitted, %d received, %d%% packet loss\n",
               sent, received, sent > 0 ? ((sent - received) * 100 / sent) : 0);

    if (received > 0) {
        shell_printf("rtt min/avg/max = %u/%u/%u ms\n",
                   min_rtt, total_rtt / received, max_rtt);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Ifconfig Command                                                            */
/* -------------------------------------------------------------------------- */

/**
 * ifconfig - Display or configure network interface
 */
static int cmd_ifconfig(int argc, char *argv[]) {
    if (!net_is_up()) {
        shell_printf("Network not initialized\n");
        return -1;
    }

    struct net_info info;
    if (net_get_info(&info) < 0) {
        shell_printf("Failed to get network info\n");
        return -1;
    }

    if (argc == 1) {
        /* Display current configuration */
        char ip_str[16], nm_str[16], gw_str[16];
        net_ip_to_str(info.ip_addr, ip_str);
        net_ip_to_str(info.netmask, nm_str);
        net_ip_to_str(info.gateway, gw_str);

        const char *dhcp_label;
        switch (info.dhcp_status) {
        case NET_DHCP_BOUND:    dhcp_label = "DHCP(bound)";    break;
        case NET_DHCP_PENDING:  dhcp_label = "DHCP(pending)";  break;
        case NET_DHCP_FAILED:   dhcp_label = "DHCP(failed)";   break;
        case NET_DHCP_DISABLED:
        default:                dhcp_label = "STATIC";         break;
        }

        shell_printf("sl0: flags=%s%s\n",
                   info.link_up ? "UP," : "DOWN,",
                   dhcp_label);
        shell_printf("     ether %02x:%02x:%02x:%02x:%02x:%02x\n",
                   info.mac[0], info.mac[1], info.mac[2],
                   info.mac[3], info.mac[4], info.mac[5]);
        shell_printf("     inet %s  netmask %s\n", ip_str, nm_str);
        shell_printf("     gateway %s\n", gw_str);
        return 0;
    }

    /* Check for configuration commands */
    if (argc >= 2 && strcmp(argv[1], "dhcp") == 0) {
        if (net_enable_dhcp() < 0) {
            shell_printf("Failed to enable DHCP\n");
            return -1;
        }
        shell_printf("DHCP enabled\n");
        return 0;
    }

    if (argc >= 4) {
        /* ifconfig <ip> <netmask> <gateway> */
        uint32_t ip, nm, gw;
        if (net_str_to_ip(argv[1], &ip) < 0) {
            shell_printf("Invalid IP address: %s\n", argv[1]);
            return -1;
        }
        if (net_str_to_ip(argv[2], &nm) < 0) {
            shell_printf("Invalid netmask: %s\n", argv[2]);
            return -1;
        }
        if (net_str_to_ip(argv[3], &gw) < 0) {
            shell_printf("Invalid gateway: %s\n", argv[3]);
            return -1;
        }

        if (net_set_static_ip(ip, nm, gw) < 0) {
            shell_printf("Failed to set IP configuration\n");
            return -1;
        }

        char ip_str[16];
        net_ip_to_str(ip, ip_str);
        shell_printf("IP set to %s\n", ip_str);
        return 0;
    }

    shell_printf("Usage:\n");
    shell_printf("  ifconfig              - Show configuration\n");
    shell_printf("  ifconfig dhcp         - Enable DHCP\n");
    shell_printf("  ifconfig <ip> <mask> <gw> - Set static IP\n");
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Net Command (init/status)                                                   */
/* -------------------------------------------------------------------------- */

/**
 * net init|status - Initialize or show network status
 */
static int cmd_net(int argc, char *argv[]) {
    if (argc < 2) {
        shell_printf("Usage: net <init|status>\n");
        shell_printf("  net init   - Initialize network subsystem\n");
        shell_printf("  net status - Show network status\n");
        return -1;
    }

    if (strcmp(argv[1], "init") == 0) {
        if (net_is_up()) {
            shell_printf("Network already initialized\n");
            return 0;
        }
        shell_printf("Initializing network...\n");
        if (net_init() < 0) {
            shell_printf("Network initialization failed\n");
            return -1;
        }
        shell_printf("Network initialized successfully\n");
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        if (!net_is_up()) {
            shell_printf("Network: DOWN (not initialized)\n");
            shell_printf("  Use 'net init' to initialize\n");
            return 0;
        }

        struct net_info info;
        net_get_info(&info);

        char ip_str[16];
        net_ip_to_str(info.ip_addr, ip_str);

        shell_printf("Network: UP\n");
        shell_printf("  Interface: sl0\n");
        shell_printf("  IP Address: %s\n", ip_str);
        shell_printf("  Link: %s\n", info.link_up ? "connected" : "disconnected");
        return 0;
    }

    shell_printf("Unknown subcommand: %s\n", argv[1]);
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Netstat Command                                                             */
/* -------------------------------------------------------------------------- */

/**
 * netstat - Display network statistics
 */
static int cmd_netstat(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    if (!net_is_up()) {
        shell_printf("Network not initialized\n");
        return -1;
    }

    struct net_stats stats;
    net_get_stats(&stats);

    shell_printf("Network Statistics:\n");
    shell_printf("  RX packets: %llu  bytes: %llu\n",
               (unsigned long long)stats.rx_packets,
               (unsigned long long)stats.rx_bytes);
    shell_printf("  TX packets: %llu  bytes: %llu\n",
               (unsigned long long)stats.tx_packets,
               (unsigned long long)stats.tx_bytes);
    shell_printf("  RX errors:  %llu  dropped: %llu  no_buffers: %llu\n",
               (unsigned long long)stats.rx_errors,
               (unsigned long long)stats.rx_dropped,
               (unsigned long long)stats.rx_no_buffers);
    shell_printf("  TX errors:  %llu\n",
               (unsigned long long)stats.tx_errors);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* telnetd Command (Phase 3)                                                   */
/*                                                                            */
/* The `telnetd` name supersedes `tcpsh` from Phases 1-2. `tcpsh` is still    */
/* registered as an alias so existing scripts and muscle memory keep working. */
/* -------------------------------------------------------------------------- */

#include "arch/sys_arch.h"   /* sys_now() for session age display */

/* Parse an unsigned decimal in [0, 65535]. Returns -1 on error. */
static int parse_port(const char *s, uint16_t *out)
{
    if (!s || !*s) return -1;
    uint32_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > 65535) return -1;
    }
    *out = (uint16_t)v;
    return 0;
}

/* Parse an unsigned decimal in [0, UINT32_MAX]. Returns -1 on error. */
static int parse_u32(const char *s, uint32_t *out)
{
    if (!s || !*s) return -1;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > 0xFFFFFFFFULL) return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static void print_usage(const char *name)
{
    shell_printf("Usage: %s <start [port] | stop | status | sessions | kick <id>>\n",
                 name);
}

/* Visitor for `telnetd sessions`. Counts rows printed; also prints
 * the entry as a formatted row. */
struct sessions_visitor_ctx {
    uint32_t now;
    uint32_t count;
};

static bool sessions_visitor(const struct tcp_session_info *info, void *c)
{
    struct sessions_visitor_ctx *sv = c;
    char ip[16];
    net_ip_to_str(info->peer_ip, ip);
    uint32_t age_ms = sv->now - info->connected_at;
    shell_printf("  %3u  %-15s  %5u  %u s\n",
                 (unsigned)info->session_id, ip,
                 (unsigned)info->peer_port,
                 (unsigned)(age_ms / 1000));
    sv->count++;
    return true;
}

static int cmd_telnetd(int argc, char *argv[])
{
    const char *name = argv[0];   /* "telnetd" or "tcpsh" alias */

    if (argc < 2) {
        print_usage(name);
        return -1;
    }

    if (strcmp(argv[1], "start") == 0) {
        if (!net_is_up()) {
            shell_printf("%s: network not initialized (run `net init` first)\n",
                         name);
            return -1;
        }
        uint16_t port = 2323;
        if (argc >= 3) {
            if (parse_port(argv[2], &port) != 0) {
                shell_printf("%s: invalid port\n", name);
                return -1;
            }
        }
        int rc = tcp_shell_server_start(port);
        if (rc == 0) {
            shell_printf("%s: listening on port %u\n", name, (unsigned)port);
            return 0;
        }
        shell_printf("%s: start failed (%d)\n", name, rc);
        return rc;
    }

    if (strcmp(argv[1], "stop") == 0) {
        if (!tcp_shell_server_running()) {
            shell_printf("%s: not running\n", name);
            return 0;
        }
        tcp_shell_server_stop();
        shell_printf("%s: stopped\n", name);
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        if (tcp_shell_server_running()) {
            shell_printf("%s: running on port %u — accepted=%u active=%u max=%u\n",
                         name,
                         (unsigned)tcp_shell_server_port(),
                         (unsigned)tcp_shell_server_accepted(),
                         (unsigned)shell_io_tcp_active_count(),
                         (unsigned)MAX_TCP_SHELL_SESSIONS);
        } else {
            shell_printf("%s: stopped\n", name);
        }
        return 0;
    }

    if (strcmp(argv[1], "sessions") == 0) {
        if (!tcp_shell_server_running()) {
            shell_printf("%s: not running\n", name);
            return 0;
        }
        shell_printf("   ID  Peer IP          Port  Connected\n");
        shell_printf("  ---  ---------------  ----  ---------\n");
        struct sessions_visitor_ctx sv = { .now = sys_now(), .count = 0 };
        shell_io_tcp_foreach(sessions_visitor, &sv);
        if (sv.count == 0) {
            shell_printf("  (no active sessions)\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "kick") == 0) {
        if (argc < 3) {
            shell_printf("Usage: %s kick <session-id>\n", name);
            return -1;
        }
        uint32_t id = 0;
        if (parse_u32(argv[2], &id) != 0) {
            shell_printf("%s: invalid session id\n", name);
            return -1;
        }
        if (shell_io_tcp_kick(id)) {
            shell_printf("%s: kicked session %u\n", name, (unsigned)id);
            return 0;
        }
        shell_printf("%s: no active session with id %u\n", name, (unsigned)id);
        return -1;
    }

    shell_printf("%s: unknown subcommand '%s'\n", name, argv[1]);
    print_usage(name);
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Command Registration                                                        */
/* -------------------------------------------------------------------------- */

/* Command definitions. `tcpsh` is an alias for the Phase-1/2-era name
 * and dispatches through the same handler as `telnetd`; the handler
 * uses argv[0] for its "Usage:" / status prefix so either name
 * produces self-consistent output. */
static const shell_cmd_t net_commands[] = {
    {"net",      cmd_net,      "Network control (init/status)",              true},
    {"ping",     cmd_ping,     "Send ICMP echo request",                     true},
    {"ifconfig", cmd_ifconfig, "Network interface config",                   true},
    {"netstat",  cmd_netstat,  "Network statistics",                         false},
    {"telnetd",  cmd_telnetd,  "Telnet shell daemon (start|stop|status|sessions|kick)", true},
    {"tcpsh",    cmd_telnetd,  "Alias for telnetd (legacy name)",            true},
};

/**
 * Register network shell commands
 *
 * Called during shell initialization to add network commands.
 */
void net_shell_init(void) {
    for (size_t i = 0; i < sizeof(net_commands) / sizeof(net_commands[0]); i++) {
        shell_register_command(&net_commands[i]);
    }
}
