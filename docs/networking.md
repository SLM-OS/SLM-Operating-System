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

### Auto-DHCP at Boot

The `NET_DHCP_AT_BOOT` CMake option (default ON) makes `net_init()`
call `dhcp_start()` before returning, so the system comes up with a
DHCP-assigned address without a manual `ifconfig dhcp` invocation.

Behavior:
- Non-blocking: lwIP runs DISCOVER/OFFER/REQUEST/ACK in the background
  while `net_poll()` drives the timers.
- Timeout: if no DHCP server responds within `dhcp_timeout_ms`
  (`NET_DHCP_TIMEOUT_DEFAULT_MS` = 10 s at compile time, runtime-
  adjustable via `net_set_dhcp_timeout_ms()`), the system stops DHCP
  and falls back to the static configuration that was applied at init.
- Status: `struct net_info.dhcp_status` reports DISABLED, PENDING,
  BOUND, or FAILED. `ifconfig` prints this as `DHCP(bound)` etc.

Override with `-DNET_DHCP_AT_BOOT=OFF` to restore the previous
manual-`ifconfig dhcp` behavior.

**Runtime DHCP API** (primarily for tests and diagnostics):

| Function | Purpose |
|---|---|
| `net_set_dhcp_timeout_ms(ms)` | Override the bind timeout at runtime. Accepts 0 for immediate fallback (tests). |
| `net_get_dhcp_timeout_ms()` | Query the current timeout. |
| `net_dhcp_check_timeout()` | Run the fallback check directly. Returns 1 if fallback fired, 0 otherwise. Used by tests to deterministically trigger `NET_DHCP_FAILED` without racing the recv path. |
| `net_get_dhcp_bind_count()` | Number of distinct DHCP-bound transitions since boot. Increments on every false→true edge of `dhcp_supplied_address()`, i.e. every fresh bind after DISCOVER/OFFER/REQUEST/ACK. Used by tests (#201) to verify the bind announcement fired. |

When DHCP binds successfully, `net_poll()` detects the transition and
logs `[INFO] DHCP bound: IP=... GW=... Mask=...` once per bind. This
saves the user from running `ifconfig` after boot just to discover the
DHCP-assigned address.

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

Display or configure network interface. With auto-DHCP at boot, the
default output shows the DHCP-bound state:

```
SLM-OS> ifconfig
sl0: flags=UP,DHCP(bound)
     ether 52:54:00:12:34:56
     inet 10.0.2.15  netmask 255.255.255.0
     gateway 10.0.2.2

SLM-OS> ifconfig dhcp
DHCP enabled

SLM-OS> ifconfig 192.168.1.100 255.255.255.0 192.168.1.1
IP set to 192.168.1.100
```

The DHCP status label on line one is one of:
- `DHCP(bound)` — DHCP server answered, this IP is a lease
- `DHCP(pending)` — DHCP started, no offer yet (normal during boot)
- `DHCP(failed)` — DHCP timed out, fell back to static IP
- `STATIC` — DHCP disabled or never started

### netstat

Display network statistics.

```
SLM-OS> netstat
Network Statistics:
  RX packets: 42  bytes: 6048
  TX packets: 38  bytes: 3192
  RX errors:  0  dropped: 0  no_buffers: 0
  TX errors:  0
```

Counter semantics:
- `errors` — protocol/format errors seen by the driver during TX/RX
- `dropped` — packets that reached lwIP but couldn't be enqueued (pbuf alloc failed, or lwIP netif input rejected the packet)
- `no_buffers` — the NIC driver couldn't post a fresh RX descriptor after recv (virtqueue descriptor pool exhausted under burst). Nonzero here indicates sustained traffic overrunning the 16-buffer default pool.

### tcpsh

Multi-session TCP shell — start the listener, then connect with
`nc localhost 2323` from the host:

```
SLM-OS> tcpsh start
[TCPSH] Listening on 0.0.0.0:2323 (unauthenticated — trusted networks only)
tcpsh: listening on port 2323

SLM-OS> tcpsh status
tcpsh: running on port 2323 — accepted=0 active=0 max=2

SLM-OS> tcpsh stop
tcpsh: stopped
```

The TCP shell uses the lwIP raw callback API (required because the
port builds with `NO_SYS=1` / `LWIP_SOCKET=0`). See
`docs/shell.md` § Multi-Session Shell and
`docs/multi-session-shell-plan.md` for the architecture.

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
    int  (*send)(const void *buf, size_t len);  /* async submit */
    int  (*recv)(void *buf, size_t max_len);
    void (*get_mac)(uint8_t mac[6]);
    bool (*link_status)(void);
    void (*tx_reap)(void);                       /* called from net_poll */
};

void net_register_driver(const struct net_driver *drv);
```

`net_init()` calls the registered driver's `init()` function; the lwIP netif
adapter (`lwip_slm.c`) funnels all packet I/O through the driver ops. Adding
a new NIC driver means writing one C file that exports a `net_driver` and
calling `net_register_driver()` before `net_init()` — no changes to lwIP
integration are required.

**TX is asynchronous** (#204): `send()` submits a packet by allocating a slot
from the driver's TX buffer pool, copying the data, queuing a descriptor on
the TX virtqueue, kicking the device, and returning. It does **not** spin
waiting for the device to ack. Returns:

- `0` — submitted; the caller may free the input buffer
- `NET_E_BUSY` — TX pool exhausted (all slots in flight); caller should
  retry after `net_poll()` runs
- `NET_E_TOO_LARGE` — packet exceeds 1514 bytes
- other `NET_E_*` — see Error codes table in API Reference

Completion happens later when `net_poll()` calls `tx_reap()`, which drains
the TX used ring and frees the pool slots. Drivers that complete TX
synchronously (e.g. an in-driver IRQ handler that clears the slot) may
leave `tx_reap` NULL; the polling fallback only runs when it's set.

The TX buffer pool is sized to allow multiple in-flight packets (currently
16 in both VirtIO drivers — adjustable per driver). Bursts up to that depth
submit without blocking; sustained traffic above that depth surfaces as
`NET_E_BUSY` from `send()`. The retry behaviour depends on the caller:

- **TCP** (via lwIP) — the packet sits in the TCP retransmit queue and
  `tcp_slowtmr()` re-invokes `linkoutput` after the retransmit interval.
- **ARP** (via lwIP) — pending packets are queued in the ARP layer and
  retried when ARP resolves.
- **UDP, ICMP, raw** — lwIP returns `ERR_IF` from `linkoutput` and the
  packet is dropped at the netif. One-shot; the application sees the
  drop (UDP is best-effort, ICMP likewise).

`net_poll()` itself does not re-submit dropped packets — its role is
only to drain `tx_reap` so the pool has free slots by the time the
next `send()` runs.

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

1. **Device Discovery**: Scan all 32 MMIO slots
   (0x0A000000 + slot × 0x200) for the first slot with
   device_id == VIRTIO_DEVICE_NET. QEMU assigns virtio-mmio devices
   to the highest free slot, so the slot the NIC lands in depends
   on what other `-device` flags were passed — hardcoding slot 0 is
   wrong. The GIC IRQ derives from the slot the device actually
   landed in.
2. **Version Check**: Accept only version ≥ 2 (modern MMIO). Version 1
   (legacy) uses the `QUEUE_PFN` register layout, which this driver
   does not implement. Run QEMU with `-global
   virtio-mmio.force-legacy=false` (already in `QEMU_NET` — see below)
   to get the modern interface; otherwise `virtio_net_init()` fails
   loudly rather than hanging on the first silent TX timeout.
3. **Feature Negotiation**: `VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS |
   VIRTIO_F_VERSION_1`. VERSION_1 is required by the modern transport
   and implies a 12-byte `struct virtio_net_hdr` (see "Packet Header
   Size" below).
4. **Queue Setup**: Initialize RX (queue 0) and TX (queue 1) virtqueues
5. **Packet I/O**: DMA-based packet transmission. TX completions are
   reaped both by `net_poll()` (opportunistic, keeps pool slots free
   under heavy load without waiting for the IRQ to land) and by the
   registered IRQ handler — see "IRQ Dispatch" below. RX is still
   drained from `net_poll()` (moving RX to IRQ context would require
   `pbuf_alloc` and lwIP input from IRQ, a much bigger change).
6. **IRQ Dispatch**: After queue setup the driver calls
   `gic_register_handler(irq, virtio_net_irq_handler)` and
   `gic_enable_irq(irq)`. The IRQ number is slot-derived
   (`VIRTIO_DEVICE_IRQ(slot) = 48 + slot`) and exposed via
   `virtio_net_get_irq()`. The EL1 IRQ dispatcher in
   `kernel/arch/arm64/exceptions.c` looks up the registered handler
   for any IRQ not matched by a compile-time case (timer, UART) and
   invokes it after EOI. On real hardware this drains TX pool slots
   without waiting for `net_poll()` — the on-QEMU latency
   improvement is modest because the opportunistic reap in `send()`
   already keeps the pool clear.

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
5. Probes the PCI MSI-X capability (cap id 0x11) and, if present,
   programs table entry 0 to route both queue and config-change
   interrupts to IDT vector 50 on the boot CPU's LAPIC. Both queues'
   `queue_msix_vector` registers and `msix_config` are bound to
   that entry; the capability's Enable bit is set last so no
   interrupt can fire against a half-programmed table. If MSI-X is
   absent (e.g. `-device virtio-net-pci,msix=off`) the driver falls
   back cleanly to polling via `net_poll()` — `tx_reap` on the
   net_driver hook still drains completions.
6. Registers `virtio_net_pci_irq_handler` against the IDT via
   `irq_register(VIRTIO_NET_MSIX_IRQ, ...)`. The handler drains the
   TX used ring and picks up link-status changes; RX remains
   polled (moving RX to IRQ context would need `pbuf_alloc` +
   lwIP input from IRQ, same trade-off as the MMIO driver).

Accessor APIs for tests and diagnostics:
`virtio_net_pci_msix_enabled()`, `virtio_net_pci_get_msix_vector()`,
`virtio_net_pci_get_irq_count()`. The first two surface whether the
hot path is IRQ-driven vs polled; the third lets tests verify the
dispatch is alive during traffic.

The virtqueue ring layout (descriptor table, available ring, used ring)
is identical between the two drivers; only the transport differs.

### Cadence MACB/GEM Driver (Pi 5, #202)

`kernel/drivers/macb.c` drives the Gigabit Ethernet controller on the
Raspberry Pi 5. Despite the Broadcom SoC (BCM2712), the MAC itself is
**Cadence MACB/GEM** IP inside the RP1 southbridge — Linux identifies
it as `compatible = "raspberrypi,rp1-gem", "cdns,macb"`. BCM GENET
was used on Pi 4 and earlier; Pi 5 chose a different IP.

**Memory map** (within RP1's PCIe BAR1 window):

| Symbol | Address | Purpose |
|--------|---------|---------|
| `RP1_ETH_IP_BASE` | `0x1F00100000` | MACB/GEM register block (MMIO) |
| `RP1_ETH_CFG_BASE` | `0x1F00104000` | Additional CFG registers |
| `RP1_CLOCKS_BASE + 0x64` | `0x1F0001_8064` | `CLK_ETH_CTRL` — 125 MHz TX clock gate |
| `RP1_CLOCKS_BASE + 0x134` | `0x1F00018134` | `CLK_ETH_TSU_CTRL` — 50 MHz TSU clock |
| `RP1_IO_BANK1_BASE` | `0x1F000D4000` | GPIO control (GPIO 32 = PHY reset) |
| `GENET_IRQ` | `MACB_IRQ = 166` | GIC IRQ via MIP0 vector 6 → SPI 134 |

The IP + CFG bases fall in the same 2 MB page already mapped by
`vmm_setup_platform` for UART/GPIO, so no additional page-table
entry is required.

**Bring-up sequence** (`macb_init` in the driver):

1. **Clock enable.** `RP1_CLK_ETH_CTRL` and `RP1_CLK_ETH_TSU_CTRL`
   both get their `CLK_CTRL_ENABLE` bit (bit 11) set. Idempotent —
   firmware may already have them running.
2. **MID probe.** `MACB_MID` (offset 0xFC) must be non-zero; we
   confirm idnum 0x0007 (Cadence GEM).
3. **MDIO bring-up.** `NCFGR[20:18]` = MDC divider ÷96, `NCR.MPE`
   set to enable the management port.
4. **PHY reset release.** Drive RP1 GPIO 32 low then high — the
   BCM54213PE's reset line is active-low active per the Pi 5 DTS.
5. **PHY probe.** MDIO-read `PHYID1`/`PHYID2` from address 1.
   BCM54213PE reports `0x600d / 0x84a2`.
6. **Auto-negotiate.** `BMCR.ANENABLE | BMCR.ANRESTART`, then
   poll `BMSR.LSTATUS + ANEGCOMPLETE` with a 5-second budget.
   MVP assumes 1000 Mbps full-duplex post-ANEG without a
   vendor-specific AUX read.
7. **Link config.** `NCFGR` gets speed/duplex, `GEM_NCFGR_GBE` for
   1000 Mbps, plus `MACB_NCFGR_BIG` (accept 1536-byte frames) and
   `MACB_NCFGR_DRFCS` (strip FCS from RX). Without these two, the
   MAC either drops all normal frames (no BIG) or hands lwIP
   garbage (+4 bytes of FCS trailing).
8. **MAC address program.** `SA1B`/`SA1T` with the factory MAC
   obtained through a 3-tier lookup (see "MAC source priority"
   below). Pi 5 boards in the lab now boot with the address printed
   on the sticker and the DHCP reservation key Linux uses.
9. **TX ring + RX ring init.** 16 × 2048-byte TX buffers in
   cacheable DRAM (per-buffer cache maintenance at DMA sync
   points), 16 × 1536-byte RX buffers the same way. **The rings
   themselves live in non-cacheable (NC) memory** — eight 8-byte
   MACB descriptors share one 64-byte cacheline, so a CPU write to
   descriptor N would dirty the whole line and a later clean would
   write the CPU's stale view of descriptors N±1..N±7 back to
   DRAM, overwriting concurrent MAC DMA. NC memory side-steps the
   false-sharing window entirely. Allocated from the 2 MB
   `ncmem` pool (`ncmem_alloc`); falls back to cacheable BSS with
   a loud WARN if the pool is exhausted. `DMACFG` set via RMW:
   FBL=16, RXBS=24 (1536/64), RXBMS=3, TXPBMS=1, DDRP=1.
10. **Enable RE + TE in NCR.** MAC starts consuming RX descriptors
    and accepting TX kicks.

**MAC source priority (#250, #255).** `macb_program_mac_address`
walks a three-tier lookup, stopping at the first source that
yields a usable address:

1. **VideoCore mailbox, property tag `0x00010003`
   (`GET_BOARD_MAC_ADDRESS`).** BCM2712 mailbox MMIO at
   `0x107C013880`, mapped in `vmm_setup_platform`. Buffer is a
   16-byte-aligned cacheable BSS (low < 1 GB, required for the
   legacy VC bus-address encoding `phys | 0xC0000000`), with
   `cache_clean_range` + `cache_invalidate_range` around the
   round-trip. Pi 5 EEPROM implements this tag from **2025-05-08
   onward** (rpi-eeprom #698); older EEPROM returns buffer-level
   parse error `0x80000001`, which the driver treats as
   "tag unsupported on this revision" rather than a generic failure.
   Code lives in `kernel/drivers/bcm_mailbox.c`.
2. **DTB `local-mac-address`** at
   `/axi/pcie@1000120000/rp1/ethernet@100000`. The Pi 5
   bootloader patches the factory MAC into this property before
   handing the kernel off, so this tier works on **every** EEPROM
   revision — the fallback that closes the "old firmware" gap left
   by tier 1. Uses the general-purpose FDT reader at
   `kernel/lib/fdt/fdt.c` via `dtb_get_blob()`
   (`kernel/src/dtb.c`). Rejects an all-zero / all-0xFF MAC as a
   sign the bootloader didn't patch.
3. **Fixed locally-administered MAC** `02:00:00:5A:00:01` with a
   loud WARN. Last-resort safety net — single-board lab safe,
   collision-prone on a shared subnet.

Hardware result on pi-5-1 (EEPROM firmware `0x66f16700`, late 2024):
tier 1 fails with parse error, tier 2 returns `2c:cf:67:ca:a0:b5`
(Raspberry Pi Trading OUI — matches the sticker and the DHCP
reservation key Linux uses). DHCP now binds the same IP Linux
would get on the same board.

The **FDT reader** (`kernel/include/fdt.h`, `kernel/lib/fdt/fdt.c`)
is general-purpose — not MACB-specific. Any driver needing a DT-
sourced value at init time can call `fdt_init` +
`fdt_get_property_by_path` (or `fdt_get_u32` for single-cell
numerics). Tests live in `kernel/tests/test_fdt.c`.

**Polling model.** Polling is the operational path, for a different
reason than #134 (timer PPI-30 policy). The peripheral-IRQ path via
RP1 MSIX_CFG was wired up (see "IRQ infrastructure" below) and is
confirmed inert: MIP0 sees the MAC asserting but MSIX_CFG never
fires a TLP. `net_poll()` drives recv on the RX ring; `macb_tx_one`
polls the `USED` bit on the TX descriptor.

**IRQ infrastructure (dormant).** The driver registers
`macb_irq_handler` against GIC IRQ 166 via `gic_register_handler`,
unmasks MACB_IER, and configures `RP1_MSIX_CFG[vec 6]` with
`ENABLE | IACK_EN`. All correctly set up — shown by the `polled + IRQ`
status line — but no TLP ever crosses BAR3 → MIP0 → GIC. The handler
is ready to take over TX/RX drain the day the MSIX_CFG blocker is
solved (see **#247** for the shared tracker — same issue UART RX
hit). Three accessors surface the live state for investigators:

| Accessor | Purpose |
|----------|---------|
| `macb_get_irq_count()` | Bumped on every handler invocation. Stays 0 while MSIX_CFG is blocked. |
| `macb_get_last_isr()` | Last `MACB_ISR` value the handler saw. |
| `macb_irq_is_registered()` | Whether `gic_register_handler` succeeded. |

The `macbdiag` shell command (`PLATFORM_RASPI5 + ENABLE_NETWORKING`)
dumps all three alongside live MIP0 `MSIX_CFG[6]` / `INTSTATL` state.
Sample output after DHCP + ping on pi-5-1:

```
=== MACB IRQ Diagnostic ===
  GIC handler registered: YES
  IRQ count:              0            <- MSIX_CFG never fired a TLP
  Last MACB_ISR observed: 0x00000000
  MIP0 MSIX_CFG[vec 6]:   0x00000009 ENABLE IACK_EN
  MIP0 INTSTATL (0-31):   0x02000040
    ETH vec 6 asserted:    YES         <- MAC IS asserting, just not forwarded
=== End Diagnostic ===
```

**Hardware verified on pi-5-1 2026-04-17:**

```
[INFO] DHCP bound: IP=192.168.4.215 GW=192.168.4.1 Mask=255.255.255.0
Reply from 192.168.4.1: seq=2 time=3 ms
Reply from 192.168.4.1: seq=3 time=2 ms
Reply from 192.168.4.1: seq=4 time=2 ms
rtt min/avg/max = 2/2/3 ms
```

### Background net_pump task

`net_poll()` is the drain point for RX + lwIP timers — it picks up
incoming frames, runs DHCP/ARP/ICMP state machines, and fires the
ping-reply path. Foreground commands (the `ping` wait loop, the
DHCP bind wait in `net_init`) call it inline, but the shell sitting
at its prompt doesn't. Without a background driver, SLM-OS would
only respond to inbound traffic while actively sending.

`net_pump_task_entry` in `kernel/net/lwip_slm.c` loops
`net_poll()` + `sleep_ms(10)`. The task is spawned from `main.c`
at boot (guarded on `ENABLE_NETWORKING` + `!ENABLE_BOOT_TESTS`) at
`TASK_PRIORITY_IDLE` pinned to CPU 0. Pinning matches where
peripheral IRQs land today (virtio-mmio SPI, MACB SPI), and
`PRIORITY_IDLE` means the pump yields to anything else that's
runnable — `net_poll` is latency-insensitive at 10 ms cadence.

The `!ENABLE_BOOT_TESTS` guard is because the test kernel's
scheduler policy tests check exact `assigned_cpu` values and task
counts; an extra background task pinned to CPU 0 shifts those
counts. Live network tests in `test_net.c` drive `net_poll()`
inline from their wait loops, so they don't need the pump running.

Verified: with net_pump active and SLM-OS sitting at the shell
prompt on pi-5-1, an external host gets **10/10 ICMP echo replies**
at 3-15 ms RTT. Before this task, inbound pings got 0/4 unless
the Pi 5 was itself running an outbound `ping` at the time.

### Stuck-descriptor watchdog (#204 item 4)

Both drivers track a wall-clock timestamp of the last successful TX
reap. When `tx_reap_locked` runs with a non-empty in-flight pool
and has not made progress for `TX_STALL_THRESHOLD_MS` (5000 ms), it
logs a single `WARN("TX descriptors stuck: no completion for %u ms
(virtio-mmio|virtio-pci)", elapsed)` line. The latch resets on the
next successful reap, so a transient stall that resolves does not
log — but a truly stuck link logs exactly once per stall episode.
Non-fatal: the driver stays usable (polling still works) and the
warning surfaces the problem to the shell / serial log for
operator action rather than failing the whole transport.

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

The current model is validated on QEMU only. Real hardware (Pi 5
GENET, Jetson EQOS) may require additional measures — see
[`docs/net-dma-coherence.md`](net-dma-coherence.md) (#203) for the
open questions and the verification plan for the first
real-hardware NIC driver.

### Packet Header Size

`struct virtio_net_hdr` must be **12 bytes** because we negotiate
`VIRTIO_F_VERSION_1` (virtio 1.1 spec §5.1.6.1). The 12th/13th bytes
are the `num_buffers` field — zero on TX, populated by the device on
RX.

Dropping `num_buffers` and using a 10-byte header — easy to do
accidentally from older docs or code samples — silently corrupts
everything: QEMU reads the first 2 bytes of packet data as part of
the header, drops the outgoing frame before it reaches the netdev
backend, and on RX writes packets offset by 2 bytes so they never
decode. The ring-level TX completion still fires, so stats advance
and nothing looks wrong — but no traffic actually goes over the wire.

Guarded by `static_assert(sizeof(struct virtio_net_hdr) == 12, ...)`
in both the MMIO header and the PCI driver source. Any future field
addition that changes the size fails at compile time.

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
    QEMU_NET := -device virtio-net-pci,netdev=net0 \
                -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2323-:2323
else ifeq ($(PLATFORM),QEMU_VIRT)
    QEMU_NET := -global virtio-mmio.force-legacy=false \
                -device virtio-net-device,netdev=net0 \
                -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2323-:2323
endif
```

This provides user-mode networking where:
- The guest can access the host and internet via NAT
- The host's loopback reaches the guest's port 2323 (TCP shell —
  see `docs/shell.md`). Bound to `127.0.0.1` so the guest's
  unauthenticated shell doesn't accidentally become visible on the
  host's LAN.
- No root/admin privileges required

### Port Forwarding

To expose additional guest ports to the host, extend the `hostfwd=`
list — each entry forwards a single host:port to a guest port:

```makefile
# Additional forward: host 2222 → guest 22 (for a future SSH server).
QEMU_NET := -device virtio-net-device,netdev=net0 \
            -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2323-:2323,hostfwd=tcp::2222-:22
```

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

### Error codes

Public `net_*` functions return 0 (`NET_OK`) on success and a negative
`enum net_error` value on failure. Enum values:

| Code | Value | Meaning |
|---|---|---|
| `NET_OK` | 0 | Success |
| `NET_E_GENERIC` | -1 | Catch-all (preserved for source compat) |
| `NET_E_NOT_INIT` | -2 | Networking subsystem not initialized |
| `NET_E_NO_DRIVER` | -3 | No `net_driver` registered |
| `NET_E_NO_DEVICE` | -4 | Driver couldn't find hardware |
| `NET_E_NO_MEM` | -5 | Allocation failed (pbuf / pmm / pool) |
| `NET_E_BUSY` | -6 | Operation in progress (pending ping, queue full) |
| `NET_E_TIMEOUT` | -7 | TX completion, DHCP bind, ARP resolve |
| `NET_E_INVAL` | -8 | Bad argument (NULL, out of range) |
| `NET_E_TOO_LARGE` | -9 | Packet exceeds MTU |
| `NET_E_LINK_DOWN` | -10 | Physical link down |
| `NET_E_PROTO` | -11 | Feature negotiation / version mismatch |

All codes are ≤ 0, so existing `if (rc < 0)` checks continue to work
when upgrading callers. `net_strerror(int)` returns a short human
description for logging and diagnostics.

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

### DHCP Control

```c
/* Adjust the bind timeout before starting DHCP (tests, diagnostics) */
net_set_dhcp_timeout_ms(500);  /* 500 ms */
net_enable_dhcp();

/* Poll for a bit, then check status */
for (int i = 0; i < 100; i++) net_poll();

struct net_info info;
net_get_info(&info);
switch (info.dhcp_status) {
    case NET_DHCP_BOUND:    uart_puts("lease acquired"); break;
    case NET_DHCP_PENDING:  uart_puts("still waiting");  break;
    case NET_DHCP_FAILED:   uart_puts("timed out");      break;
    case NET_DHCP_DISABLED: uart_puts("not running");    break;
}
```

For tests that need deterministic fallback without racing the packet
receive path, call `net_dhcp_check_timeout()` directly after setting
`net_set_dhcp_timeout_ms(0)`.

### net_register_driver

Platform hook called during kernel init so the lwIP netif adapter can
route packet I/O through a NIC-specific driver without a per-platform
`#ifdef` in `lwip_slm.c`.

```c
static int my_nic_init(void)       { ...  return 0; }
static int my_nic_send(const void *buf, size_t len)  { ... }
static int my_nic_recv(void *buf, size_t max_len)    { ... }
static void my_nic_get_mac(uint8_t mac[6])           { ... }
static bool my_nic_link_status(void)                 { ... }

static const struct net_driver my_nic_driver = {
    .name        = "my-nic",
    .init        = my_nic_init,
    .send        = my_nic_send,
    .recv        = my_nic_recv,
    .get_mac     = my_nic_get_mac,
    .link_status = my_nic_link_status,
};

/* From platform init (kernel/src/main.c), before net_init(): */
net_register_driver(&my_nic_driver);
```

---

## Testing

Network tests are in `kernel/tests/test_net.c`. They fall into three tiers:

**Tier 1 — IP utility functions** (run on any platform with
`ENABLE_NETWORKING`, no device required):

| Test | Description |
|------|-------------|
| `test_net_ip4_addr_*` | Address creation + edge cases |
| `test_net_ip_to_str_*` | Address-to-string conversion |
| `test_net_str_to_ip_valid` / `_invalid` | Parser correctness |
| `test_net_ip_roundtrip` | Parse/format consistency |
| `test_net_get_info_*` | Info-query edge cases |
| `test_net_commands_without_init` | Graceful failure pre-init |
| `test_net_get_stats_*` | Stats-struct safety |

**Tier 2 — virtqueue ring bookkeeping** (QEMU_VIRT only; exercises
the MMIO driver's `virtqueue_add_buf` / `virtqueue_get_buf` against
a synthetic virtqueue in BSS):

| Test | Description |
|------|-------------|
| `test_virtqueue_add_buf_*` | Descriptor allocation, flags, exhaustion |
| `test_virtqueue_avail_idx_beyond_size` | Avail ring wrap at TVQ_SIZE |
| `test_virtqueue_get_buf_*` | Used ring read-back and descriptor free |
| `test_virtqueue_add_two_distinct_buffers` | Independent slots |

**Tier 3 — live integration against QEMU's virtio-net device** (runs
on both ARM64 MMIO and x86-64 PCI paths):

| Test | Description |
|------|-------------|
| `test_net_driver_registered` | Platform init hooked `net_register_driver()` |
| `test_net_init_live` | Driver probes device, reads MAC, brings link UP |
| `test_net_poll_after_init` | Recv path doesn't fault on empty ring |
| `test_net_auto_dhcp_at_boot` | `NET_DHCP_AT_BOOT=ON` starts DHCP during init |
| `test_net_dhcp_binds` | QEMU SLIRP answers DISCOVER → BOUND state |
| `test_net_dhcp_fallback` | Forced timeout → `NET_DHCP_FAILED`, static IP restored |
| `test_net_driver_tx` | Raw 64-byte frame traverses the TX virtqueue to completion |
| `test_net_driver_has_tx_reap` | Driver exposes the async TX reap op (#204) |
| `test_net_send_returns_quickly` | `send()` returns in <50 ms — guards against the spin-wait regression (#204) |
| `test_net_send_oversized_rejected` | 2048-byte packet rejected with `NET_E_TOO_LARGE` (#204) |
| `test_net_send_pool_exhaustion` | 32 submits without intervening poll never hit an unexpected error — only 0 or `NET_E_BUSY` (#204) |
| `test_net_burst_8_sends_async` | 8 back-to-back submits succeed without blocking; pool absorbs them; `net_poll()` drains completions (#204) |
| `test_net_irq_handler_registered` | MMIO driver called `gic_register_handler` — dispatch table points to `virtio_net_irq_handler` for the runtime IRQ (#204) |
| `test_net_irq_handler_drains_tx` | Direct handler invocation bumps `virtio_net_get_irq_count()` and drains the TX used ring (#204) |
| `test_net_msix_enabled` | PCI driver enabled MSI-X during init (#204 item 3) |
| `test_net_msix_vector_is_in_range` | MSI-X vector is in the 50-63 reserved range (#204 item 3) |
| `test_net_msix_handler_drains_tx` | Direct handler invocation bumps `virtio_net_pci_get_irq_count()` and drains the TX used ring (#204 item 3) |
| `test_net_mmio_watchdog_quiet` / `test_net_pci_watchdog_quiet` | Watchdog stays silent on normal TX burst + drain (#204 item 4) |
| `test_net_mmio_watchdog_fires_on_stall` / `test_net_pci_watchdog_fires_on_stall` | Synthetic trigger bumps the stall counter exactly once (#204 item 4) |

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
