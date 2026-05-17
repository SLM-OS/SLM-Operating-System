# Networking — Fact Sheet

TCP/IP networking: NIC driver, stack integration, shell-visible results.

## Matrix

| Sub-capability | QEMU (ARM64) | QEMU (x86-64) | Pi 5 | Jetson | x86-64 HW |
|---|---|---|---|---|---|
| Wire interface | virtio-mmio | virtio-pci | Cadence MACB/GEM via RP1 | USB CDC-ECM over retained Tegra XHCI root-hub handoff | virtio-pci (QEMU); Realtek RTL8168 (#243) planned for HW |
| Driver file | `kernel/drivers/virtio_net.c` | `kernel/drivers/virtio_net_pci.c` | `kernel/drivers/macb.c` | `kernel/usb/class/cdc_ecm.c` + `kernel/drivers/usb/xhci/*` | same as QEMU x86-64 for now |
| Driver abstraction | `struct net_driver` | `struct net_driver` | `struct net_driver` | `struct net_driver` | `struct net_driver` |
| IP stack | lwIP | lwIP | lwIP | lwIP | lwIP |
| IRQ model | MMIO IRQ | MSI-X + PCI IRQ | Polled (#247) | xHCI event ring + polled net pump | MSI-X |
| DHCP | ✅ auto at boot | ✅ | ✅ | ✅ | ✅ |
| ping | ✅ | ✅ | ✅ 2-4 ms RTT | ✅ | ✅ |
| `ifconfig` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `netstat` | ✅ | ✅ | ✅ | ✅ | ✅ |
| TCP shell (raw, port 2323) | ✅ | ✅ | ✅ | ✅ | ✅ |
| Telnet protocol (RFC 854 + ECHO/SGA/NAWS/TERMINAL-TYPE/IP) | ✅ | ✅ | ✅ | ✅ | ✅ |
| `telnetd` daemon control (`start/stop/status/sessions/kick`) | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/etc/telnetd.conf` + `NET_TELNETD_AUTOSTART` build flag | ✅ | ✅ | ✅ | ✅ | ✅ |
| `slm.telnetd_*` Lua bindings | ✅ | ✅ | ✅ | ✅ | ✅ |
| Telemetry feed TCP server (port 2325, `telemetryd`) | ✅ | ✅ | ✅ | ✅ | ✅ |
| `slm.telemetryd_*` Lua bindings | ✅ | ✅ | ✅ | ✅ | ✅ |
| SSH | ⏸️ (#199) | ⏸️ | ⏸️ | ⏸️ | ⏸️ |

## Skipped / Blocked

- **#25 — Jetson internal Ethernet path.** On the Super Developer Kit in the lab, the onboard RJ45 routes through a PCIe RTL8168 behind Tegra PCIe root complex C8, not an active EQOS path. No SLM-OS driver landed yet; see `docs/jetson-pcie-investigation.md` and `docs/networking-expansion-plan.md`.
- **#266 — Jetson USB networking** is the shipped Jetson NIC path. Current path is USB CDC-ECM on the validated `jetson-nano-2` lab topology: retained root hub plus downstream RTL8153 after `kexec`. Generic multi-device USB-host work tracked in #384.
- **#384 — General USB host support beyond the current Jetson NIC path.** Follow-on to the landed Jetson CDC-ECM path: generic multi-device topology, hub traversal, hotplug, alternate settings, and class binding. Plan written at `docs/archive/plans/usb-host-generalization-plan.md`.
- **#243 — x86-64 Realtek RTL8168/8111 driver for bare-metal.** Works under QEMU x86-64 (virtio-pci); the test-pc dev board has a Realtek NIC that would need a native driver. Not blocking the capstone narrative because QEMU x86-64 demonstrates the full stack.
- **#247 — Pi 5 RP1 MSIX_CFG engine doesn't fire TLPs.** MACB IRQ handler is registered but never runs. MACB falls back to polling (same pattern as UART RX). Functional at 2-4 ms RTT; not a correctness issue. Affects every RP1 peripheral.
- **Unauthenticated telnet on Pi 5 + Jetson** — Pi 5 and Jetson lab/demo builds default `NET_TELNETD_AUTOSTART=ON`, but an explicit `-DNET_TELNETD_AUTOSTART=OFF` still wins. That is an operator choice for trusted networks, not a security claim; SSH/authentication (#199) is still the real hardening path.
- **Jetson networking over the internal Ethernet** — still requires solving #25. The current shipped Jetson networking path is USB CDC-ECM, not the internal RJ45.
- **IPv6** — lwIP config option; not compiled in. Not a project priority.
- **TLS** — not in-tree; follows after #199 SSH.

## Telemetry feed (`telemetryd`)

Push-only TCP server on port 2325 bridging in-process `tel.*` msg_router topics out to the network. Wire format: newline-terminated `<topic> seq=<n> ts=<ms> <payload>`. Per-client server-side glob filter (default `tel.*`) re-settable via the `SUB <pattern>` command on the same connection. Slow-client back-pressure is per-client drop-oldest. Controls: `telemetry server start|stop|status|sessions|kick`, `slm.telemetryd_*` Lua bindings, `NET_TELEMETRYD_AUTOSTART` build flag.

## See also

- `docs/networking.md` (narrative)
- `docs/networking-expansion-plan.md` (phased plan)
- `docs/net-driver-checklist.md` (driver implementor's checklist)
- `docs/net-dma-coherence.md` (DMA coherence model)
- `docs/design/admin-telemetry-suite.md` §"Network feed" (telemetryd wire protocol)
- `docs/archive/plans/usb-host-generalization-plan.md` (general USB host follow-on after the current CDC-ECM path)

*Last updated: 26 April 2026*
