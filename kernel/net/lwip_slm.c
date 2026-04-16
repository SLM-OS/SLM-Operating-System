/**
 * lwIP Integration for SLM-OS
 *
 * This file bridges the lwIP TCP/IP stack to the VirtIO-Net driver.
 * It provides the net_* API functions and implements the lwIP netif driver.
 */

#include "net.h"
#include "net_driver.h"
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

/* Auto-DHCP state (issue #197).
 *
 * Set to the sys_now() timestamp when dhcp_start() is called at boot.
 * net_get_info() uses this to report NET_DHCP_PENDING vs BOUND.
 * When the elapsed time exceeds NET_DHCP_TIMEOUT_MS without a bind,
 * net_poll() stops DHCP, restores the static IP, and flips status to
 * NET_DHCP_FAILED so the system has a working IP even if no DHCP
 * server answered.
 */
#ifndef NET_DHCP_TIMEOUT_MS
#define NET_DHCP_TIMEOUT_MS  10000
#endif
static uint32_t dhcp_start_time;
static bool     dhcp_fallback_done;
static uint32_t static_ip_fallback;
static uint32_t static_nm_fallback;
static uint32_t static_gw_fallback;

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

    /* Check if this is an ICMP echo reply */
    if (p->len >= sizeof(struct ip_hdr) + 8) {
        struct ip_hdr *ip = (struct ip_hdr *)p->payload;
        uint16_t ip_hlen = IPH_HL(ip) * 4;

        if (p->len >= ip_hlen + 8) {
            uint8_t *icmp = (uint8_t *)p->payload + ip_hlen;
            uint8_t type = icmp[0];
            uint16_t seq = (icmp[6] << 8) | icmp[7];

            if (type == 0 && seq == ping_state.seq) {
                /* This is our reply */
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
        return -1;
    }
    if (active_driver->init() < 0) {
        ERROR("Failed to initialize network driver: %s", active_driver->name);
        return -1;
    }

    /* Initialize lwIP */
    lwip_init();
    INFO("lwIP %s initialized", LWIP_VERSION_STRING);

    /* Set up default IP configuration (will be overridden by DHCP) */
    ip4_addr_t ipaddr, netmask, gateway;
    IP4_ADDR(&ipaddr, 10, 0, 2, 15);     /* QEMU user-mode default guest IP */
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, 10, 0, 2, 2);     /* QEMU user-mode gateway */

    /* Add network interface */
    if (netif_add(&slm_netif, &ipaddr, &netmask, &gateway, NULL,
                  slm_netif_init, ethernet_input) == NULL) {
        ERROR("Failed to add network interface");
        return -1;
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
    if (dhcp_start(&slm_netif) == ERR_OK) {
        dhcp_started = true;
        dhcp_start_time = sys_now();
        dhcp_fallback_done = false;
        INFO("DHCP client started at boot (timeout %u ms)", NET_DHCP_TIMEOUT_MS);
    } else {
        WARN("DHCP auto-start failed; using static IP");
    }
#endif

    return 0;
}

void net_poll(void) {
    if (!net_initialized) {
        return;
    }

    /* Check for received packets */
    int len = active_driver->recv(rx_packet_buf, sizeof(rx_packet_buf));
    if (len > 0) {
        /* Create pbuf for the received packet */
        struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
        if (p) {
            memcpy(p->payload, rx_packet_buf, len);
            net_statistics.rx_packets++;
            net_statistics.rx_bytes += len;

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

    /* DHCP auto-start timeout fallback (issue #197).
     *
     * If DHCP has been running longer than NET_DHCP_TIMEOUT_MS without
     * binding an address, stop it and restore the static IP. This
     * ensures the system always has a working IP even if the network
     * has no DHCP server. Runs once — after fallback we stay on static
     * until the user manually re-enables DHCP via `ifconfig dhcp`. */
    if (dhcp_started && !dhcp_fallback_done &&
        !dhcp_supplied_address(&slm_netif)) {
        uint32_t elapsed = sys_now() - dhcp_start_time;
        if (elapsed > NET_DHCP_TIMEOUT_MS) {
            WARN("DHCP timeout after %u ms; falling back to static IP",
                 elapsed);
            dhcp_stop(&slm_netif);
            dhcp_started = false;
            dhcp_fallback_done = true;
            ip4_addr_t ip, nm, gw;
            ip.addr = static_ip_fallback;
            nm.addr = static_nm_fallback;
            gw.addr = static_gw_fallback;
            netif_set_addr(&slm_netif, &ip, &nm, &gw);
        }
    }

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
}

int net_get_info(struct net_info *info) {
    if (!net_initialized || !info) {
        return -1;
    }

    active_driver->get_mac(info->mac);
    info->ip_addr = slm_netif.ip_addr.addr;
    info->netmask = slm_netif.netmask.addr;
    info->gateway = slm_netif.gw.addr;
    info->link_up = (slm_netif.flags & NETIF_FLAG_LINK_UP) != 0;
    info->dhcp_enabled = dhcp_started;

    /* Derive detailed DHCP status from lwIP's view of the netif */
    if (dhcp_started) {
        info->dhcp_status = dhcp_supplied_address(&slm_netif)
            ? NET_DHCP_BOUND
            : NET_DHCP_PENDING;
    } else if (dhcp_fallback_done) {
        info->dhcp_status = NET_DHCP_FAILED;
    } else {
        info->dhcp_status = NET_DHCP_DISABLED;
    }

    return 0;
}

int net_set_static_ip(uint32_t ip_addr, uint32_t netmask, uint32_t gateway) {
    if (!net_initialized) {
        return -1;
    }

    /* Stop DHCP if running */
    if (dhcp_started) {
        dhcp_stop(&slm_netif);
        dhcp_started = false;
    }

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
        return -1;
    }

    if (!dhcp_started) {
        if (dhcp_start(&slm_netif) == ERR_OK) {
            dhcp_started = true;
            dhcp_start_time = sys_now();
            dhcp_fallback_done = false;
            INFO("DHCP client started");
        } else {
            ERROR("Failed to start DHCP client");
            return -1;
        }
    }

    return 0;
}

int net_ping(uint32_t addr, uint16_t seq, ping_callback_t callback, void *user) {
    if (!net_initialized) {
        return -1;
    }

    if (ping_state.pending) {
        return -1;  /* Previous ping still pending */
    }

    /* Create ICMP echo request */
    struct pbuf *p = pbuf_alloc(PBUF_IP, 8 + 32, PBUF_RAM);
    if (!p) {
        return -1;
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
        return -1;
    }

    return 0;
}

void net_get_stats(struct net_stats *stats) {
    if (stats) {
        *stats = net_statistics;
    }
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

int net_str_to_ip(const char *str, uint32_t *addr) {
    if (!str || !addr) {
        return -1;
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
                return -1;
            }
        } else if (*p == '.' || *p == '\0') {
            if (digits == 0 || octet >= 4) {
                return -1;
            }
            octets[octet++] = value;
            value = 0;
            digits = 0;
            if (*p == '\0') break;
        } else {
            return -1;
        }
    }

    if (octet != 4) {
        return -1;
    }

    *addr = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return 0;
}

bool net_is_up(void) {
    return net_initialized && (slm_netif.flags & NETIF_FLAG_LINK_UP);
}
