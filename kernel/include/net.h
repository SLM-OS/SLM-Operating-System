/**
 * Network Subsystem API for SLM-OS
 *
 * High-level network interface that abstracts the lwIP stack and
 * VirtIO-Net driver. Provides simple packet send/receive and
 * configuration functions.
 */

#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Network Subsystem Initialization                                            */
/* -------------------------------------------------------------------------- */

/**
 * Initialize the network subsystem
 *
 * This function:
 * 1. Initializes the VirtIO-Net driver
 * 2. Initializes the lwIP TCP/IP stack
 * 3. Configures the network interface
 * 4. Starts DHCP if enabled
 *
 * @return  0 on success, negative error code on failure
 */
int net_init(void);

/**
 * Process pending network events
 *
 * This function must be called periodically (e.g., from main loop or
 * a dedicated network task) to:
 * - Process received packets
 * - Handle lwIP timers
 * - Service TCP/UDP connections
 *
 * In a single-threaded (NO_SYS) lwIP configuration, this is the
 * main entry point for network processing.
 */
void net_poll(void);

/* -------------------------------------------------------------------------- */
/* Network Interface Configuration                                             */
/* -------------------------------------------------------------------------- */

/**
 * DHCP client state.
 *
 * NET_DHCP_DISABLED: DHCP not running (static IP in use)
 * NET_DHCP_PENDING:  DHCP started, no lease yet
 * NET_DHCP_BOUND:    DHCP bound an address; ip_addr is from DHCP server
 * NET_DHCP_FAILED:   DHCP timed out or failed; fell back to static IP
 */
enum net_dhcp_status {
    NET_DHCP_DISABLED = 0,
    NET_DHCP_PENDING  = 1,
    NET_DHCP_BOUND    = 2,
    NET_DHCP_FAILED   = 3,
};

/**
 * Network interface information
 */
struct net_info {
    uint8_t  mac[6];        /* MAC address */
    uint32_t ip_addr;       /* IPv4 address (network byte order) */
    uint32_t netmask;       /* Subnet mask (network byte order) */
    uint32_t gateway;       /* Default gateway (network byte order) */
    bool     link_up;       /* Physical link status */
    bool     dhcp_enabled;  /* DHCP in use (any active state) */
    enum net_dhcp_status dhcp_status;  /* Detailed DHCP state */
};

/**
 * Get current network interface information
 *
 * @param info  Pointer to structure to fill
 * @return  0 on success, negative on error
 */
int net_get_info(struct net_info *info);

/**
 * Set static IP configuration
 *
 * Disables DHCP and sets a static IP address.
 *
 * @param ip_addr   IPv4 address (network byte order)
 * @param netmask   Subnet mask (network byte order)
 * @param gateway   Default gateway (network byte order)
 * @return  0 on success, negative on error
 */
int net_set_static_ip(uint32_t ip_addr, uint32_t netmask, uint32_t gateway);

/**
 * Enable DHCP client
 *
 * @return  0 on success, negative on error
 */
int net_enable_dhcp(void);

/* -------------------------------------------------------------------------- */
/* ICMP (Ping) Support                                                         */
/* -------------------------------------------------------------------------- */

/**
 * Ping result callback
 *
 * @param seq       Sequence number
 * @param addr      Target IP address (network byte order)
 * @param rtt_ms    Round-trip time in milliseconds
 * @param success   true if reply received, false if timeout
 * @param user      User-provided context pointer
 */
typedef void (*ping_callback_t)(uint16_t seq, uint32_t addr, uint32_t rtt_ms,
                                bool success, void *user);

/**
 * Send an ICMP echo request (ping)
 *
 * @param addr      Target IP address (network byte order)
 * @param seq       Sequence number
 * @param callback  Callback for result (may be NULL for fire-and-forget)
 * @param user      User context passed to callback
 * @return  0 if request sent, negative on error
 */
int net_ping(uint32_t addr, uint16_t seq, ping_callback_t callback, void *user);

/* -------------------------------------------------------------------------- */
/* Network Statistics                                                          */
/* -------------------------------------------------------------------------- */

/**
 * Network statistics
 */
struct net_stats {
    uint64_t rx_packets;    /* Packets received */
    uint64_t tx_packets;    /* Packets transmitted */
    uint64_t rx_bytes;      /* Bytes received */
    uint64_t tx_bytes;      /* Bytes transmitted */
    uint64_t rx_errors;     /* Receive errors */
    uint64_t tx_errors;     /* Transmit errors */
    uint64_t rx_dropped;    /* Packets dropped (no buffer) */
};

/**
 * Get network statistics
 *
 * @param stats  Pointer to structure to fill
 */
void net_get_stats(struct net_stats *stats);

/* -------------------------------------------------------------------------- */
/* Utility Functions                                                           */
/* -------------------------------------------------------------------------- */

/**
 * Convert IPv4 address to string
 *
 * @param addr      IPv4 address (network byte order)
 * @param buf       Buffer for string (at least 16 bytes)
 * @return  Pointer to buf
 */
char *net_ip_to_str(uint32_t addr, char *buf);

/**
 * Convert string to IPv4 address
 *
 * @param str       IP address string (e.g., "10.0.2.15")
 * @param addr      Output: IPv4 address (network byte order)
 * @return  0 on success, negative on parse error
 */
int net_str_to_ip(const char *str, uint32_t *addr);

/**
 * Create IPv4 address from octets
 *
 * @param a, b, c, d  Address octets (a.b.c.d)
 * @return  IPv4 address in network byte order
 */
static inline uint32_t net_ip4_addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a) | ((uint32_t)b << 8) |
           ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

/**
 * Check if network is initialized
 *
 * @return  true if network subsystem is ready
 */
bool net_is_up(void);

/* -------------------------------------------------------------------------- */
/* Shell Command Registration                                                  */
/* -------------------------------------------------------------------------- */

/**
 * Register network shell commands
 *
 * Registers ping, ifconfig, and netstat commands with the shell.
 * Called automatically by shell_init() on QEMU platform.
 */
void net_shell_init(void);

#endif /* NET_H */
