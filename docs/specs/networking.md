# Networking — Fact Sheet

TCP/IP networking: NIC driver, stack integration, shell-visible results.

## Matrix

| Sub-capability | QEMU (ARM64) | QEMU (x86-64) | Pi 5 | Jetson | x86-64 HW |
|---|---|---|---|---|---|
| Wire interface | virtio-mmio | virtio-pci | Cadence MACB/GEM via RP1 | — (see Skipped) | virtio-pci (QEMU); Realtek RTL8168 (#243) planned for HW |
| Driver file | `kernel/drivers/virtio_net.c` | `kernel/drivers/virtio_net_pci.c` | `kernel/drivers/macb.c` | — | same as QEMU x86-64 for now |
| Driver abstraction | `struct net_driver` | `struct net_driver` | `struct net_driver` | — | `struct net_driver` |
| IP stack | lwIP | lwIP | lwIP | — | lwIP |
| IRQ model | MMIO IRQ | MSI-X + PCI IRQ | Polled (#247) | — | MSI-X |
| DHCP | ✅ auto at boot | ✅ | ✅ | — | ✅ |
| ping | ✅ | ✅ | ✅ 2-4 ms RTT | — | ✅ |
| `ifconfig` | ✅ | ✅ | ✅ | — | ✅ |
| `netstat` | ✅ | ✅ | ✅ | — | ✅ |
| TCP shell (raw, port 2323) | ✅ | ✅ | ✅ | — | ✅ |
| Telnet protocol (RFC 854 + ECHO/SGA/NAWS/TERMINAL-TYPE/IP) | ✅ | ✅ | ✅ | — | ✅ |
| `telnetd` daemon control (`start/stop/status/sessions/kick`) | ✅ | ✅ | ✅ | — | ✅ |
| `/etc/telnetd.conf` + `NET_TELNETD_AUTOSTART` build flag | ✅ | ✅ | ✅ | — | ✅ |
| `slm.telnetd_*` Lua bindings | ✅ | ✅ | ✅ | — | ✅ |
| SSH | ⏸️ (#199) | ⏸️ | ⏸️ | — | ⏸️ |

## Skipped / Blocked

- **#25 — Jetson EQOS driver.** Target hardware identified (Synopsys DWC-EQOS + RTL8211F PHY via RGMII/MDIO — see `docs/networking-expansion-plan.md` §4.1), no code written yet. First experiment before committing to the driver: CBB-probe EQOS MMIO at `0x02310000` from NS EL2.
- **#266 — Jetson USB networking** (fallback for #25). Plan written at `docs/jetson-usb-networking-plan.md`. Option A (USB-A CDC-ECM dongle via XHCI host) is primary; Option B (USB-C device mode CDC-ECM gadget via XUDC) is fallback. Not started; gated on Phase 0 CBB probe of XHCI/XUDC.
- **#243 — x86-64 Realtek RTL8168/8111 driver for bare-metal.** Works under QEMU x86-64 (virtio-pci); the test-pc dev board has a Realtek NIC that would need a native driver. Not blocking the capstone narrative because QEMU x86-64 demonstrates the full stack.
- **#247 — Pi 5 RP1 MSIX_CFG engine doesn't fire TLPs.** MACB IRQ handler is registered but never runs. MACB falls back to polling (same pattern as UART RX). Functional at 2-4 ms RTT; not a correctness issue. Affects every RP1 peripheral.
- **Unauthenticated telnet on Pi 5** — Pi 5 lab/demo builds now default `NET_TELNETD_AUTOSTART=ON`, but an explicit `-DNET_TELNETD_AUTOSTART=OFF` still wins. That is an operator choice for trusted networks, not a security claim; SSH/authentication (#199) is still the real hardening path.
- **Jetson networking over the internal Ethernet** — requires solving #25 OR #266 OR taking a permanent CBB bypass route (`docs/jetson-cbb-report.md` §6).
- **IPv6** — lwIP config option; not compiled in. Not a project priority.
- **TLS** — not in-tree; follows after #199 SSH.

## Capabilities delivered

- Single `struct net_driver` abstraction plugs any NIC into lwIP — proven with virtio-mmio, virtio-pci, and MACB.
- Auto-DHCP at boot with static-IP fallback (#197).
- Live integration tests covering init, TX, RX, DHCP BOUND and FAILED states.
- Multi-session TCP shell via lwIP raw callbacks (Phase 1).
- Full telnet protocol: IAC, ECHO, SGA, NAWS, TERMINAL-TYPE, IAC IP→Ctrl+C. `telnet localhost 2323` gives character-at-a-time server-echoed mode (Phase 2).
- `telnetd` daemon controls — `start/stop/status/sessions/kick` shell commands, `/etc/telnetd.conf` parser, `NET_TELNETD_AUTOSTART` build flag, `slm.telnetd_*` Lua bindings (Phase 3).

## See also

- `docs/networking.md` (narrative)
- `docs/networking-expansion-plan.md` (phased plan)
- `docs/net-driver-checklist.md` (driver implementor's checklist)
- `docs/net-dma-coherence.md` (DMA coherence model)
- `docs/multi-session-shell-plan.md` (TCP shell + telnet + telnetd)
- `docs/jetson-usb-networking-plan.md` (USB networking option)
- `docs/archive/plans/capstone-feature-status.md` §"Networking"

*Last updated: 22 April 2026*
