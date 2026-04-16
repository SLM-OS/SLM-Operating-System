/**
 * Network Shell Commands for SLM-OS
 *
 * Implements ping, ifconfig, and netstat commands for the debug shell.
 */

#include "net.h"
#include "shell.h"
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
        uart_printf("Usage: ping <ip_address> [count]\n");
        uart_printf("  Example: ping 10.0.2.2\n");
        return -1;
    }

    if (!net_is_up()) {
        uart_printf("Network not initialized\n");
        return -1;
    }

    uint32_t ip_addr;
    if (net_str_to_ip(argv[1], &ip_addr) < 0) {
        uart_printf("Invalid IP address: %s\n", argv[1]);
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
    uart_printf("PING %s: %d packets\n", ip_str, count);

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
            uart_printf("Failed to send ping\n");
            continue;
        }
        sent++;

        /* Wait for response (poll network) */
        uint32_t start = sys_now();
        while (!ping_done && (sys_now() - start) < 2000) {
            net_poll();
        }

        if (ping_success) {
            uart_printf("Reply from %s: seq=%d time=%u ms\n",
                       ip_str, i + 1, ping_rtt);
            received++;
            total_rtt += ping_rtt;
            if (ping_rtt < min_rtt) min_rtt = ping_rtt;
            if (ping_rtt > max_rtt) max_rtt = ping_rtt;
        } else {
            uart_printf("Request timeout for seq=%d\n", i + 1);
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
    uart_printf("\n--- %s ping statistics ---\n", ip_str);
    uart_printf("%d packets transmitted, %d received, %d%% packet loss\n",
               sent, received, sent > 0 ? ((sent - received) * 100 / sent) : 0);

    if (received > 0) {
        uart_printf("rtt min/avg/max = %u/%u/%u ms\n",
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
        uart_printf("Network not initialized\n");
        return -1;
    }

    struct net_info info;
    if (net_get_info(&info) < 0) {
        uart_printf("Failed to get network info\n");
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

        uart_printf("sl0: flags=%s%s\n",
                   info.link_up ? "UP," : "DOWN,",
                   dhcp_label);
        uart_printf("     ether %02x:%02x:%02x:%02x:%02x:%02x\n",
                   info.mac[0], info.mac[1], info.mac[2],
                   info.mac[3], info.mac[4], info.mac[5]);
        uart_printf("     inet %s  netmask %s\n", ip_str, nm_str);
        uart_printf("     gateway %s\n", gw_str);
        return 0;
    }

    /* Check for configuration commands */
    if (argc >= 2 && strcmp(argv[1], "dhcp") == 0) {
        if (net_enable_dhcp() < 0) {
            uart_printf("Failed to enable DHCP\n");
            return -1;
        }
        uart_printf("DHCP enabled\n");
        return 0;
    }

    if (argc >= 4) {
        /* ifconfig <ip> <netmask> <gateway> */
        uint32_t ip, nm, gw;
        if (net_str_to_ip(argv[1], &ip) < 0) {
            uart_printf("Invalid IP address: %s\n", argv[1]);
            return -1;
        }
        if (net_str_to_ip(argv[2], &nm) < 0) {
            uart_printf("Invalid netmask: %s\n", argv[2]);
            return -1;
        }
        if (net_str_to_ip(argv[3], &gw) < 0) {
            uart_printf("Invalid gateway: %s\n", argv[3]);
            return -1;
        }

        if (net_set_static_ip(ip, nm, gw) < 0) {
            uart_printf("Failed to set IP configuration\n");
            return -1;
        }

        char ip_str[16];
        net_ip_to_str(ip, ip_str);
        uart_printf("IP set to %s\n", ip_str);
        return 0;
    }

    uart_printf("Usage:\n");
    uart_printf("  ifconfig              - Show configuration\n");
    uart_printf("  ifconfig dhcp         - Enable DHCP\n");
    uart_printf("  ifconfig <ip> <mask> <gw> - Set static IP\n");
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
        uart_printf("Usage: net <init|status>\n");
        uart_printf("  net init   - Initialize network subsystem\n");
        uart_printf("  net status - Show network status\n");
        return -1;
    }

    if (strcmp(argv[1], "init") == 0) {
        if (net_is_up()) {
            uart_printf("Network already initialized\n");
            return 0;
        }
        uart_printf("Initializing network...\n");
        if (net_init() < 0) {
            uart_printf("Network initialization failed\n");
            return -1;
        }
        uart_printf("Network initialized successfully\n");
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        if (!net_is_up()) {
            uart_printf("Network: DOWN (not initialized)\n");
            uart_printf("  Use 'net init' to initialize\n");
            return 0;
        }

        struct net_info info;
        net_get_info(&info);

        char ip_str[16];
        net_ip_to_str(info.ip_addr, ip_str);

        uart_printf("Network: UP\n");
        uart_printf("  Interface: sl0\n");
        uart_printf("  IP Address: %s\n", ip_str);
        uart_printf("  Link: %s\n", info.link_up ? "connected" : "disconnected");
        return 0;
    }

    uart_printf("Unknown subcommand: %s\n", argv[1]);
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
        uart_printf("Network not initialized\n");
        return -1;
    }

    struct net_stats stats;
    net_get_stats(&stats);

    uart_printf("Network Statistics:\n");
    uart_printf("  RX packets: %llu  bytes: %llu\n",
               (unsigned long long)stats.rx_packets,
               (unsigned long long)stats.rx_bytes);
    uart_printf("  TX packets: %llu  bytes: %llu\n",
               (unsigned long long)stats.tx_packets,
               (unsigned long long)stats.tx_bytes);
    uart_printf("  RX errors:  %llu  dropped: %llu\n",
               (unsigned long long)stats.rx_errors,
               (unsigned long long)stats.rx_dropped);
    uart_printf("  TX errors:  %llu\n",
               (unsigned long long)stats.tx_errors);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Command Registration                                                        */
/* -------------------------------------------------------------------------- */

/* Command definitions */
static const shell_cmd_t net_commands[] = {
    {"net", cmd_net, "Network control (init/status)"},
    {"ping", cmd_ping, "Send ICMP echo request"},
    {"ifconfig", cmd_ifconfig, "Network interface config"},
    {"netstat", cmd_netstat, "Network statistics"},
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
