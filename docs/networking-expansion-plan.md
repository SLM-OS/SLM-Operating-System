# Networking Expansion Plan

Extend networking from QEMU-only to all four supported platforms.

**Current state (25 April 2026):** Phases 1 and 2 have **landed**.
Full TCP/IP networking now works on QEMU ARM64, QEMU x86-64,
Raspberry Pi 5, and the validated Jetson `jetson-nano-2` lab topology.
The Jetson path landed through USB CDC-ECM over the Tegra XHCI host
after Linux `kexec`; the remaining Jetson work now splits into:
- internal Ethernet over the Super Dev Kit's PCIe RTL8168 path (#25)
- broader validation of the shipped USB path (#385)
- Jetson xHCI robustness follow-up work (#386)
- broader USB networking and USB host generalization (#387, #384)

Landed work summary:
- `struct net_driver` abstraction (`kernel/include/net_driver.h`)
- x86-64 VirtIO-Net PCI driver (`kernel/drivers/virtio_net_pci.c`)
- `ENABLE_NETWORKING` CMake option (default ON for QEMU_VIRT and X86_64)
- Auto-DHCP at boot with static-IP fallback (closes #197)
- Live integration tests covering init, TX, RX, DHCP BOUND and FAILED
- Pi 5 Cadence MACB/GEM driver (`kernel/drivers/macb.c`)
- Jetson USB CDC-ECM over retained XHCI/root-hub handoff (#266)

Open follow-up tickets:
- #200 — Scheduler + integration test flakiness (pre-existing, observed during this work)
- #201 — Shell message when DHCP binds
- #203 — DMA coherence verification for real-hardware NICs
- #25 — Jetson internal Ethernet path via PCIe RTL8168
- #384 — General USB host topology/class support beyond the current Jetson NIC path
- #385 — Broaden Jetson USB networking validation beyond the current lab topology
- #386 — Jetson xHCI robustness follow-up tracker
- #387 — Generalize USB networking beyond the current Jetson CDC-ECM path

**Last updated:** 25 April 2026

---

## Phase 1: Decouple Networking from QEMU (No Hardware Needed) ✅ LANDED

The networking stack is currently hardcoded to QEMU via build gates and
VirtIO MMIO slot probing. This phase makes the stack platform-agnostic
so any platform with a NIC driver gets networking automatically.

### 1.1 Extract the Platform Gate from lwIP

**Problem:** `CMakeLists.txt` compiles lwIP, `sys_arch.c`, `lwip_slm.c`,
and `net_shell.c` only when `PLATFORM == QEMU_VIRT`. The lwIP stack
itself has no platform dependency — only the driver does.

**Fix:** Introduce a `NETWORKING` CMake option (default ON for QEMU_VIRT,
OFF for others until drivers exist). Gate lwIP and the network API on
`NETWORKING=ON`, gate individual drivers on platform.

```
CMake structure:
  NETWORKING=ON  → lwIP core, sys_arch.c, lwip_slm.c, net_shell.c, net.h API
  PLATFORM=QEMU_VIRT  → virtio_net.c
  PLATFORM=X86_64     → virtio_net_pci.c (new)
  PLATFORM=RASPI5     → macb.c
  PLATFORM=JETSON     → CDC-ECM over tegra_xhci retained handoff
```

**Files to modify:**
- `CMakeLists.txt` — restructure networking source gates
- `kernel/net/lwip_slm.c` — remove `#ifdef PLATFORM_QEMU_VIRT` guards;
  make `net_init()` call a platform-provided driver init function
- `kernel/tests/test_net.c` — gate on `NETWORKING` not `PLATFORM_QEMU_VIRT`

### 1.2 Create a NIC Driver Registration Interface

**Problem:** `lwip_slm.c` directly calls `virtio_net_init()`,
`virtio_net_send()`, `virtio_net_recv()`. Adding a second driver means
either `#ifdef` chains or a proper abstraction.

**Fix:** Lightweight driver ops struct:

```c
struct net_driver {
    const char *name;
    int  (*init)(void);
    int  (*send)(const void *buf, size_t len);
    int  (*recv)(void *buf, size_t max_len);
    void (*get_mac)(uint8_t mac[6]);
    bool (*link_status)(void);
};
```

`lwip_slm.c` calls through the active driver pointer. Each platform
registers its driver at boot. VirtIO-Net becomes the first
implementation.

**Files to create:**
- `kernel/include/net_driver.h` — driver interface definition

**Files to modify:**
- `kernel/net/lwip_slm.c` — call through `net_driver` ops instead of
  `virtio_net_*()` directly
- `kernel/drivers/virtio_net.c` — export a `struct net_driver` instance

### 1.3 Portable sys_arch Fixes

`kernel/net/sys_arch.c` uses `timer_get_count()` /
`timer_get_frequency()` which are available on all platforms. Verify:
- `sys_now()` returns correct milliseconds on x86-64 (TSC-based timer)
- `sys_arch_protect()` / `sys_arch_unprotect()` use real spinlocks
  (currently marked TODO for SMP)

---

## Phase 2: x86-64 Networking (No Hardware Needed) ✅ LANDED

x86-64 is the highest-value target because it can be fully developed and
tested in QEMU with no hardware access.

### 2.1 Add VirtIO-Net PCI Device to QEMU Launch

**Problem:** The x86-64 QEMU command line has no network device.

**Fix:** Add to Makefile's x86-64 QEMU flags:

```makefile
QEMU_NET_X86 := -device virtio-net-pci,netdev=net0 \
                -netdev user,id=net0
```

This creates a PCI VirtIO-Net device discoverable via ECAM.

### 2.2 Write VirtIO-Net PCI Driver

**Problem:** The existing VirtIO-Net driver uses MMIO transport
(hardcoded slot at `0x0A000000 + slot*0x200`). x86-64 QEMU exposes
VirtIO as a PCI device instead.

**Approach:** Write `kernel/drivers/virtio_net_pci.c` that:
1. Discovers VirtIO-Net via PCI enumeration (vendor `0x1AF4`,
   device `0x1000` or `0x1041`)
2. Maps BAR0 for VirtIO PCI config access
3. Uses the PCI VirtIO transport (BAR-based registers instead of MMIO
   slots) — the virtqueue ring format is identical
4. Shares TX/RX buffer management with the MMIO driver where possible,
   or duplicates the ~200 lines of virtqueue code

**Reference:** The x86-64 PCI enumeration is already working
(`kernel/arch/x86_64/pci.c`). The NVIDIA GPU driver
(`kernel/arch/x86_64/nvidia_gpu.c`) provides a pattern for PCI device
discovery + BAR mapping.

**VirtIO PCI vs MMIO differences:**
- Device config is at BAR0 offsets (not fixed MMIO)
- Capability structures in PCI config space identify BAR regions
- ISR status via PCI interrupt (or MSI-X)
- Virtqueue notify via BAR offset (not MMIO doorbell)

**Files to create:**
- `kernel/drivers/virtio_net_pci.c` — PCI transport VirtIO-Net driver

**Files to modify:**
- `CMakeLists.txt` — add `virtio_net_pci.c` for X86_64
- `Makefile` — add QEMU network device flags for x86-64

### 2.3 Testing

All testable in QEMU:
- `make test PLATFORM=X86_64` — existing tests + network tests
- `make run PLATFORM=X86_64` — interactive shell with `ping`, `ifconfig`
- QEMU user-mode networking provides a virtual gateway at 10.0.2.2

---

## Phase 3: Pi 5 Networking — LANDED (#202)

**Status:** ✅ Landed on `pi5-genet-driver` branch, tracked in #202.

**Key hardware correction during bring-up:** the Pi 5's MAC is
**Cadence MACB/GEM** inside the RP1 southbridge, NOT Broadcom GENET
(that was Pi 4). Linux identifies it as
`compatible = "raspberrypi,rp1-gem", "cdns,macb"`. The driver file
and register layout reflect this; the initial `genet.c` scaffolding
was renamed to `kernel/drivers/macb.c`.

### 3.1 Cadence MACB/GEM Driver

**Controller:** Cadence GEM (idnum 0x0007 per MACB_MID) in the RP1
southbridge.
**MMIO base:** `0x1F00100000` (RP1 BAR1 offset 0x100000). No extra
vmm mapping needed — same 2 MB block as UART.
**PHY:** BCM54213PE on MDIO address 1, reset gated by RP1 GPIO 32.
**Reference driver:** Linux `drivers/net/ethernet/cadence/macb*.c`
(~5000 lines; the `raspberrypi_rp1_config` entry is stock MACB plus
config flags — no Pi-5-specific code path). Cached locally at
`~/slmos-ref/linux/linux-cadence-macb.h` and `linux-cadence-macb-main.c`.

**Landed driver:** `kernel/drivers/macb.c` (~1200 lines) implements:
1. RP1 clock enable (`CLK_ETH_CTRL`, `CLK_ETH_TSU_CTRL`)
2. MACB MID probe + MDIO bring-up (NCFGR CLK div, NCR.MPE)
3. BCM54213PE PHY reset release via RP1 GPIO 32
4. PHY auto-negotiate, link-up detection (BMSR, double-read to
   clear the IEEE 802.3 latched-low LSTATUS)
5. Speed/duplex application (real ANEG decode over MII_STAT1000
   and MII_LPA — 10/100/1000 all supported) + NCFGR.BIG +
   NCFGR.DRFCS for real-world frame acceptance
6. MAC address via 3-tier source (#250, #255): VC mailbox
   (tag 0x00010003, needs EEPROM >= 2025-05-08) → DTB
   `local-mac-address` (works on every EEPROM) → fixed
   `02:00:00:5A:00:01` with WARN. Programmed into SA1B/SA1T.
7. 16-slot TX + RX descriptor rings **in NC memory** (avoids
   8-descriptors-per-cacheline false sharing), with polled
   completion and 2-phase locking around `macb_tx_one` (claim
   slot under `tx_lock`, unlock before the busy-wait, reacquire
   to advance `tx_tail` — closes the yield-with-interrupts-
   disabled bug that would have deadlocked a second `macb_send`
   caller on CPU 0)
8. Cache clean/invalidate at every DMA sync point for buffers
   (mandatory on Pi 5 — no SMPEN); ring accesses are plain dsb
   because NC memory is non-cacheable

**Polling is the operational path**, but not for the reason originally
assumed. The timer PPI-30 blocker (#134) is a separate policy issue;
peripheral IRQs via RP1 MSI-X → MIP0 → GIC SPI should work through
a different mechanism. The MACB driver wires up that path in full —
`gic_register_handler(166)` + `MACB_IER` unmask + `RP1_MSIX_CFG[vec 6]`
programmed — and hardware testing confirms MIP0 sees the MAC
asserting, but RP1's MSIX_CFG engine doesn't fire TLPs on peripheral
IRQ assertion. Same blocker as UART RX; shared tracker at **#247**.
The IRQ code is dormant but correct — it becomes live the day the
MSIX_CFG issue is solved. The `macbdiag` shell command surfaces the
live state.

### 3.2 Hardware-verified (pi-5-1)

End-to-end: boot clean, `net init`, DHCP bound `192.168.4.215`,
ICMP echo 3/4 (first timeout = ARP resolve) at 2-3 ms RTT against
the lab gateway. Five consecutive boots clean at 8.4 s each.

See `docs/networking.md` §"Cadence MACB/GEM Driver (Pi 5)" for
the full driver walkthrough and register addresses.

---

## Phase 4: Jetson Networking (Hardware Required) — Partially landed

**Status:** Jetson networking is no longer blocked. The shipped path is
USB CDC-ECM over the Tegra XHCI host, validated on `jetson-nano-2`
with the Realtek hub + downstream RTL8153 already attached before
`kexec`. The remaining Jetson work is now split between broader support
for that USB path and the still-unlanded internal-RJ45 / PCIe route.

### 4.1 Shipped path — USB CDC-ECM over the Tegra XHCI host (#266)

What landed:
1. Linux-side `slmos-kexec` helper preserves the required XUSB/XHCI
   state and publishes retained-slot handoff data into the next boot.
2. SLM-OS adopts the retained root-hub topology, enumerates the
   downstream RTL8153 via CDC-ECM, and brings up lwIP with DHCP and
   ping without manual unplug/replug.
3. CDC-ECM link readiness is now gated on the notification endpoint's
   `NETWORK_CONNECTION` signal, so the first DHCP attempt no longer
   depends on a retry race after `kexec`.
4. Host-driven smoke coverage exists for repeated Linux → `kexec` →
   SLM-OS networking checks on the lab Jetson path.

For the detailed bring-up history and dead-end investigations, see the
archived record at `docs/archive/plans/jetson-usb-networking-plan.md`.

### 4.2 Remaining Jetson path — internal RJ45 via PCIe RTL8168 (#25)

The Super Developer Kit carrier in the lab routes the RJ45 through a
PCIe RTL8168 behind Tegra PCIe root complex C8, not through an active
EQOS path. That PCIe path remains future work.

Current status:
- driver scaffolding and diagnostics from the earlier investigation are
  still useful infrastructure
- standalone bare-metal access to the RC/NIC still depends on post-kexec
  clock / bring-up behavior
- USB networking removed the immediate product need, but the PCIe route
  remains useful for a self-contained internal Ethernet path

See `docs/jetson-pcie-investigation.md` for the evidence trail and
remaining options.

### 4.3 Jetson follow-on work after the shipped USB path

The shipped USB path is intentionally narrow. Remaining follow-on work:
- **#385** — broaden the validation matrix beyond the current one-tier
  lab topology and single validated adapter chain
- **#386** — Jetson xHCI robustness follow-ups that improve recovery and
  reduce reliance on bring-up-era diagnostics
- **#387** — generalize USB networking beyond the current Jetson
  CDC-ECM path
- **#384** — broader USB host topology and class-driver support beyond
  the current one-tier retained-root-hub flow

This work is about turning a validated lab topology into a broader USB
host/networking capability, not re-solving the original Jetson bring-up.

---

## Dependency Graph

```
Phase 1 (no HW)          Phase 2 (no HW)
┌──────────────────┐     ┌──────────────────┐
│ 1.1 Un-gate lwIP │────▶│ 2.1 QEMU net dev │
│ 1.2 Driver iface │────▶│ 2.2 VirtIO PCI   │
│ 1.3 sys_arch fix │     │ 2.3 Testing      │
└──────────────────┘     └──────────────────┘
         │
         ├──────────────────────────────────────┐
         ▼                                      ▼
Phase 3 (Pi 5 HW)                   Phase 4 (Jetson HW)
┌──────────────────┐                ┌──────────────────┐
│ 3.1 Cadence GEM  │                │ 4.1 USB CDC-ECM  │
│ 3.2 HW testing   │                │ 4.2 USB follow-on│
└──────────────────┘                │ 4.3 PCIe RTL8168 │
                                    └──────────────────┘
```

Phases 1 and 2 are independent of hardware and can be completed
entirely in QEMU. Phase 3 depends on the driver interface and real Pi 5
hardware. Phase 4 now splits into a shipped Jetson USB path plus
follow-on work on broader USB support and the separate internal-RJ45
PCIe route.

---

## Deliverables Per Phase

| Phase | Platform | Deliverable | Hardware? |
|-------|----------|-------------|-----------|
| 1.1 | All | lwIP compiles for all platforms with NETWORKING=ON | No |
| 1.2 | All | `struct net_driver` interface, VirtIO-Net refactored | No |
| 1.3 | All | `sys_arch.c` SMP-safe, timer verified on x86-64 | No |
| 2.1 | x86-64 | QEMU launches with VirtIO-Net PCI device | No |
| 2.2 | x86-64 | VirtIO-Net PCI driver, full networking in QEMU | No |
| 2.3 | x86-64 | `ping`, `ifconfig`, `netstat` working in x86-64 QEMU | No |
| 3.1 | Pi 5 | Cadence MACB/GEM driver, networking on real Pi 5 | Yes |
| 3.2 | Pi 5 | Hardware-validated ping, DHCP, link status | Yes |
| 4.1 | Jetson | USB CDC-ECM networking via retained XHCI/root-hub handoff (#266) | Yes |
| 4.2 | Jetson | Broaden and harden the shipped USB networking path (#385, #386, #387, #384) | Yes |
| 4.3 | Jetson | Optional future internal RJ45 path via PCIe RTL8168 (#25) | Yes |

---

*Created: 15 April 2026. Jetson section re-scoped 25 April 2026 after
 the USB CDC-ECM path landed: Jetson networking is no longer "blocked"
 as a whole, but splits into a shipped USB path plus follow-on USB
 generalization and the separate internal-RJ45 / PCIe route.*
