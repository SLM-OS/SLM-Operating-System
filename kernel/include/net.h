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
/* Error codes                                                                 */
/* -------------------------------------------------------------------------- */

/*
 * Networking-specific error codes (#213).
 *
 * Public net_* functions return negative values from this enum on
 * failure, 0 on success. Source-compat with the old "return -1 on
 * any error" contract: every enum value is <=0, so existing
 * `if (rc < 0)` checks keep working — they just see a more specific
 * code when they care.
 *
 * NET_E_GENERIC (-1) is retained for sites where the failure mode
 * isn't worth enumerating (e.g. forwarded from a third-party
 * library). Prefer a specific code whenever possible.
 *
 * Naming aligned loosely with Linux errno where there's a match.
 * Freestanding C — no <errno.h> — so the SLM-OS codes are defined locally.
 */
enum net_error {
    NET_OK            = 0,
    NET_E_GENERIC     = -1,   /* catch-all, for compatibility */
    NET_E_NOT_INIT    = -2,   /* networking subsystem not initialized */
    NET_E_NO_DRIVER   = -3,   /* no net_driver registered */
    NET_E_NO_DEVICE   = -4,   /* driver could not find hardware */
    NET_E_NO_MEM      = -5,   /* pbuf / pmm / pool allocation failed */
    NET_E_BUSY        = -6,   /* operation in progress (ping pending, queue full) */
    NET_E_TIMEOUT     = -7,   /* TX completion, DHCP bind, ARP resolve */
    NET_E_INVAL       = -8,   /* bad argument (NULL, out of range) */
    NET_E_TOO_LARGE   = -9,   /* packet exceeds MTU */
    NET_E_LINK_DOWN   = -10,  /* physical link is down */
    NET_E_PROTO       = -11,  /* feature negotiation / version mismatch */
};

/**
 * Convert a net_error code to a short human-readable string.
 *
 * Returns a pointer to a static string; never NULL. Handles both
 * the enum values (e.g. -7) and raw negative values that don't
 * match any enum member (returned as "unknown").
 */
const char *net_strerror(int err);

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

/*
 * Background task entry that loops net_poll() + sleep_ms(10). Spawned
 * once at boot (from main.c) so the RX path keeps draining when no
 * foreground command is explicitly polling — otherwise SLM-OS only
 * responds to inbound traffic while the shell is inside `ping` or
 * net_init's DHCP wait.
 */
void net_pump_task_entry(void *arg);

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

/**
 * Override the DHCP bind timeout at runtime.
 *
 * Primarily for tests that want to exercise the auto-DHCP fallback
 * path without waiting the full default (10 s). Passing 0 restores
 * the compile-time default (NET_DHCP_TIMEOUT_DEFAULT_MS).
 *
 * @param ms  New timeout in milliseconds
 */
void net_set_dhcp_timeout_ms(uint32_t ms);

/**
 * Query the current DHCP bind timeout.
 *
 * @return  Timeout in milliseconds
 */
uint32_t net_get_dhcp_timeout_ms(void);

/**
 * Check whether DHCP has exceeded its bind timeout, and fall back to
 * the static IP configuration if so. net_poll() calls this once per
 * poll; tests can call it directly to deterministically trigger
 * fallback without racing the packet receive path.
 *
 * @return  1 if fallback fired, 0 if no action was taken
 */
int net_dhcp_check_timeout(void);

/**
 * Number of distinct DHCP-bound transitions since boot.
 *
 * Increments each time the netif transitions from not-bound to bound
 * (covers fresh DHCP acquisition and rebind after release/renew).
 * Used by tests (issue #201) to verify the status callback fired;
 * also useful for diagnostics.
 *
 * @return  Monotonic count of BOUND transitions
 */
uint32_t net_get_dhcp_bind_count(void);

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
    uint64_t rx_dropped;    /* Packets dropped at lwIP layer (pbuf alloc fail, input reject) */
    uint64_t rx_no_buffers; /* Packets dropped by driver: RX virtqueue post failed (no descriptor) */
};

/**
 * Get network statistics
 *
 * @param stats  Pointer to structure to fill
 */
void net_get_stats(struct net_stats *stats);

/**
 * Bump the rx_no_buffers counter from a driver.
 *
 * Called by NIC drivers when they fail to post an RX descriptor —
 * typically during re-post after recv, if the virtqueue descriptor
 * pool is exhausted under sustained burst traffic. Makes the
 * underlying silent drop visible in `netstat`.
 */
void net_stats_rx_no_buffers_inc(void);

/* -------------------------------------------------------------------------- */
/* RX-stall watchdog                                                           */
/* -------------------------------------------------------------------------- */

/**
 * Snapshot of the RX-stall watchdog state.
 *
 * Captured by `net_watchdog_get` and dumped automatically (one-shot)
 * when the watchdog detects link-up but no RX for `stall_threshold_ms`.
 * Exposes enough state to diagnose the four most common stall causes
 * without needing to reach for serial: pbuf-pool exhaustion, TCP-PCB
 * exhaustion, lwIP heap exhaustion, and "no RX coming in at all"
 * (driver / link-layer wedge).
 */
struct net_watchdog_snapshot {
    bool      armed;                /* watchdog enabled (link-up + RX seen at least once) */
    bool      alarmed;               /* currently in stalled state */
    uint32_t  ms_since_last_rx;      /* monotonic ms since last successful pbuf_alloc */
    uint32_t  stall_threshold_ms;    /* config threshold */
    uint64_t  rx_packets;            /* mirror of net_stats.rx_packets at sample time */
    uint64_t  rx_dropped;            /* lwIP-layer drops */
    uint64_t  rx_no_buffers;         /* driver-layer drops */
    uint16_t  pbuf_pool_used;        /* lwip_stats.memp[MEMP_PBUF_POOL]->used */
    uint16_t  pbuf_pool_avail;       /* configured PBUF_POOL_SIZE */
    uint16_t  tcp_pcb_used;
    uint16_t  tcp_pcb_avail;
    uint32_t  heap_used;             /* lwip_stats.mem.used */
    uint32_t  heap_avail;            /* lwip_stats.mem.avail */
    uint32_t  stall_events;          /* count of stall→alarm transitions since boot */
    uint32_t  recovery_events;       /* count of alarm→recovery transitions since boot */
};

/**
 * Sample the watchdog state.
 *
 * Cheap (a handful of static loads). Safe to call from any task or
 * from the M3 telemetry feed publisher.
 */
void net_watchdog_get(struct net_watchdog_snapshot *out);

/**
 * Override the stall threshold at runtime.
 *
 * Default is `NET_RX_STALL_THRESHOLD_MS` (10 s). Tests pass a smaller
 * value (e.g. 200 ms) to drive the alarm path without sleeping the
 * whole CI run; pass 0 to restore the default. Negative / wrap values
 * are clamped to a minimum of 100 ms so a unit test cannot
 * accidentally turn the watchdog into a busy log spammer.
 */
void net_watchdog_set_threshold_ms(uint32_t ms);

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
