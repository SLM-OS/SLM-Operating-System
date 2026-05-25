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
| SSH daemon (port 2222, `sshd`) — wolfSSH 1.4.18 + wolfCrypt 5.7.4 | ✅ | ✅ | ✅ (autostart ON) | ✅ (autostart ON) | ✅ |
| SSH crypto: curve25519-sha256 KEX, Ed25519 host key, AES-256-GCM | ✅ | ✅ | ✅ | ✅ | ✅ |
| SSH password auth (scrypt N=2^15) + bootstrap gate | ✅ | ✅ | ✅ | ✅ | ✅ |
| Persisted Ed25519 host key at `/mnt/files/etc/ssh/host_ed25519_key` | ✅ | ✅ | ✅ | ✅ | ✅ |
| Per-session shell channel → existing REPL (same path as UART / telnet) | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/etc/sshd.conf` + `NET_SSHD_AUTOSTART` build flag | ✅ | ✅ | ✅ | ✅ | ✅ |

## Skipped / Blocked

- **#25 — Jetson internal Ethernet path.** On the Super Developer Kit in the lab, the onboard RJ45 routes through a PCIe RTL8168 behind Tegra PCIe root complex C8, not an active EQOS path. No SLM-OS driver landed yet; see `docs/jetson-pcie-investigation.md` and `docs/archive/plans/networking-expansion-plan.md`.
- **#266 — Jetson USB networking** is the shipped Jetson NIC path. Current path is USB CDC-ECM on the validated `jetson-nano-2` lab topology: retained root hub plus downstream RTL8153 after `kexec`. Generic multi-device USB-host work tracked in #384.
- **#384 — General USB host support beyond the current Jetson NIC path.** Follow-on to the landed Jetson CDC-ECM path: generic multi-device topology, hub traversal, hotplug, alternate settings, and class binding. Plan written at `docs/archive/plans/usb-host-generalization-plan.md`.
- **#243 — x86-64 Realtek RTL8168/8111 driver for bare-metal.** Works under QEMU x86-64 (virtio-pci); the test-pc dev board has a Realtek NIC that would need a native driver. Not blocking the capstone narrative because QEMU x86-64 demonstrates the full stack.
- **#247 — Pi 5 RP1 MSIX_CFG engine doesn't fire TLPs.** MACB IRQ handler is registered but never runs. MACB falls back to polling (same pattern as UART RX). Functional at 2-4 ms RTT; not a correctness issue. Affects every RP1 peripheral.
- **Unauthenticated telnet on Pi 5 + Jetson** — Pi 5 and Jetson lab/demo builds default `NET_TELNETD_AUTOSTART=ON`, but an explicit `-DNET_TELNETD_AUTOSTART=OFF` still wins. That's an operator choice for trusted networks, not a security claim; SSH (#199, shipped) is the authenticated path — `ssh -p 2222 root@<board-ip>` after a one-time `adduser` on the console.
- **Jetson networking over the internal Ethernet** — still requires solving #25. The current shipped Jetson networking path is USB CDC-ECM, not the internal RJ45.
- **IPv6** — lwIP config option; not compiled in. Not a project priority.
- **TLS** — not in-tree. wolfSSL is vendored under `kernel/lib/wolfcrypt/` (it backs wolfSSH) but the TLS-side surface isn't enabled in `user_settings.h`. A future ticket can flip the wolfSSL `WOLFSSL_NO_TLS12 / 13` knobs on if a TLS consumer materialises.
- **SSH public-key authentication** — out of #199 scope per the documented non-goals. Password auth + scrypt is the shipped surface; pubkey auth is a follow-up if/when a multi-user deployment needs it.

## Telemetry feed (`telemetryd`)

Push-only TCP server on port 2325 bridging in-process `tel.*` msg_router topics out to the network. Wire format: newline-terminated `<topic> seq=<n> ts=<ms> <payload>`. Per-client server-side glob filter (default `tel.*`) re-settable via the `SUB <pattern>` command on the same connection. Slow-client back-pressure is per-client drop-oldest. Controls: `telemetry server start|stop|status|sessions|kick`, `slm.telemetryd_*` Lua bindings, `NET_TELEMETRYD_AUTOSTART` build flag.

## SSH daemon (`sshd`)

Authenticated network shell. Port 2222 default (operator-overridable via `sshd start <port>` or `/mnt/files/etc/sshd.conf`). Same REPL the UART + telnet paths use, bound to the wolfSSH stream via `kernel/net/ssh/shell_io_ssh.c`.

| Layer | Choice | Source |
|---|---|---|
| KEX | curve25519-sha256 | wolfSSH 1.4.18 |
| Host key | Ed25519, persisted to `/mnt/files/etc/ssh/host_ed25519_key` | `kernel/net/ssh/host_key.c` |
| Cipher | AES-256-GCM (RFC 5647) | wolfCrypt 5.7.4 |
| Password KDF | scrypt N=2^15, r=8, p=1 (RFC 7914) | wolfCrypt's `wc_scrypt` |
| Allocator | dedicated 48 MB PMM-backed pool (scrypt needs 32 MB) | `kernel/net/ssh/wolf_heap.c` |
| Entropy floor | RNDR / RDRAND / SHA-256-mixed jitter pool | `kernel/src/rng.c` |

**Bootstrap gate.** `sshd_userauth_passwd` refuses every connection until at least one user is provisioned. A boot with `NET_SSHD_AUTOSTART=ON` and an empty `/mnt/files/etc/passwd` exposes only a listening port that rejects logins — there's no exploit window.

```
slmos> adduser root <password>          # bootstrap on the console
slmos> sshd status                      # confirm listener is up
slmos> sshd fingerprint                 # SHA256:base64 — pin client-side
slmos> sshd regenerate-host-key         # compromise recovery (rare)

# From a workstation, after the console-side adduser:
$ ssh -p 2222 root@<board-ip>
```

**Demo defaults.** Lab images for Pi 5 and Jetson Orin Nano ship with `NET_SSHD_AUTOSTART=ON`; QEMU and x86-64 default to OFF so CI images stay quiet. `NET_SSHD_DEMO_ALLOW_ALL` (default OFF since #199d) re-enables the wolfSSH-accepts-every-password stub for debugging.

Detailed design + threat model: [`docs/ssh.md`](../ssh.md). Security audit: [`docs/security.md`](../security.md) §SSH.

## See also

- `docs/networking.md` (narrative)
- `docs/archive/plans/networking-expansion-plan.md` (phased plan)
- `docs/net-driver-checklist.md` (driver implementor's checklist)
- `docs/net-dma-coherence.md` (DMA coherence model)
- `docs/design/admin-telemetry-suite.md` §"Network feed" (telemetryd wire protocol)
- `docs/archive/plans/usb-host-generalization-plan.md` (general USB host follow-on after the current CDC-ECM path)

*Last updated: 26 April 2026*
