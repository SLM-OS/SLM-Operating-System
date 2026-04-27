/**
 * lwIP Integration for SLM-OS
 *
 * This file bridges the lwIP TCP/IP stack to the VirtIO-Net driver.
 * It provides the net_* API functions and implements the lwIP netif driver.
 */

#include "net.h"
#include "net_driver.h"
#include "usb.h"
#include "cdc_ecm.h"
#include "debug.h"
#include "timer.h"

/* lwIP includes */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/icmp.h"
#include "lwip/ip4.h"
#include "lwip/raw.h"
#include "lwip/stats.h"
#include "lwip/memp.h"

#include "shell_io_tcp.h"
#include "netif/ethernet.h"
#include "arch/sys_arch.h"

#include <string.h>

/* -------------------------------------------------------------------------- */
/* Driver Registration                                                         */
/* -------------------------------------------------------------------------- */

static const struct net_driver *active_driver;

void net_register_driver(const struct net_driver *drv) {
    active_driver = drv;
}

const struct net_driver *net_get_driver(void) {
    return active_driver;
}

/* -------------------------------------------------------------------------- */
/* Network Interface State                                                     */
/* -------------------------------------------------------------------------- */

static struct netif slm_netif;
static bool net_initialized = false;
static bool dhcp_started = false;
static bool dhcp_requested = false;
static bool last_was_bound = false;

/* Auto-DHCP state (issue #197).
 *
 * `dhcp_start_time` is only meaningful while `dhcp_timeout_armed`
 * is true. Boot-time auto-DHCP can be deferred until link-ready;
 * that path intentionally leaves the timeout disarmed until the DHCP
 * client actually starts so slow USB bring-up does not consume the
 * entire fallback budget before the first DISCOVER is sent.
 */
#ifndef NET_DHCP_TIMEOUT_DEFAULT_MS
#define NET_DHCP_TIMEOUT_DEFAULT_MS  10000
#endif
/* Runtime-adjustable so tests can force the fallback path in seconds
 * rather than waiting the full default. Production callers don't need
 * to touch this. */
static uint32_t dhcp_timeout_ms = NET_DHCP_TIMEOUT_DEFAULT_MS;
static uint32_t dhcp_start_time;
static bool     dhcp_timeout_armed;
static bool     dhcp_fallback_done;
static bool     dhcp_test_force_start_fail;
static uint32_t static_ip_fallback;
static uint32_t static_nm_fallback;
static uint32_t static_gw_fallback;

static bool net_link_is_up(void) {
    return (slm_netif.flags & NETIF_FLAG_LINK_UP) != 0;
}

static int net_start_dhcp_client(const char *reason) {
    if (dhcp_started)
        return NET_OK;

    if (dhcp_test_force_start_fail) {
        dhcp_test_force_start_fail = false;
        dhcp_requested = false;
        dhcp_started = false;
        dhcp_timeout_armed = false;
        dhcp_fallback_done = false;
        ERROR("Failed to start DHCP client");
        return NET_E_NO_MEM;
    }

    if (dhcp_start(&slm_netif) != ERR_OK) {
        dhcp_requested = false;
        dhcp_started = false;
        dhcp_timeout_armed = false;
        dhcp_fallback_done = false;
        ERROR("Failed to start DHCP client");
        return NET_E_NO_MEM;
    }

    dhcp_requested = true;
    dhcp_started = true;
    dhcp_start_time = sys_now();
    dhcp_timeout_armed = true;
    dhcp_fallback_done = false;
    last_was_bound = false;  /* #201: announce on next BOUND */
    if (reason != NULL) {
        INFO("DHCP client started (%s)", reason);
    } else {
        INFO("DHCP client started");
    }
    return NET_OK;
}

static void net_sync_link_state(void) {
    if (!net_initialized || active_driver == NULL || active_driver->link_status == NULL)
        return;

    bool driver_link_up = active_driver->link_status();
    bool lwip_link_up = net_link_is_up();
    if (driver_link_up == lwip_link_up)
        return;

    if (driver_link_up) {
        netif_set_link_up(&slm_netif);
        INFO("Network link up");
        if (dhcp_requested && !dhcp_started) {
            (void)net_start_dhcp_client("after link-ready");
        }
    } else {
        if (dhcp_requested && dhcp_started &&
            !dhcp_supplied_address(&slm_netif)) {
            /*
             * Mid-discovery link drop: stop lwIP's DHCP state machine
             * and pause the fallback timer entirely while carrier is
             * down. When the link comes back, net_start_dhcp_client()
             * restarts DHCP with a fresh deadline instead of letting
             * the old timeout expire during the outage.
             */
            dhcp_stop(&slm_netif);
            dhcp_started = false;
            dhcp_start_time = 0;
            dhcp_timeout_armed = false;
            INFO("DHCP paused waiting for link restore");
        }
        netif_set_link_down(&slm_netif);
        INFO("Network link down");
    }
}

void net_set_dhcp_timeout_ms(uint32_t ms) {
    /* Accept any value including 0 — tests use 0 to trigger fallback
     * immediately on the next check. Production callers should use
     * a reasonable value; 0 disables the wait entirely. */
    dhcp_timeout_ms = ms;
}

uint32_t net_get_dhcp_timeout_ms(void) {
    return dhcp_timeout_ms;
}

void net_test_force_boot_deferred_dhcp(void) {
    dhcp_requested = true;
    dhcp_started = false;
    dhcp_timeout_armed = false;
    dhcp_fallback_done = false;
    dhcp_start_time = 0;
}

void net_test_force_dhcp_start_fail(void) {
    dhcp_test_force_start_fail = true;
}

/*
 * Check whether DHCP has exceeded its bind timeout and fall back to
 * the static IP if so. Called from net_poll() once per poll; also
 * callable directly from tests that want to deterministically
 * trigger fallback without racing the recv path. Returns 1 if the
 * fallback fired, 0 if no action was taken.
 */
int net_dhcp_check_timeout(void) {
    if (!dhcp_requested || dhcp_fallback_done || !dhcp_timeout_armed)
        return 0;
    if (dhcp_supplied_address(&slm_netif))
        return 0;

    uint32_t elapsed = sys_now() - dhcp_start_time;
    if (elapsed < dhcp_timeout_ms)
        return 0;

    WARN("DHCP timeout after %u ms; falling back to static IP", elapsed);
    if (dhcp_started)
        dhcp_stop(&slm_netif);
    dhcp_requested = false;
    dhcp_started = false;
    dhcp_timeout_armed = false;
    dhcp_fallback_done = true;
    ip4_addr_t ip, nm, gw;
    ip.addr = static_ip_fallback;
    nm.addr = static_nm_fallback;
    gw.addr = static_gw_fallback;
    netif_set_addr(&slm_netif, &ip, &nm, &gw);
    return 1;
}

/* Receive buffer for packet processing */
static uint8_t rx_packet_buf[1518];

/* Ping state */
static struct {
    ping_callback_t callback;
    void *user;
    uint16_t seq;
    uint32_t addr;
    uint32_t send_time;
    bool pending;
} ping_state;

/* Statistics */
static struct net_stats net_statistics;

/* -------------------------------------------------------------------------- */
/* RX-stall watchdog                                                           */
/* -------------------------------------------------------------------------- */

/* Default stall threshold. 10 s with link up and zero RX is well outside
 * any healthy network — even an idle subnet sees ARP traffic, gateway
 * keep-alives, and DHCP renewals. Anything past this is one of the four
 * known failure modes the snapshot is designed to surface (pbuf pool
 * out, TCP PCB out, heap out, driver wedge). */
#ifndef NET_RX_STALL_THRESHOLD_MS
#define NET_RX_STALL_THRESHOLD_MS  10000u
#endif

/* Minimum override threshold. Prevents a misconfigured test from
 * setting threshold=0 and turning the watchdog into a log spammer. */
#define NET_RX_STALL_THRESHOLD_MIN_MS  100u

static uint32_t  rx_watchdog_threshold_ms  = NET_RX_STALL_THRESHOLD_MS;
static uint32_t  rx_watchdog_last_rx_ms    = 0;
static bool      rx_watchdog_seen_first_rx = false;
static bool      rx_watchdog_alarmed       = false;
static uint32_t  rx_watchdog_stall_events  = 0;
static uint32_t  rx_watchdog_recovery_events = 0;

static void net_watchdog_note_rx(void) {
    rx_watchdog_last_rx_ms = sys_now();
    rx_watchdog_seen_first_rx = true;
    if (rx_watchdog_alarmed) {
        rx_watchdog_alarmed = false;
        rx_watchdog_recovery_events++;
        /* One INFO line per stall→recovery edge. Bounded under the
         * tested workload (10 cycles on the hardware verification
         * pass produced 10 lines, paired with their stall WARNs).
         * If a future failure mode causes flapping at hundreds of
         * Hz, this could fight for UART bandwidth — gate behind a
         * rate-limit then. Today the spam ceiling is "one line per
         * threshold-ms while the network is marginal", which is
         * the right diagnostic granularity. */
        INFO("net: RX recovered after stall (rx_packets=%llu)",
             (unsigned long long)net_statistics.rx_packets);
    }
}

void net_watchdog_set_threshold_ms(uint32_t ms) {
    if (ms == 0) {
        rx_watchdog_threshold_ms = NET_RX_STALL_THRESHOLD_MS;
        return;
    }
    if (ms < NET_RX_STALL_THRESHOLD_MIN_MS) {
        ms = NET_RX_STALL_THRESHOLD_MIN_MS;
    }
    rx_watchdog_threshold_ms = ms;
}

/*
 * Diagnostic snapshot. Each individual field is read with a
 * single-copy-atomic load (naturally aligned uint32 / uint64 on
 * AArch64 and x86-64), but the snapshot as a *whole* is not
 * transactional — a caller on a different CPU can observe an
 * intermediate writer state (e.g. `alarmed` already cleared but
 * `recovery_events` not yet incremented). That's intentional:
 * nothing branches on this snapshot, it's only displayed by
 * `netstat` and published by the M3 telemetry feed. If a future
 * caller needs a transactional view, wrap the body in
 * SYS_ARCH_PROTECT to serialise against the writer in net_poll.
 */
void net_watchdog_get(struct net_watchdog_snapshot *out) {
    if (!out) return;

    uint32_t now = sys_now();
    uint32_t since = rx_watchdog_seen_first_rx
                     ? (uint32_t)(now - rx_watchdog_last_rx_ms)
                     : 0u;

    out->armed                = rx_watchdog_seen_first_rx && net_link_is_up();
    out->alarmed              = rx_watchdog_alarmed;
    out->ms_since_last_rx     = since;
    out->stall_threshold_ms   = rx_watchdog_threshold_ms;
    out->rx_packets           = net_statistics.rx_packets;
    out->rx_dropped           = net_statistics.rx_dropped;
    out->rx_no_buffers        = net_statistics.rx_no_buffers;
    out->stall_events         = rx_watchdog_stall_events;
    out->recovery_events      = rx_watchdog_recovery_events;

    /* `lwip_stats.memp[i]` slots are NULL until `memp_init` runs as
     * part of `lwip_init`. The watchdog snapshot must work on a fresh
     * boot too — a unit test that calls net_watchdog_get before
     * net_init must not data-abort. Treat NULL as "0/0". */
#if MEMP_STATS
    if (lwip_stats.memp[MEMP_PBUF_POOL] != NULL) {
        out->pbuf_pool_used  = (uint16_t)lwip_stats.memp[MEMP_PBUF_POOL]->used;
        out->pbuf_pool_avail = (uint16_t)lwip_stats.memp[MEMP_PBUF_POOL]->avail;
    } else {
        out->pbuf_pool_used  = 0;
        out->pbuf_pool_avail = 0;
    }
    if (lwip_stats.memp[MEMP_TCP_PCB] != NULL) {
        out->tcp_pcb_used  = (uint16_t)lwip_stats.memp[MEMP_TCP_PCB]->used;
        out->tcp_pcb_avail = (uint16_t)lwip_stats.memp[MEMP_TCP_PCB]->avail;
    } else {
        out->tcp_pcb_used  = 0;
        out->tcp_pcb_avail = 0;
    }
#else
    out->pbuf_pool_used  = 0;
    out->pbuf_pool_avail = 0;
    out->tcp_pcb_used    = 0;
    out->tcp_pcb_avail   = 0;
#endif
#if MEM_STATS
    out->heap_used      = (uint32_t)lwip_stats.mem.used;
    out->heap_used_peak = (uint32_t)lwip_stats.mem.max;
    out->heap_avail     = (uint32_t)lwip_stats.mem.avail;
#else
    out->heap_used      = 0;
    out->heap_used_peak = 0;
    out->heap_avail     = 0;
#endif
}

/* Called from net_poll(). Cheap when not alarmed. */
static void net_watchdog_check(void) {
    /* Don't fire before we've ever seen RX (boot-time link bring-up
     * has its own DHCP timeout machinery). Don't fire while link is
     * down — no RX is expected. */
    if (!rx_watchdog_seen_first_rx) return;
    if (!net_link_is_up()) return;
    if (rx_watchdog_alarmed) return;

    uint32_t since = (uint32_t)(sys_now() - rx_watchdog_last_rx_ms);
    if (since < rx_watchdog_threshold_ms) return;

    rx_watchdog_alarmed = true;
    rx_watchdog_stall_events++;

    struct net_watchdog_snapshot snap;
    net_watchdog_get(&snap);

    /* One-shot dump — `net_watchdog_note_rx` clears `alarmed` and
     * logs the recovery line if traffic resumes. */
    WARN("net: RX stalled (link up, %u ms idle, threshold %u ms)",
         (unsigned)snap.ms_since_last_rx,
         (unsigned)snap.stall_threshold_ms);
    WARN("net:   rx_packets=%llu dropped=%llu no_buffers=%llu",
         (unsigned long long)snap.rx_packets,
         (unsigned long long)snap.rx_dropped,
         (unsigned long long)snap.rx_no_buffers);
    WARN("net:   pbuf_pool=%u/%u tcp_pcb=%u/%u heap=%u/%u",
         (unsigned)snap.pbuf_pool_used, (unsigned)snap.pbuf_pool_avail,
         (unsigned)snap.tcp_pcb_used,   (unsigned)snap.tcp_pcb_avail,
         (unsigned)snap.heap_used,      (unsigned)snap.heap_avail);
}

/* -------------------------------------------------------------------------- */
/* Network Interface (netif) Driver                                            */
/* -------------------------------------------------------------------------- */

/**
 * Send a packet via the network interface
 */
static err_t slm_netif_output(struct netif *netif, struct pbuf *p) {
    (void)netif;

    if (!p) {
        return ERR_ARG;
    }

    /* For simple case, assume single pbuf (or copy to contiguous buffer) */
    uint8_t tx_buf[1518];
    uint16_t len = 0;

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if (len + q->len > sizeof(tx_buf)) {
            net_statistics.tx_errors++;
            return ERR_BUF;
        }
        memcpy(tx_buf + len, q->payload, q->len);
        len += q->len;
    }

    int ret = active_driver->send(tx_buf, len);
    if (ret < 0) {
        net_statistics.tx_errors++;
        return ERR_IF;
    }

    net_statistics.tx_packets++;
    net_statistics.tx_bytes += len;
    return ERR_OK;
}

/*
 * DHCP bind announcer (issue #201).
 *
 * Prints a one-line INFO whenever dhcp_supplied_address() transitions
 * false → true, so the user sees the DHCP-acquired IP without having
 * to run ifconfig. Originally tried via netif_set_status_callback,
 * but lwIP's netif_do_set_ipaddr skips the callback when the new IP
 * equals the existing one — QEMU SLIRP typically hands out 10.0.2.15
 * which matches the configured static default, so the callback never fired on
 * bind under QEMU. Polling from net_poll() is race-free regardless
 * of whether the IP actually changed.
 */
/* Mutated only from net_poll() context (single-caller today). If a
 * future multi-CPU RX dispatch adds a second poll caller, the
 * false→true edge detection becomes racy — either serialise the
 * callers or convert to _Atomic + CAS. */
static uint32_t dhcp_bind_count = 0;

static void net_check_dhcp_bind_transition(void) {
    bool bound_now = dhcp_supplied_address(&slm_netif) != 0;
    if (bound_now && !last_was_bound) {
        char ip[16], gw[16], nm[16];
        net_ip_to_str(slm_netif.ip_addr.addr, ip);
        net_ip_to_str(slm_netif.gw.addr,      gw);
        net_ip_to_str(slm_netif.netmask.addr, nm);
        INFO("DHCP bound: IP=%s GW=%s Mask=%s", ip, gw, nm);
        dhcp_bind_count++;
    }
    last_was_bound = bound_now;
}

uint32_t net_get_dhcp_bind_count(void) {
    return dhcp_bind_count;
}

/**
 * Initialize the network interface
 */
static err_t slm_netif_init(struct netif *netif) {
    /* Get MAC address from network driver */
    active_driver->get_mac(netif->hwaddr);
    netif->hwaddr_len = 6;

    /* Set interface name */
    netif->name[0] = 's';
    netif->name[1] = 'l';

    /* Set MTU */
    netif->mtu = 1500;

    /* Set flags */
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                   NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;

    /* Link is up if driver reports it */
    if (active_driver->link_status()) {
        netif->flags |= NETIF_FLAG_LINK_UP;
    }

    /* Set output function */
    netif->output = etharp_output;
    netif->linkoutput = slm_netif_output;

    INFO("Network interface initialized: sl0");
    INFO("  MAC: %02x:%02x:%02x:%02x:%02x:%02x",
         netif->hwaddr[0], netif->hwaddr[1], netif->hwaddr[2],
         netif->hwaddr[3], netif->hwaddr[4], netif->hwaddr[5]);

    return ERR_OK;
}

/* -------------------------------------------------------------------------- */
/* Ping Implementation                                                         */
/* -------------------------------------------------------------------------- */

/* Raw PCB for receiving ICMP replies */
static struct raw_pcb *ping_pcb = NULL;

/**
 * ICMP echo reply callback
 */
static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                      const ip_addr_t *addr) {
    (void)arg;
    (void)pcb;
    LWIP_UNUSED_ARG(addr);

    if (!ping_state.pending) {
        return 0;  /* Not expecting a reply */
    }

    /* raw_recv() delivers the IPv4 header in front of the ICMP payload.
     * Remove it before examining the echo-reply header, then restore it
     * if the packet is not ours so the rest of lwIP sees the original pbuf.
     */
    if (p->tot_len >= sizeof(struct ip_hdr) + 8) {
        struct ip_hdr *ip = (struct ip_hdr *)p->payload;
        uint16_t ip_hlen = (uint16_t)(IPH_HL(ip) * 4);

        if (p->tot_len >= ip_hlen + 8 && pbuf_remove_header(p, ip_hlen) == 0) {
            uint8_t *icmp = (uint8_t *)p->payload;
            uint8_t type = icmp[0];
            uint16_t seq = (uint16_t)((uint16_t)icmp[6] << 8) | icmp[7];

            if (type == 0 && seq == ping_state.seq) {
                uint32_t now = sys_now();
                uint32_t rtt = now - ping_state.send_time;

                if (ping_state.callback) {
                    ping_state.callback(seq, ping_state.addr, rtt, true,
                                        ping_state.user);
                }

                ping_state.pending = false;
                pbuf_free(p);
                return 1;  /* Packet consumed */
            }

            pbuf_add_header(p, ip_hlen);
        }
    }

    return 0;  /* Not our packet */
}

/* -------------------------------------------------------------------------- */
/* Public API Implementation                                                   */
/* -------------------------------------------------------------------------- */

int net_init(void) {
    if (net_initialized) {
        return 0;
    }

    INFO("Initializing network subsystem...");

    /* Initialize the registered network driver */
    if (!active_driver) {
        ERROR("No network driver registered");
        return NET_E_NO_DRIVER;
    }
    /* The driver's init() can fail for several reasons — device not
     * present, feature negotiation rejected, MMIO map fault, OOM on
     * virtqueue ring allocation. The current net_driver contract
     * collapses all of these into `-1`, so the specific cause can't
     * be distinguished here. Return NET_E_GENERIC rather than
     * guessing NO_DEVICE; if the driver contract is ever extended to
     * propagate specific codes, this site should thread them through. */
    if (active_driver->init() < 0) {
        ERROR("Failed to initialize network driver: %s", active_driver->name);
        return NET_E_GENERIC;
    }

    /* Initialize lwIP */
    lwip_init();
    INFO("lwIP %s initialized", LWIP_VERSION_STRING);

    /* Set up default IP configuration (will be overridden by DHCP) */
    ip4_addr_t ipaddr, netmask, gateway;
    IP4_ADDR(&ipaddr, 10, 0, 2, 15);     /* QEMU user-mode default guest IP */
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, 10, 0, 2, 2);     /* QEMU user-mode gateway */

    /* Add network interface. netif_add() returns NULL on OOM in
     * the netif pool, but also on init-callback failure or internal
     * lwIP state errors — same broad-failure-to-specific-code
     * mismatch as the driver init above. */
    if (netif_add(&slm_netif, &ipaddr, &netmask, &gateway, NULL,
                  slm_netif_init, ethernet_input) == NULL) {
        ERROR("Failed to add network interface");
        return NET_E_GENERIC;
    }

    /* Set as default interface */
    netif_set_default(&slm_netif);
    netif_set_up(&slm_netif);

    /* Create raw PCB for ping replies */
    ping_pcb = raw_new(IP_PROTO_ICMP);
    if (ping_pcb) {
        raw_recv(ping_pcb, ping_recv, NULL);
        raw_bind(ping_pcb, IP_ADDR_ANY);
    }

    /* Remember static fallback IP for DHCP timeout recovery */
    static_ip_fallback = ipaddr.addr;
    static_nm_fallback = netmask.addr;
    static_gw_fallback = gateway.addr;

    net_initialized = true;

    /* Log initial configuration */
    char ip_str[16], gw_str[16], nm_str[16];
    net_ip_to_str(ipaddr.addr, ip_str);
    net_ip_to_str(gateway.addr, gw_str);
    net_ip_to_str(netmask.addr, nm_str);
    INFO("Network configured: IP=%s GW=%s Mask=%s", ip_str, gw_str, nm_str);

    /* Auto-start DHCP at boot (issue #197).
     *
     * Gated on NET_DHCP_AT_BOOT (CMake option, default ON). Non-blocking:
     * lwIP runs the DHCP DISCOVER/OFFER/REQUEST/ACK handshake in the
     * background as long as net_poll() is called regularly. If no DHCP
     * server answers within NET_DHCP_TIMEOUT_MS, net_poll() falls back
     * to the static IP configured above. */
#if defined(NET_DHCP_AT_BOOT)
    dhcp_requested = true;
    dhcp_timeout_armed = false;
    dhcp_fallback_done = false;
    if (net_link_is_up()) {
        if (net_start_dhcp_client("at boot") == NET_OK) {
            INFO("DHCP auto-start armed (timeout %u ms)", dhcp_timeout_ms);
        }
    } else {
        INFO("DHCP auto-start deferred until link-ready (timeout %u ms)",
             dhcp_timeout_ms);
    }
#endif

    return 0;
}

void net_poll(void) {
    /*
     * Drive the USB core's hot-plug retry path even before net_init
     * has run — usb_core_hotplug_poll() is a no-op when no HCD is
     * registered, and returns immediately once a device has been
     * enumerated. On Jetson this is what notices a post-kexec
     * re-plug and kicks off the Phase 3A enumeration sequence (#309).
     *
     * Once the USB device has enumerated, retry the CDC-ECM probe so
     * the class driver can bind. Idempotent — subsequent ticks return
     * 0 immediately once bound. This is the Phase 4 hookup the
     * cdc_ecm.h header note describes.
     *
     * Both calls run unconditionally on every platform (not gated by
     * #ifdef). On QEMU / Pi 5 / x86-64 there is never a USB device to
     * enumerate, so each call is an atomic-load early-return — one
     * function call and one cache-hot load per ~100 Hz tick, well
     * below anything that would show up on a profile. Keeping the
     * call site platform-agnostic avoids duplicating the CMake
     * platform gate in C and mirrors the "USB core compiles on every
     * platform" design in docs/networking.md.
     */
    (void)usb_core_hotplug_poll();
    (void)cdc_ecm_probe_and_register();

    if (!net_initialized) {
        return;
    }
    /* Defensive: net_init guarantees active_driver was non-NULL at
     * the time it ran, but nothing prevents later code from clearing
     * the slot (tests, future hot-unplug paths). Bail rather than
     * NULL-deref the driver ops below. */
    if (active_driver == NULL) {
        return;
    }

    /* Drain TX completions first (#204). The driver's send() submits
     * asynchronously and returns; the actual completion arrives via
     * the device writing to the TX used ring. tx_reap walks that ring
     * and frees the pool buffers so subsequent sends can reuse them.
     * Optional op — drivers that complete TX synchronously inside
     * send() may leave it NULL. */
    if (active_driver->tx_reap)
        active_driver->tx_reap();

    net_sync_link_state();

    /* Check for received packets */
    int len = active_driver->recv(rx_packet_buf, sizeof(rx_packet_buf));
    if (len > 0) {
        /* Create pbuf for the received packet */
        struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
        if (p) {
            memcpy(p->payload, rx_packet_buf, len);
            net_statistics.rx_packets++;
            net_statistics.rx_bytes += len;

            /* Watchdog liveness ping. Note here (after pbuf_alloc
             * succeeded) rather than after slm_netif.input() because
             * "received a frame at the netif boundary" is the right
             * granularity — even a frame the IP stack drops indicates
             * the driver/USB/lwIP-pool path is alive. */
            net_watchdog_note_rx();

            /* Pass to lwIP */
            if (slm_netif.input(p, &slm_netif) != ERR_OK) {
                pbuf_free(p);
                net_statistics.rx_dropped++;
            }
        } else {
            net_statistics.rx_dropped++;
        }
    }

    /* Process lwIP timers */
    sys_check_timeouts();

    /* DHCP auto-start timeout fallback (issue #197) */
    net_dhcp_check_timeout();

    /* Announce DHCP binds (issue #201). Polled here rather than via
     * netif_set_status_callback because lwIP suppresses the callback
     * when the bound IP equals the prior static IP — common under
     * QEMU SLIRP. */
    net_check_dhcp_bind_transition();

    /* Check ping timeout (1 second) */
    if (ping_state.pending) {
        uint32_t now = sys_now();
        if (now - ping_state.send_time > 1000) {
            if (ping_state.callback) {
                ping_state.callback(ping_state.seq, ping_state.addr, 0, false,
                                   ping_state.user);
            }
            ping_state.pending = false;
        }
    }

    /* Drain TX + complete teardown for any TCP shell sessions. */
    shell_io_tcp_poll();

    /* RX-stall watchdog. Cheap when not alarmed (a load + compare). */
    net_watchdog_check();
}

/*
 * Background task body that drives net_poll() at ~100 Hz so RX and
 * lwIP timers keep running when the shell is idle. Without this, the
 * only RX drain was inside the `ping` command's wait loops and
 * net_init's DHCP wait — meaning SLM-OS wouldn't respond to an
 * inbound ping while sitting at the prompt. Spawned at
 * TASK_PRIORITY_IDLE (see kernel/src/main.c) so shell, tests, and
 * workloads preempt it trivially; sleep_ms(10) yields cooperatively
 * between polls.
 *
 * Entry function lives here (next to net_poll) rather than in
 * main.c so platform init only needs to task_create it, not know
 * the body. Safe to call before net_init runs — net_poll returns
 * early when net_initialized is false, so this task spins at
 * near-zero cost until the shell brings up networking.
 */
void net_pump_task_entry(void *arg)
{
    (void)arg;
    for (;;) {
        net_poll();
        sleep_ms(10);
    }
}

int net_get_info(struct net_info *info) {
    if (!info) {
        return NET_E_INVAL;
    }
    if (!net_initialized) {
        return NET_E_NOT_INIT;
    }

    active_driver->get_mac(info->mac);
    info->ip_addr = slm_netif.ip_addr.addr;
    info->netmask = slm_netif.netmask.addr;
    info->gateway = slm_netif.gw.addr;
    info->link_up = (slm_netif.flags & NETIF_FLAG_LINK_UP) != 0;
    info->dhcp_enabled = dhcp_requested;

    /* Derive detailed DHCP status from lwIP's view of the netif */
    if (dhcp_fallback_done) {
        info->dhcp_status = NET_DHCP_FAILED;
    } else if (dhcp_requested) {
        info->dhcp_status = dhcp_supplied_address(&slm_netif)
            ? NET_DHCP_BOUND
            : NET_DHCP_PENDING;
    } else {
        info->dhcp_status = NET_DHCP_DISABLED;
    }

    return 0;
}

int net_set_static_ip(uint32_t ip_addr, uint32_t netmask, uint32_t gateway) {
    if (!net_initialized) {
        return NET_E_NOT_INIT;
    }

    /* Stop DHCP if running */
    if (dhcp_started) {
        dhcp_stop(&slm_netif);
        dhcp_started = false;
    }
    dhcp_requested = false;
    dhcp_timeout_armed = false;
    dhcp_fallback_done = false;

    /* Set static IP */
    ip4_addr_t ip, nm, gw;
    ip.addr = ip_addr;
    nm.addr = netmask;
    gw.addr = gateway;

    netif_set_addr(&slm_netif, &ip, &nm, &gw);

    char ip_str[16];
    net_ip_to_str(ip_addr, ip_str);
    INFO("Static IP set: %s", ip_str);

    return 0;
}

int net_enable_dhcp(void) {
    if (!net_initialized) {
        return NET_E_NOT_INIT;
    }

    dhcp_requested = true;
    dhcp_fallback_done = false;

    if (dhcp_started) {
        return NET_OK;
    }

    dhcp_timeout_armed = false;
    if (net_link_is_up()) {
        return net_start_dhcp_client("manual request");
    }

    INFO("DHCP request deferred until link-ready");
    return NET_OK;
}

int net_ping(uint32_t addr, uint16_t seq, ping_callback_t callback, void *user) {
    if (!net_initialized) {
        return NET_E_NOT_INIT;
    }

    if (ping_state.pending) {
        return NET_E_BUSY;  /* Previous ping still pending */
    }

    /* Create ICMP echo request */
    struct pbuf *p = pbuf_alloc(PBUF_IP, 8 + 32, PBUF_RAM);
    if (!p) {
        return NET_E_NO_MEM;
    }

    /* Fill in ICMP header */
    uint8_t *icmp = (uint8_t *)p->payload;
    icmp[0] = 8;        /* Type: Echo Request */
    icmp[1] = 0;        /* Code: 0 */
    icmp[2] = 0;        /* Checksum (calculated below) */
    icmp[3] = 0;
    icmp[4] = 0;        /* Identifier high */
    icmp[5] = 1;        /* Identifier low */
    icmp[6] = seq >> 8; /* Sequence high */
    icmp[7] = seq & 0xFF; /* Sequence low */

    /* Payload (timestamp or pattern) */
    for (int i = 0; i < 32; i++) {
        icmp[8 + i] = i;
    }

    /* Calculate checksum */
    uint32_t sum = 0;
    for (int i = 0; i < 40; i += 2) {
        sum += (icmp[i] << 8) | icmp[i + 1];
    }
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    uint16_t checksum = ~sum;
    icmp[2] = checksum >> 8;
    icmp[3] = checksum & 0xFF;

    /* Save state for reply */
    ping_state.callback = callback;
    ping_state.user = user;
    ping_state.seq = seq;
    ping_state.addr = addr;
    ping_state.send_time = sys_now();
    ping_state.pending = true;

    /* Send via raw IP */
    ip4_addr_t dest;
    dest.addr = addr;

    err_t err = raw_sendto(ping_pcb, p, &dest);
    pbuf_free(p);

    if (err != ERR_OK) {
        ping_state.pending = false;
        return NET_E_GENERIC;
    }

    return 0;
}

void net_get_stats(struct net_stats *stats) {
    if (stats) {
        *stats = net_statistics;
    }
}

void net_stats_rx_no_buffers_inc(void) {
    net_statistics.rx_no_buffers++;
}

char *net_ip_to_str(uint32_t addr, char *buf) {
    uint8_t *b = (uint8_t *)&addr;
    /* Simple integer to string conversion */
    char *p = buf;
    for (int i = 0; i < 4; i++) {
        uint8_t n = b[i];
        if (n >= 100) {
            *p++ = '0' + (n / 100);
            n %= 100;
            *p++ = '0' + (n / 10);
            n %= 10;
        } else if (n >= 10) {
            *p++ = '0' + (n / 10);
            n %= 10;
        }
        *p++ = '0' + n;
        if (i < 3) *p++ = '.';
    }
    *p = '\0';
    return buf;
}

const char *net_strerror(int err) {
    switch (err) {
        case NET_OK:            return "ok";
        case NET_E_GENERIC:     return "unspecified error";
        case NET_E_NOT_INIT:    return "network not initialized";
        case NET_E_NO_DRIVER:   return "no driver registered";
        case NET_E_NO_DEVICE:   return "device not found";
        case NET_E_NO_MEM:      return "out of memory";
        case NET_E_BUSY:        return "busy";
        case NET_E_TIMEOUT:     return "timeout";
        case NET_E_INVAL:       return "invalid argument";
        case NET_E_TOO_LARGE:   return "packet too large";
        case NET_E_LINK_DOWN:   return "link down";
        case NET_E_PROTO:       return "protocol error";
        default:                return "unknown";
    }
}

int net_str_to_ip(const char *str, uint32_t *addr) {
    if (!str || !addr) {
        return NET_E_INVAL;
    }

    uint8_t octets[4];
    int octet = 0;
    int value = 0;
    int digits = 0;

    for (const char *p = str; ; p++) {
        if (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            digits++;
            if (value > 255 || digits > 3) {
                return NET_E_INVAL;
            }
        } else if (*p == '.' || *p == '\0') {
            if (digits == 0 || octet >= 4) {
                return NET_E_INVAL;
            }
            octets[octet++] = value;
            value = 0;
            digits = 0;
            if (*p == '\0') break;
        } else {
            return NET_E_INVAL;
        }
    }

    if (octet != 4) {
        return NET_E_INVAL;
    }

    *addr = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return 0;
}

bool net_is_up(void) {
    return net_initialized && (slm_netif.flags & NETIF_FLAG_LINK_UP);
}
