# Networking

TCP/IP networking support using lwIP with pluggable NIC drivers.

**Status:** Implemented on QEMU (ARM64 + x86-64); hardware platforms pending real NIC drivers.

---

## Overview

SLM-OS includes a networking subsystem that provides TCP/IP connectivity via the lwIP TCP/IP stack. A lightweight `net_driver` abstraction decouples the lwIP netif adapter from any specific hardware driver, letting each platform register its own NIC at boot time.

```
┌─────────────────────────────────────────────────────────────┐
│  Shell Commands: ping, ifconfig, netstat, net              │
├─────────────────────────────────────────────────────────────┤
│  lwIP TCP/IP Stack                                          │
│  - ICMP (ping), TCP, UDP, DHCP                              │
├─────────────────────────────────────────────────────────────┤
│  lwip_slm.c (SLM-OS wrapper)                                │
│  - sys_arch.c: critical sections, timers                    │
│  - netif adapter: bridges lwIP to struct net_driver         │
├─────────────────────────────────────────────────────────────┤
│  net_driver abstraction (kernel/include/net_driver.h)       │
│  - Platform registers driver via net_register_driver()      │
├─────────────────────────────────────────────────────────────┤
│  Drivers (one per platform)                                 │
│  - QEMU ARM64: virtio_net.c (MMIO @ 0x0A000000)             │
│  - x86-64:     virtio_net_pci.c (PCI, BAR-mapped regs)      │
└─────────────────────────────────────────────────────────────┘
```

---

## Configuration

### lwIP Settings

The lwIP stack is configured in `kernel/include/lwipopts.h`:

| Option | Value | Description |
|--------|-------|-------------|
| `NO_SYS` | 1 | Single-threaded, polled mode |
| `MEM_SIZE` | 32KB | Heap size for lwIP allocations |
| `PBUF_POOL_SIZE` | 16 | Number of packet buffers |
| `PBUF_POOL_BUFSIZE` | 1536 | Size of each packet buffer |
| `LWIP_TCP` | 1 | Enable TCP support |
| `LWIP_UDP` | 1 | Enable UDP support |
| `LWIP_ICMP` | 1 | Enable ICMP (ping) support |
| `LWIP_DHCP` | 1 | Enable DHCP client |
| `LWIP_SOCKET` | 0 | Disable BSD sockets (raw API only) |
| `LWIP_NETCONN` | 0 | Disable netconn API |

### Default IP Configuration

QEMU user-mode networking provides:

| Parameter | Value |
|-----------|-------|
| IP Address | 10.0.2.15 |
| Netmask | 255.255.255.0 |
| Gateway | 10.0.2.2 |
| DNS | 10.0.2.3 |

The gateway (10.0.2.2) is the host machine from the VM's perspective.

---

## Shell Commands

### net

Initialize or query network status.

```
SLM-OS> net init
Initializing network...
Network initialized successfully

SLM-OS> net status
Network: UP
  Interface: sl0
  IP Address: 10.0.2.15
  Link: connected
```

### ping

Send ICMP echo requests to a remote host.

```
SLM-OS> ping 10.0.2.2
PING 10.0.2.2: 4 packets
Reply from 10.0.2.2: seq=1 time=1 ms
Reply from 10.0.2.2: seq=2 time=0 ms
Reply from 10.0.2.2: seq=3 time=0 ms
Reply from 10.0.2.2: seq=4 time=0 ms

--- 10.0.2.2 ping statistics ---
4 packets transmitted, 4 received, 0% packet loss
rtt min/avg/max = 0/0/1 ms

SLM-OS> ping 10.0.2.2 2
PING 10.0.2.2: 2 packets
Reply from 10.0.2.2: seq=1 time=0 ms
Reply from 10.0.2.2: seq=2 time=0 ms

--- 10.0.2.2 ping statistics ---
2 packets transmitted, 2 received, 0% packet loss
rtt min/avg/max = 0/0/0 ms
```

### ifconfig

Display or configure network interface.

```
SLM-OS> ifconfig
sl0: flags=UP,STATIC
     ether 52:54:00:12:34:56
     inet 10.0.2.15  netmask 255.255.255.0
     gateway 10.0.2.2

SLM-OS> ifconfig dhcp
DHCP enabled

SLM-OS> ifconfig 192.168.1.100 255.255.255.0 192.168.1.1
IP set to 192.168.1.100
```

### netstat

Display network statistics.

```
SLM-OS> netstat
Network Statistics:
  RX packets: 42  bytes: 6048
  TX packets: 38  bytes: 3192
  RX errors:  0  dropped: 0
  TX errors:  0
```

---

## Implementation

### Files

| File | Purpose |
|------|---------|
| `kernel/include/net.h` | Public network API |
| `kernel/include/net_driver.h` | Driver abstraction (`struct net_driver`) |
| `kernel/include/lwipopts.h` | lwIP configuration |
| `kernel/include/virtio.h` | VirtIO MMIO definitions |
| `kernel/include/virtio_net.h` | VirtIO-Net MMIO driver API |
| `kernel/drivers/virtio_net.c` | VirtIO-Net MMIO driver (QEMU ARM64) |
| `kernel/drivers/virtio_net_pci.c` | VirtIO-Net PCI driver (x86-64) |
| `kernel/net/lwip_slm.c` | lwIP wrapper, netif, driver registration |
| `kernel/net/sys_arch.c` | lwIP OS abstraction (sys_now, IRQ-safe locks) |
| `kernel/src/net_shell.c` | Shell commands |
| `kernel/tests/test_net.c` | Network tests |

### Driver Abstraction

Each platform implements a `struct net_driver` and registers it during kernel init:

```c
struct net_driver {
    const char *name;
    int  (*init)(void);
    int  (*send)(const void *buf, size_t len);
    int  (*recv)(void *buf, size_t max_len);
    void (*get_mac)(uint8_t mac[6]);
    bool (*link_status)(void);
};

void net_register_driver(const struct net_driver *drv);
```

`net_init()` calls the registered driver's `init()` function; the lwIP netif
adapter (`lwip_slm.c`) funnels all packet I/O through the driver ops. Adding
a new NIC driver means writing one C file that exports a `net_driver` and
calling `net_register_driver()` before `net_init()` — no changes to lwIP
integration are required.

### Key Functions

| Function | Description |
|----------|-------------|
| `net_init()` | Initialize VirtIO driver and lwIP stack |
| `net_is_up()` | Check if network is initialized |
| `net_poll()` | Poll for incoming packets (call frequently) |
| `net_ping()` | Send ICMP echo request |
| `net_get_info()` | Get current IP configuration |
| `net_set_static_ip()` | Configure static IP address |
| `net_enable_dhcp()` | Enable DHCP client |
| `net_get_stats()` | Get TX/RX statistics |

### VirtIO-Net MMIO Driver (QEMU ARM64)

`kernel/drivers/virtio_net.c` uses VirtIO MMIO transport at address
0x0A000000 on the QEMU virt machine:

1. **Device Discovery**: Check magic number and device ID
2. **Feature Negotiation**: Enable MAC address and status features
3. **Queue Setup**: Initialize TX and RX virtqueues
4. **Packet I/O**: DMA-based packet transmission and reception

### VirtIO-Net PCI Driver (x86-64)

`kernel/drivers/virtio_net_pci.c` uses VirtIO PCI transport — the modern
virtio-net device exposes its config, notify, ISR, and device-specific
regions through PCI capability structures (cap_vndr=0x09). The driver:

1. Scans PCI for vendor 0x1AF4 / device 0x1041 (modern) or 0x1000
   (transitional).
2. Walks the PCI capabilities list to locate each `cfg_type` region
   (common, notify, ISR, device) and maps them at their BAR+offset.
3. Negotiates features (MAC, STATUS, VIRTIO_F_VERSION_1).
4. Sets up RX/TX virtqueues in guest RAM with the same split-ring
   format used by the MMIO driver.
5. Disables MSI-X (uses polling via `net_poll()`).

The virtqueue ring layout (descriptor table, available ring, used ring)
is identical between the two drivers; only the transport differs.

### Descriptor Ring Cache Maintenance

VirtIO devices DMA through the point of coherency (PoC). On platforms
where secondary-CPU L1/L2 caches do not participate in coherency
(`PLATFORM_HAS_NC_MEMORY` — Pi 5, Jetson — SMPEN not set), a plain
`dsb sy` is not enough: dirty cachelines for the descriptor table, the
`avail` ring slot, and `avail->idx` stay in L1/L2 and the device reads
stale data from DRAM.

`virtqueue_add_buf()` in `kernel/drivers/virtio_net.c` pairs each
`virtio_mb()` with `cache_clean_range()` on the three regions it wrote.
`virtqueue_get_buf()` uses `cache_invalidate_range()` on `used->idx`
and the used-ring slot so the CPU re-reads PoC values written by the
device. On coherent platforms the cache helpers resolve to a `dmb ish`
(QEMU ARM64) or a compiler barrier (x86-64) — there is no per-platform
`#ifdef` in the driver.

A set of synthetic-virtqueue unit tests in `kernel/tests/test_net.c`
exercises `virtqueue_add_buf` / `virtqueue_get_buf` bookkeeping (free
list, avail-idx wrap, descriptor reuse) independent of any device, so
regressions in the cache-maintenance calls surface in `make test`.

### lwIP Integration

The integration runs in `NO_SYS` mode (single-threaded):

- **Polling**: `net_poll()` must be called regularly to process packets
- **Timers**: lwIP timers are checked during poll
- **Critical Sections**: `sys_arch_protect()` uses spinlocks
- **Memory**: Static pools, no dynamic allocation

---

## QEMU Configuration

The Makefile configures QEMU with VirtIO networking, selecting the
transport (MMIO vs PCI) per platform:

```makefile
ifeq ($(PLATFORM),X86_64)
    QEMU_NET := -device virtio-net-pci,netdev=net0 -netdev user,id=net0
else ifeq ($(PLATFORM),QEMU_VIRT)
    QEMU_NET := -device virtio-net-device,netdev=net0 -netdev user,id=net0
endif
```

This provides user-mode networking where:
- The guest can access the host and internet via NAT
- Host cannot initiate connections to guest (use port forwarding)
- No root/admin privileges required

### Port Forwarding

To expose a guest port to the host:

```makefile
QEMU_NET := -device virtio-net-device,netdev=net0 \
            -netdev user,id=net0,hostfwd=tcp::2222-:23
```

This forwards host port 2222 to guest port 23 (telnet).

---

## Platform Support

| Platform | Status | Transport | Driver |
|----------|--------|-----------|--------|
| QEMU virt (ARM64) | Implemented | VirtIO MMIO | `virtio_net.c` |
| x86-64 QEMU | Implemented | VirtIO PCI | `virtio_net_pci.c` |
| Raspberry Pi 5 | Not implemented | — | Requires RP1 gigabit Ethernet driver |
| Jetson Orin Nano | Not implemented | — | Requires Realtek/Intel NIC driver |

### Build-Time Configuration

Networking is gated behind the `ENABLE_NETWORKING` CMake option, which
defaults `ON` on platforms that have a driver (QEMU_VIRT, X86_64) and
`OFF` on the others. The option controls both the lwIP library
compilation and the `ENABLE_NETWORKING` preprocessor define used in
`test_net.c`, `shell.c`, `main.c`, and `test_harness.c`:

```c
#if defined(ENABLE_NETWORKING)
// Networking code
#endif
```

To enable networking on a platform without a driver, write a new
`struct net_driver` implementation, register it during platform init,
and add `-DENABLE_NETWORKING=ON` to the CMake invocation.

---

## API Reference

### net_ip4_addr

Create IPv4 address in network byte order.

```c
uint32_t addr = net_ip4_addr(10, 0, 2, 15);
```

### net_ip_to_str

Convert IP address to string.

```c
char buf[16];
net_ip_to_str(addr, buf);  // "10.0.2.15"
```

### net_str_to_ip

Parse IP address from string.

```c
uint32_t addr;
if (net_str_to_ip("10.0.2.15", &addr) == 0) {
    // Success
}
```

### net_ping

Send ICMP echo request with callback.

```c
void my_callback(uint16_t seq, uint32_t addr, uint32_t rtt_ms,
                 bool success, void *user) {
    if (success) {
        uart_printf("Reply: seq=%d, rtt=%dms\n", seq, rtt_ms);
    }
}

net_ping(net_ip4_addr(10, 0, 2, 2), 1, my_callback, NULL);

// Must poll for response
while (!done) {
    net_poll();
}
```

---

## Testing

Network tests are in `kernel/tests/test_net.c`:

| Test | Description |
|------|-------------|
| `test_net_ip4_addr_basic` | IP address creation |
| `test_net_ip4_addr_edge_cases` | 0.0.0.0, 255.255.255.255 |
| `test_net_ip_to_str_*` | Address to string conversion |
| `test_net_str_to_ip_valid` | Valid address parsing |
| `test_net_str_to_ip_invalid` | Invalid input handling |
| `test_net_ip_roundtrip` | Parse and format consistency |
| `test_net_is_up_*` | Network state queries |
| `test_net_get_info_*` | Configuration retrieval |
| `test_net_get_stats_*` | Statistics functions |

Run tests with:

```bash
make test
```

---

## Future Work

- **Pi 5 NIC driver**: RP1 gigabit Ethernet driver
- **Jetson NIC driver**: Realtek/Intel NIC for Orin Nano Devkit
- **TCP server**: Accept incoming connections
- **HTTP client**: Download model updates
- **mDNS**: Zero-configuration discovery
- **TLS**: Secure communications

See `docs/networking-expansion-plan.md` for the full hardware-driver
roadmap.

---

*Last updated: April 2026*
