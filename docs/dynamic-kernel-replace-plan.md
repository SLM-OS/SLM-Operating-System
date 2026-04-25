# Dynamic Kernel Replace Plan

## Goal

Replace the running SLM-OS kernel on the Pi 5 SD card from within a
running SLM-OS instance, atomically, with rollback, and without
requiring a maintenance OS on the card.

The target demo flow:

1. Admin uploads a new kernel image into the running system.
2. Admin runs `kernel activate`; SLM-OS stages the image to the boot
   partition, arms Pi 5 tryboot, and reboots.
3. Firmware boots the candidate kernel.
4. If the candidate comes up healthy, `kernel promote` installs it as
   the default. If not, the next cold boot automatically reverts to the
   previous kernel.

---

## Relation to Existing Work

The dynamic policy model loading plan
(`docs/dynamic-policy-model-loading-plan.md`) covers **file ingress**
(phases F1–F5): arbitrary files landing in the VFS at `/mnt/files/…`.
That plan stops at "file is on the device."

This plan extends the ingress story to a specific promotion target:
copy an uploaded kernel blob from the VFS into the Pi 5 boot partition
on the SD card and reboot into it safely. The same staging / activate /
rollback shape used for eviction model payloads is reused for the
kernel command surface.

---

## Architecture

Three new capabilities on top of the F-series ingress work.

### 1. SDHCI / EMMC2 Block Driver

- BCM2712 EMMC2 is SDHCI-compatible.
- Legacy 25 MHz SDR is sufficient for admin-speed writes (an image-size
  kernel lands in a few seconds).
- Reference sources: Linux `drivers/mmc/host/sdhci-iproc.c`, u-boot
  BCM2712 board files.
- Scope: CMD0 / CMD2 / CMD3 / CMD7 / CMD8 / CMD9 / CMD16 / CMD17 /
  CMD18 / CMD24 / CMD25 / CMD55 / ACMD41 only. No HS200, no HS400, no
  tuning, no CQE, no TRIM.
- Closes #35 (currently Post-Capstone).

### 2. FAT32 Writer

- Integrate Elm-Chan **FatFs** (permissive license) and wire
  `disk_read` / `disk_write` to the SDHCI driver.
- Parse MBR, parse BPB on partition 1, create / overwrite a file in the
  partition 1 root directory.
- Skip long filename support — `kernel_2712.img`, `tryboot.img`, and
  `config.txt` all fit 8.3.

### 3. Tryboot + Admin Command Surface

- Write the Pi 5 tryboot flag to its persistent reset register, then
  call `psci_system_reset()`. See Risk 2 below for the register itself.
- Ship `config.txt` on the lab card with `[all]` and `[tryboot]`
  sections so the firmware knows what to do with the flag.
- Shell commands (admin-gated, same gate as telnet admin):
  - `kernel status` — current staged / active / previous image state.
    The *running* kernel's identity is reported via the
    `SLMOS_VERSION` / `SLMOS_BUILD_STAMP` / `SLMOS_BUILD_SHA` triple
    from `kernel/include/build_info.h` (issue #360), already exposed
    by the boot banner, `cat /sys/version`, and the `slm.VERSION` /
    `slm.BUILD_STAMP` / `slm.BUILD_SHA` Lua bindings.
  - `kernel stage <vfs-path>` — copy VFS file into SD boot partition as
    `tryboot.img` + compute SHA-256 sidecar
  - `kernel activate` — set tryboot flag, `psci_system_reset()`
  - `kernel promote` — rename `tryboot.img` over `kernel_2712.img`
  - `kernel rollback` — delete `tryboot.img`, clear flag if still set
- State machine: `empty → staged → armed → promoted` (or `rolled_back`).
- State is inferred from which files exist on the FAT partition, so no
  separate persistence layer is needed.

---

## Effort Estimates

Engineering days, solo focused work.

| Piece | Low | High | Notes |
|-------|-----|------|-------|
| SDHCI / EMMC2 driver | 3d | 5d | Legacy SDR only. +1–2 d if VC firmware hands off gated clocks. |
| FAT32 writer (FatFs) | 3d | 5d | Integrate + wire block glue. |
| FAT32 writer (from scratch) | +3d | +3d | Only if FatFs licensing blocks use. |
| Tryboot register RE + write | 1d | 3d | +1–2 d if flag lives behind a VC mailbox RPC and mailbox driver must be added. |
| Staging state machine + commands | 2d | 3d | Mirrors the eviction-model shape. |
| Image validation (magic, size) | 0.5d | 1d | Header check + free-space check. |
| QEMU tests | 1d | 2d | `-drive if=sd` with a FAT image. |
| Hardware round-trip on lab Pi 5 | 2d | 3d | Mostly hardware-loop time. |
| Buffer for Pi 5 surprises | 2d | 4d | BCM2712 delta vs Pi 4, VC firmware opacity, SDWire interactions. |

**Totals**

- Tryboot A/B, demo-quality: **14–26 d** (~3–5 weeks elapsed).
- Unsafe one-shot overwrite (no staging, no rollback): **8–13 d**
  (~2–3 weeks elapsed). Dropping tryboot trades rollback for speed;
  one bad write requires SDWire re-flash.

---

## Risks

### Risk 1 — VC Firmware Handoff State of EMMC2

**Question:** when SLM-OS takes control, is EMMC2 already clocked and
in a usable state, or does the controller need a full re-init from
cold?

- The VideoCore firmware is a closed blob — definitive answer requires
  reading real hardware registers after handoff.
- Partial de-risk without hardware: read the Pi 5 SDHCI driver's probe
  path in Linux mainline to see whether Linux re-inits from CMD0 or
  trusts firmware-left state. This reveals what Linux *assumes*, not
  what firmware *does*, but if Linux re-inits unconditionally, the safe
  play is "SLM-OS re-inits too" and residual risk drops.
- Impact if hit: +1–3 d to add cold-start init and any required
  clock / reset via VC mailbox.

**Hardware required:** partial — code read bounds it, hardware
confirms it.

### Risk 2 — Pi 5 Tryboot Register Mechanism

**Question:** which register holds the tryboot flag on BCM2712, and is
it a bare MMIO write or a VC mailbox RPC?

- On Pi 4 / CM4 the flag is a couple of bits in `PM_RSTS`; any OS can
  write it directly.
- On Pi 5 the reset logic moved. The Linux kernel handler for
  `reboot "0 tryboot"` is open-source and identifies the exact
  register, offset, and value.
- Can be answered entirely from Linux mainline: `drivers/power/reset/`,
  `drivers/watchdog/`, the Pi 5 device-tree at
  `arch/arm64/boot/dts/broadcom/bcm2712*.dts*`.
- Impact if the flag requires a mailbox call and SLM-OS lacks a BCM
  mailbox driver: +2–4 d.

**Hardware required:** no for identification, yes for end-to-end
verification of boot cycle behavior.

---

## Pre-Hardware Tasks

Items that can land before a Pi 5 is free.

- ☐ Trace Pi 5 restart handler in Linux mainline: identify register,
  offset, and magic value for `reboot "0 tryboot"`. Record whether it
  is a direct MMIO write or a VC mailbox RPC (and the tag if mailbox).
- ☐ Read the Pi 5 EMMC2 driver probe path and determine whether Linux
  re-inits from CMD0 or trusts firmware-left state.
- ☐ Check whether SLM-OS already has a BCM VC mailbox driver; if not,
  scope what a minimal one would look like (needed only if Risk 2
  resolves to "mailbox").
- ☐ Confirm Pi 5 boot partition layout the firmware expects (FAT32 on
  MBR partition 1).
- ☐ Identify the minimum Pi 5 bootloader EEPROM version required for
  tryboot support; confirm the lab Pi 5s meet it.

## Hardware Tasks

Blocked until a Pi 5 is available.

- ☐🔗 Build a diagnostic kernel that dumps, at EL2 entry: EMMC2
  controller clock gate state, reset-status register contents,
  controller mode, and CMD0 response. Record what VC firmware leaves
  behind after handoff.
- ☐🔗 Verify that writing the tryboot register identified in the
  pre-hardware trace causes firmware to honor the `[tryboot]` section
  on the next boot. Run via `labctl boot_test --count 10` to catch
  flakes.
- ☐🔗 Implement SDHCI driver against the real card; run read / write /
  verify loops against a scratch FAT image on the second partition (if
  present) or a guarded region of the boot partition.
- ☐🔗 Full round-trip on the lab Pi 5 through labctl: `kernel stage`,
  `kernel activate`, `kernel promote`, `kernel rollback`.

---

## Open Questions

- Does the capstone demo target boot from SD, NVMe, or USB? SDHCI work
  only helps SD-boot targets.
- Should `config.txt` ship with the `[tryboot]` section baked in, or
  should SLM-OS be able to edit it over FAT? Baked in is simpler and
  sufficient for the first cut.
- How is `promote` triggered after a successful tryboot boot? Explicit
  admin command is simplest. Automatic promotion on a heartbeat would
  be a follow-up.

---

## Dependencies

- Phase F2 (native upload) of the dynamic policy model loading plan
  must land first — without file ingress there is nothing to stage.
- Admin gate from the telnet work is already in place and can be
  reused without modification.
