/**
 * lwIP Options for SLM-OS
 *
 * This file configures which lwIP features are enabled and sets
 * buffer sizes and pool counts appropriate for our embedded use case.
 *
 * Key decisions:
 * - NO_SYS=1: Single-threaded mode (polled, no OS threading)
 * - Static memory pools (no malloc)
 * - ICMP enabled for ping
 * - DHCP enabled for automatic configuration
 * - TCP/UDP enabled
 * - DNS + altcp enabled for HTTP client support
 */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* -------------------------------------------------------------------------- */
/* Platform / Architecture                                                     */
/* -------------------------------------------------------------------------- */

/* Single-threaded mode - no OS threading abstraction needed */
#define NO_SYS                      1

/* We handle timeouts ourselves in the main loop */
#define NO_SYS_NO_TIMERS            0

/* Disable threading when NO_SYS=1 */
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0

/* -------------------------------------------------------------------------- */
/* Memory Configuration                                                        */
/* -------------------------------------------------------------------------- */

/* Use lwIP's internal memory pools, not libc malloc */
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define MEMP_MEM_INIT               1  /* Zero-initialize pools at startup */

/* Heap size for variable-length allocations.
 *
 * History:
 *   - 32 KB: original VirtIO-net bringup default (DHCP + ping only).
 *   - 128 KB (PR #439): the RX-stall watchdog caught heap exhaustion
 *     under multi-session telnet + slm-put load. The hot allocations
 *     were per-frame RX pbufs (`pbuf_alloc(PBUF_RAW, len, PBUF_RAM)`)
 *     piling up in TCP receive queues.
 *   - 256 KB (#581): RX-side migrated to PBUF_POOL (see lwip_slm.c —
 *     no longer draws from this heap), so the heap now needs to
 *     cover only TCP send buffers, retransmit-queue segment data,
 *     ARP queue entries, HTTP-client state, and DNS state. With
 *     TCP_WND/TCP_SND_BUF bumped to 32*MSS (~46 KB each), a single
 *     in-progress download holds up to ~100 KB of TCP working set;
 *     256 KB gives comfortable headroom for two concurrent transfers
 *     plus transient TX pbufs without falling back to drops.
 *
 * Tradeoffs: BSS growth is +128 KB (negligible on 4-8 GB hardware
 * targets and fits the 256 MB QEMU x86-64 budget). A fragmented
 * heap walk under SYS_ARCH_PROTECT (PR #435) holds IRQs off
 * proportionally longer; the watchdog will catch IRQ-latency-driven
 * drops if this size grows further. */
#define MEM_SIZE                    (256 * 1024)  /* 256 KB */

/* Memory alignment (8-byte for AArch64) */
#define MEM_ALIGNMENT               8

/* Route the lwIP heap protection through SYS_ARCH_PROTECT (irq_save in
 * sys_arch.c) instead of the sys_mutex_t path. With NO_SYS=1 the
 * sys_mutex_* macros expand to empty no-ops (lwip/sys.h), so the default
 * setting of 0 leaves `mem_malloc/mem_free/mem_trim` completely
 * unprotected — every shell-task lwIP call (cmd_ping → raw_sendto,
 * cmd_telnetd → tcp_listen/tcp_close) races against the net_pump task
 * that drives `tcp_input`, `pbuf_alloc`, and the timer wheel. Setting
 * this to 1 makes lwIP wrap each heap critical section in
 * SYS_ARCH_PROTECT → sys_arch_protect() → irq_save(), which serialises
 * task context against task context AND task against IRQ on the local
 * CPU. Surfaced as `lwIP ASSERT: invalid next ptr at .../mem.c:790`
 * after PR #430 bumped the pbuf and TCP segment pools — bigger pools
 * meant more heap traffic which widened the race window. */
#define LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT 1

/* Allow sending without copying (zero-copy TX) */
#define LWIP_NETIF_TX_SINGLE_PBUF   1

/* -------------------------------------------------------------------------- */
/* Buffer Pools (pbuf)                                                         */
/* -------------------------------------------------------------------------- */

/* Number of pbufs in the pool.
 *
 * Increased from 16 to 64 for #427: under fast connect/disconnect
 * cycles on the telnet shell port, lingering TCP TIME_WAIT PCBs
 * each retain pbufs for retransmit / unACKed segments. With a 16-
 * pbuf pool the pool would saturate after ~8 close-then-reopen
 * cycles, at which point `pbuf_alloc` in the cdc_ecm RX path
 * started failing — incoming frames (including ARP requests for
 * our own IP) were dropped at the netif boundary. The host's ARP
 * cache then went `(incomplete)` for SLM-OS's IP, breaking both
 * ping and any new TCP connect from outside. 64 pbufs gives each
 * of the 16 max-shell-sessions enough headroom for retransmit
 * queues + a comfortable RX buffer without significantly bumping
 * static memory footprint (64 × 1536 = 96 KB). */
#define PBUF_POOL_SIZE              64

/* Size of each pbuf in pool (standard Ethernet MTU + headers) */
#define PBUF_POOL_BUFSIZE           1536

/* -------------------------------------------------------------------------- */
/* Protocol Support                                                            */
/* -------------------------------------------------------------------------- */

/* IPv4 support (required) */
#define LWIP_IPV4                   1

/* IPv6 support (disabled for simplicity) */
#define LWIP_IPV6                   0

/* ICMP (ping) support */
#define LWIP_ICMP                   1
#define LWIP_RAW                    1  /* Required for ping implementation */

/* ARP (Address Resolution Protocol) for Ethernet */
#define LWIP_ARP                    1
#define ARP_TABLE_SIZE              10
#define ARP_QUEUEING                1

/* Address Conflict Detection - disabled (requires extra code) */
#define LWIP_ACD                    0

/* DHCP client for automatic IP configuration */
#define LWIP_DHCP                   1
#define DHCP_DOES_ARP_CHECK         0  /* Disable - requires ACD */
#define LWIP_DHCP_DOES_ACD_CHECK    0  /* Disable ACD check in DHCP */

/* TCP support (for future telnet/HTTP) */
#define LWIP_TCP                    1
#define TCP_MSS                     1460
/* Per-connection TCP receive + send window. Both bumped 4*MSS → 32*MSS
 * (5840 → 46720 bytes) for #581: the prior 5.8 KB receive window meant
 * a 1 GB transfer required ~170 K RTTs end-to-end (≥ 170 s on a 1 ms
 * LAN, far worse on real internet paths) regardless of available
 * bandwidth. 46 KB is well under lwip's 64 KB unscaled-window cap
 * (RFC 7323 scaling not enabled here) and saturates a 100 Mb/s link
 * at 4 ms RTT or a 1 Gb/s link at 0.4 ms RTT. Per-connection working
 * set under load is roughly TCP_WND + TCP_SND_BUF ≈ 92 KB; MEM_SIZE
 * was bumped to 256 KB to cover this with headroom for two concurrent
 * transfers. */
#define TCP_WND                     (32 * TCP_MSS)
#define TCP_SND_BUF                 (32 * TCP_MSS)
/* TCP_SND_QUEUELEN must hold the larger send buffer's worth of
 * pbufs. lwip's recommended setting is (2 * TCP_SND_BUF) / TCP_MSS
 * = 64 for the new TCP_SND_BUF = 32*MSS. 16 → 64. */
#define TCP_SND_QUEUELEN            64
/* MEMP_NUM_TCP_PCB is the *total* pool of active TCP PCBs:
 *   - established connections
 *   - half-open SYN_RCVD
 *   - TIME_WAIT after close (default lifetime 2*MSL ≈ 60s)
 *
 * Bumped from 5 to 32 for #427: shell-tcp accepts up to
 * MAX_TCP_SHELL_SESSIONS=16 concurrent telnet sessions, and a fast
 * connect→disconnect cycle leaves each session in TIME_WAIT for ~60s.
 * With the pool at 5, four close cycles in <60s permanently
 * exhausted the table — `tcp_listen` could no longer hand a fresh
 * connection a PCB. 32 = 16 active + 16 TIME_WAIT headroom. */
#define MEMP_NUM_TCP_PCB            32
#define MEMP_NUM_TCP_PCB_LISTEN     4
/* TCP segment buffers. Sized to cover the per-connection
 * TCP_SND_QUEUELEN (64) plus headroom for a second concurrent
 * download and the 16 max telnet sessions' steady-state windows.
 * 16 → 64 (PR #427 telnet-pool fix) → 128 (#581: bumped alongside
 * TCP_SND_QUEUELEN so a single 1 GB transfer doesn't starve the
 * telnetd accept path during the transfer's burst window). Each
 * tcp_seg slot is ~32 bytes of BSS, so 128 slots = ~4 KB. */
#define MEMP_NUM_TCP_SEG            128

/* UDP support (for DNS, DHCP) */
#define LWIP_UDP                    1
#define MEMP_NUM_UDP_PCB            4

/* DNS client */
#define LWIP_DNS                    1

/* altcp layer required by lwIP http_client */
#define LWIP_ALTCP                  1

/* Autoip (link-local addressing) - disabled */
#define LWIP_AUTOIP                 0

/* IGMP (multicast) - disabled */
#define LWIP_IGMP                   0

/* -------------------------------------------------------------------------- */
/* Network Interfaces                                                          */
/* -------------------------------------------------------------------------- */

/* Single network interface */
#define LWIP_SINGLE_NETIF           1

/* Enable netif status/link callbacks */
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1

/* Hostname for DHCP */
#define LWIP_NETIF_HOSTNAME         1

/* Hardware address length (Ethernet = 6) */
#define NETIF_MAX_HWADDR_LEN        6

/* Loopback interface */
#define LWIP_HAVE_LOOPIF            0
#define LWIP_NETIF_LOOPBACK         0

/* -------------------------------------------------------------------------- */
/* Checksum Configuration                                                      */
/* -------------------------------------------------------------------------- */

/* Let CPU calculate all checksums (no hardware offload) */
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_ICMP           1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1
#define CHECKSUM_CHECK_ICMP         1

/* -------------------------------------------------------------------------- */
/* Statistics and Debugging                                                    */
/* -------------------------------------------------------------------------- */

/* Enable statistics collection */
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          1

/* Debug output (can be enabled per-module) */
#define LWIP_DEBUG                  0
#define LWIP_DBG_MIN_LEVEL          LWIP_DBG_LEVEL_ALL
#define LWIP_DBG_TYPES_ON           LWIP_DBG_ON

/* Per-module debug (uncomment to enable) */
#if LWIP_DEBUG
#define ETHARP_DEBUG                LWIP_DBG_ON
#define NETIF_DEBUG                 LWIP_DBG_ON
#define PBUF_DEBUG                  LWIP_DBG_OFF
#define ICMP_DEBUG                  LWIP_DBG_ON
#define IP_DEBUG                    LWIP_DBG_ON
#define TCP_DEBUG                   LWIP_DBG_OFF
#define UDP_DEBUG                   LWIP_DBG_OFF
#define DHCP_DEBUG                  LWIP_DBG_ON
#endif

/* -------------------------------------------------------------------------- */
/* Timeouts                                                                    */
/* -------------------------------------------------------------------------- */

/* Number of active timeout handlers */
#define MEMP_NUM_SYS_TIMEOUT        8

/* TCP timer intervals */
#define TCP_TMR_INTERVAL            250
#define TCP_FAST_INTERVAL           250
#define TCP_SLOW_INTERVAL           500

/* -------------------------------------------------------------------------- */
/* Application Hooks                                                           */
/* -------------------------------------------------------------------------- */

/* No application hooks by default */
#define LWIP_HOOK_UNKNOWN_ETH_PROTOCOL(p, netif) 0

/* -------------------------------------------------------------------------- */
/* Sanity Checks                                                               */
/* -------------------------------------------------------------------------- */

/* Enable internal consistency checks */
#define LWIP_ASSERT_CORE_LOCKED()   /* NO_SYS mode, nothing to check */

#endif /* LWIPOPTS_H */
