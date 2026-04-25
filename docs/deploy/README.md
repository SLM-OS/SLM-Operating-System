# Deploying SLM-OS to Target Hardware

This directory contains step-by-step deploy guides for each supported hardware target. Each guide covers **first-time provisioning** (partitioning, bootloader configuration, firmware prerequisites) as well as **iterative kernel updates**.

For build instructions, see `docs/getting-started.md`. For interactive lab operations once a board is running, see `docs/lab-operations.md`.

---

## Pi 5 Deployment Models

Pi 5 boards are intentionally supported in **two deployment models** and
the project should preserve both:

- **SDWire-first deploy model**:
  - the boot card is exposed to the host through lab-managed SDWire
  - iterative development updates the SLM-OS boot artifact directly
  - this is the primary fast lab workflow when hardware supports it
- **Maintenance-OS dual-boot model**:
  - the card carries both SLM-OS and a Linux maintenance environment
  - Linux is used for recovery, local kernel replacement, and one-shot
    handoff into SLM-OS when no SDWire exists
  - this is the supported workflow for boards such as `pi-5-2`

These are complementary, not competing. SDWire is the fastest deploy
path where available; dual boot is the resilient no-SDWire path.

For file staging onto a running SLM-OS instance, see
[`slm-put.md`](slm-put.md) for the first telnet-based host upload path.
[`slm-modelctl.md`](slm-modelctl.md) for the first upload + load +
activate wrapper around runtime model blobs.

---

## Target × Media Matrix

| Target | SD card | NVMe / SSD | USB drive | Notes |
|---|---|---|---|---|
| Raspberry Pi 5 | [`pi5-sdcard.md`](pi5-sdcard.md), [`pi5-sdwire.md`](pi5-sdwire.md), [`../pi5-dual-boot-setup.md`](../pi5-dual-boot-setup.md) | **Gap** (#TBD) | — | Preserve both Pi 5 models: SDWire-first deploy where hardware supports it, and maintenance-OS dual boot where it does not. |
| Jetson Orin Nano | [`jetson-kexec.md`](jetson-kexec.md) (kexec from Linux on the SD rootfs) | [`jetson-kexec.md`](jetson-kexec.md) (kexec from Linux on the SSD rootfs) | — | No standalone bare-metal boot path yet — UEFI-direct is WIP (see `docs/archive/investigations/jetson-uefi-direct-result.md`). |
| x86-64 (test-pc) | — | [`x86-64-ssd.md`](x86-64-ssd.md) | **Gap** (#TBD) | UEFI disk image written to SSD via SDWire; kexec-from-Linux is WIP (see `docs/x86-64-gpu-inference-status.md` §4.2.k). |
| QEMU (ARM64 / x86-64) | N/A | N/A | N/A | `make run` — no deploy step. See `docs/getting-started.md`. |

Gaps marked above represent paths that are known to work in principle but have no written procedure. File a tracking issue before attempting to fill one in.

---

## Conventions

### Hardware access

All lab hardware is managed by **labctl** (Embedded Lab Control). SD cards, power, and serial are accessed through labctl. The main exception is **sneakernet** — when a board lacks an SDWire interface (for example `pi-5-2`), the SD card may need to be physically removed and written via the host machine's built-in card reader. Some Pi 5 cards also keep a local Raspberry Pi OS maintenance install on the same card; that maintenance OS is part of the supported dual-boot model and can update `kernel_2712.img` in place when it is reachable via SSH or an exclusive serial/login path.

Never bypass labctl by running `mount`/`cp`/`picocom`/`nc` directly against lab-managed SD cards or serial ports — doing so risks device conflicts with other sessions and can corrupt the wrong card. Sneakernet is the one sanctioned exception.

### Provisioning vs. updates

Each guide has two sections:

- **First-time provisioning** — creating the partition table, installing Pi firmware / EFI loader / GRUB, writing a bare-metal `config.txt` or equivalent. Done once per card/disk.
- **Kernel update** — rebuilding `slmos.bin` / `slmos.elf` / `slmos-x86.img` and writing just the kernel artifact to an already-provisioned medium. Done many times per day during development.

Iterative development uses the update path. Full provisioning is rare — typically only when a card is wiped, a new SBC is added, or the boot config schema changes.

### Post-deploy verification

Every guide ends with a serial-console verification step. Successful boot means the shell prompt is reachable, not just "no obvious crash message" — the shell is the hand-off point from boot/HAL to userland.

---

## Writing new deploy guides

When adding a guide for a new target × media combination:

1. Start from the closest existing guide as a template.
2. Document first-time provisioning in full — assume the reader has only a blank SD/SSD and a fresh checkout.
3. Call out firmware prerequisites (EEPROM version on Pi 5, UEFI firmware settings on x86, L4T version on Jetson). Where a known-bad version exists, link to the tracking issue.
4. Include the exact `config.txt` / `extlinux.conf` / `grub.cfg` used. Copy-paste sources of truth are more reliable than prose descriptions.
5. Preserve any existing OS on the target medium with a `.bak` extension wherever feasible, so the card/disk can be reverted.

---

*Maintenance: update the matrix above whenever a new guide is added or a gap is closed. Individual guides should stand alone — this README is a dispatch table, not a primer.*
