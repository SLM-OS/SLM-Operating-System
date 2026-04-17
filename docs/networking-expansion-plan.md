# Networking Expansion Plan

Extend networking from QEMU-only to all four supported platforms.

**Current state (April 2026):** Phases 1 and 2 have **landed**. Full
TCP/IP networking (lwIP + VirtIO-Net) works on both QEMU ARM64 and
QEMU x86-64 via the `net_driver` abstraction. Pi 5 (#202) and Jetson
(#25) still need platform-specific NIC drivers — Phases 3 and 4
below, blocked only on hardware access.

Landed work summary (commits on branch `worktree-networking-no-hw`):
- `struct net_driver` abstraction (`kernel/include/net_driver.h`)
- x86-64 VirtIO-Net PCI driver (`kernel/drivers/virtio_net_pci.c`)
- `ENABLE_NETWORKING` CMake option (default ON for QEMU_VIRT and X86_64)
- Auto-DHCP at boot with static-IP fallback (closes #197)
- Live integration tests covering init, TX, RX, DHCP BOUND and FAILED

Follow-up tickets filed:
- #200 — Scheduler + integration test flakiness (pre-existing, observed during this work)
- #201 — Shell message when DHCP binds
- #202 — Pi 5 BCM GENET driver (Phase 3 below)
- #203 — DMA coherence verification for real-hardware NICs
- #25 (updated) — Jetson EQOS driver (Phase 4 below)

**Last updated:** 17 April 2026

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
  PLATFORM=RASPI5     → (future: bcmgenet.c)
  PLATFORM=JETSON     → (future: rtl8169.c or usb_net.c)
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
`docs/reference/linux-cadence-macb.h` and `linux-cadence-macb-main.c`.

**Landed driver:** `kernel/drivers/macb.c` (~700 lines) implements:
1. RP1 clock enable (`CLK_ETH_CTRL`, `CLK_ETH_TSU_CTRL`)
2. MACB MID probe + MDIO bring-up (NCFGR CLK div, NCR.MPE)
3. BCM54213PE PHY reset release via RP1 GPIO 32
4. PHY auto-negotiate, link-up detection (BMSR)
5. Speed/duplex application + NCFGR.BIG + NCFGR.DRFCS for real-
   world frame acceptance
6. Locally-administered MAC address (02:00:00:5A:00:01) programmed
   into SA1B/SA1T
7. 16-slot TX + RX descriptor rings, polled completion
8. Cache clean/invalidate at every DMA sync point (mandatory on
   Pi 5 — no SMPEN)

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

## Phase 4: Jetson Networking (Hardware Required — tracked in #25)

The Jetson Orin Nano Developer Kit carrier board wires its RJ45 directly
to the Tegra T234 SoC via MDI differential pairs — there is **no PCIe
NIC** on the Ethernet path. The data path is:

```
RJ45 ↔ MDI ↔ RTL8211F PHY (RGMII + MDIO) ↔ Tegra EQOS MAC (MMIO)
```

Authoritative source: *Jetson Orin Nano Developer Kit Carrier Board
Specification* SP-11324-001, Table 2-4 ("Ethernet RJ45 Connector Pin
Description") — RJ45 pins wire to module pins `GPE_MDI0_P/N` through
`GPE_MDI3_P/N`. No PCIe TX/RX pairs reach the RJ45.

An earlier draft of this plan mistakenly identified the external PHY
(RTL8211F) as a PCIe NIC (RTL8111) and targeted Linux's `r8169` driver.
That framing is wrong and has been removed. The correct target is the
Synopsys DWC-EQOS IP block, as tracked in #25.

### 4.1 Target: Synopsys DWC-EQOS MAC + RTL8211F PHY

**Controller (MAC):** Synopsys DesignWare Cores Ethernet QoS (DWC-EQOS)
**Discovery:** Memory-mapped at a fixed SoC address — NOT enumerated via
PCI. Orin's EQOS base is in the `0x023_0_0000` region (confirm exact
offset from the Orin TRM / NVIDIA device tree
`tegra234-p3768-0000+p3767-0003.dts` before writing the driver).
**PHY:** Realtek RTL8211F, 1000BASE-T, accessed via MDIO from the MAC.
**Reference drivers:** Linux `drivers/net/ethernet/stmicro/stmmac/`
(specifically `dwmac4_core.c`, `dwmac4_descs.c`, `dwmac4_dma.c`) and
NVIDIA's `nvethernet` downstream fork in L4T.

**Minimum viable driver (~1200-1800 lines):**
1. Clock / reset gating for the EQOS block (CAR register writes — Orin
   TRM) — may already be set up by the Linux bootloader pre-kexec.
2. MMIO mapping of EQOS register bank into kernel virtual space.
3. Small MDIO helper for PHY access (Clause 22 frames over the EQOS
   MAC's MDIO controller).
4. PHY bring-up: RTL8211F reset, auto-negotiation, link-up poll,
   speed/duplex read-back.
5. RGMII interface config on the MAC side matched to the PHY link
   state (speed, duplex, TX/RX delay — RTL8211F needs specific
   internal-delay programming).
6. TX/RX descriptor rings in DMA-coherent memory — use NC memory on
   Jetson to match the `net-dma-coherence.md` guidance.
7. IRQ wiring to GIC SPI (confirm EQOS IRQ numbers from DT).
8. Integrate as lwIP netif (analogous to `virtio_net.c`).

**Not required:**
- PCIe root complex enumeration on Jetson (was the scariest item in the
  earlier draft — it's moot because EQOS is not on PCIe).
- Any Tegra PCIe controller init for Ethernet purposes (PCIe on Orin is
  still relevant for future M.2 NVMe / AI-accel boards, but that's a
  separate workstream).

### 4.2 Fallback: USB Ethernet

If EQOS bring-up stalls (e.g., clock/reset init needs firmware sequences
that aren't accessible from EL2), a USB CDC-ECM Ethernet adapter on the
dev kit's USB-A port is a viable fallback.

Total USB stack effort (~2000 lines XHCI + ~500 lines CDC-ECM + USB core)
is larger than EQOS but avoids any SoC-level clock gymnastics. Only
revisit this path after a concrete EQOS blocker is identified.

### 4.3 Testing (Hardware Only)

- Deploy via `labctl sdwire_update`, test via `labctl serial_send`
- Connect Jetson Ethernet to lab network
- `ping`, `ifconfig`, `dhcp` from SLM-OS shell
- Early smoke test before ring setup: MDIO probe — read PHY ID registers
  from the RTL8211F (expect `0x001C.C916` or similar). Confirms MMIO
  mapping, clock, and MDIO pathway in a few dozen lines of code.

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
│ 3.1 BCM GENET    │                │ 4.1 EQOS+RTL8211F│
│ 3.2 HW testing   │                │ 4.2 (alt: USB)   │
└──────────────────┘                │ 4.3 HW testing   │
                                    └──────────────────┘
```

Phases 1 and 2 are independent of hardware and can be completed
entirely in QEMU. Phases 3 and 4 depend on Phase 1 (driver interface)
and require their respective hardware.

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
| 3.1 | Pi 5 | BCM GENET driver, networking on real Pi 5 | Yes |
| 3.2 | Pi 5 | Hardware-validated ping, DHCP, link status | Yes |
| 4.1 | Jetson | DWC-EQOS MAC + RTL8211F PHY driver (USB CDC-ECM as fallback) | Yes |
| 4.3 | Jetson | Hardware-validated ping, DHCP, link status | Yes |

---

*Created: 15 April 2026*
