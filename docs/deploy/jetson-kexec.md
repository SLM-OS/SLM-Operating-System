# Deploying SLM-OS to Jetson Orin Nano — kexec from Linux

This guide covers the **primary deploy path** for Jetson Orin Nano boards: boot Linux (L4T / JetPack) from the normal rootfs, then `kexec` into SLM-OS. This inherits the NVIDIA boot chain's exception-level configuration (NS EL2 + VHE) and PCIe initialization, which SLM-OS needs — the UEFI-direct cold-boot path is still WIP (see `docs/archive/investigations/jetson-uefi-direct-result.md` for blockers).

The kexec-from-Linux flow is identical whether the Linux install lives on the microSD card or an NVMe SSD. [First-time provisioning](#first-time-provisioning-dual-media-layout) describes the recommended lab layout (NVMe development install plus microSD recovery install) and what each needs. SLM-OS itself never gets a partition: it is a file in the Linux root filesystem.

---

## When to use this guide

| Scenario | Guide |
|---|---|
| Jetson with L4T already installed | This guide |
| Fresh Jetson, or reinstalling a lab board | [First-time provisioning](#first-time-provisioning-dual-media-layout) below, then the rest of this guide |
| Jetson cold-boot directly to SLM-OS (no Linux in the chain) | **Not yet working** — `docs/archive/investigations/jetson-uefi-direct-result.md` |

---

## First-time provisioning: dual-media layout

### SLM-OS has no partition of its own

On Jetson, SLM-OS is a single ELF file (`/root/slmos.elf`) stored in an
ordinary Linux root filesystem. Linux boots normally, then `slmos-kexec`
loads the ELF and transfers control with `kexec`. SLM-OS never reads
the boot media itself: it has no SD or NVMe driver on Jetson
(`docs/fact-sheets/storage.md`), and its `/mnt/files` is a RAM-disk
LittleFS (`docs/setup.md` §"Jetson SD-Card Layout"). Provisioning a
Jetson for SLM-OS therefore means provisioning a Linux install with the
SLM-OS extras listed below. No disk carries an "SLM-OS partition".

This is also why the Jetson development loop needs neither an SDWire
nor moving the SD card between the board and the host. Each new build
reaches the board over SSH (`scp`, then `sync`), and a power cycle
returns the board to Linux for the next iteration.

### Recommended layout

| Medium | Role | Boot order | Contents |
|---|---|---|---|
| NVMe SSD | Development Linux — the kexec launcher | Default | JetPack R36.4.x matching the QSPI firmware, plus every item in [SLM-OS extras](#slm-os-extras-on-the-development-install) |
| microSD | Recovery Linux | Selected manually from the UEFI boot manager | Stock JetPack with networking and root SSH only. Never kexecs into SLM-OS |

Rationale:

- Every iteration writes a new `slmos.elf` and runs `sync`. NVMe is
  faster and tolerates repeated writes better than microSD.
- A stock recovery install means a broken development install (bad
  helper build, failed module install, full rootfs) never leaves the
  board unbootable. Booting the SD card gives a working Linux from
  which the SSD can be repaired or re-flashed.
- A stock Linux on the same board is a clean baseline for comparing
  Linux-side GPU and USB behavior against the development install's
  patched modules.

The `kexec` flow is identical on either medium, so the roles can be
swapped. A board without an NVMe drive runs the development install
from microSD and has no recovery install.

### Boot firmware is shared — QSPI caveats

Every boot stage up to and including UEFI lives in the module's QSPI
flash, not on the SD card or SSD (`docs/jetson-boot.md`). Both media
boot through the same firmware, which has three consequences:

1. **Flashing rewrites QSPI.** SDK Manager and `l4t_initrd_flash.sh`
   reflash QSPI along with the rootfs. On a board carrying the patched
   BL31 from `tools/tfa-patches/0004-*` (required by the default
   `JETSON_HW_TICK=ON` build), a reflash restores stock BL31. Redeploy
   the patched BL31 per `tools/tfa-patches/README.md` §"Deploying on
   Jetson Orin Nano", or build with `JETSON_HW_TICK=OFF` until then.
2. **Flash the Super configuration.** The board configuration chosen at
   flash time sets the GPU clock ceiling: a non-Super flash caps the
   GA10B near 624 MHz versus about 1020 MHz for Super (#844). Every lab
   board should use `jetson-orin-nano-devkit-super` so GPU results are
   comparable across boards.
3. **Match releases.** The rootfs on each medium should match the
   JetPack release whose firmware is in QSPI (the UEFI banner on serial
   shows the firmware version). Both lab boards should also run the
   same release; board-to-board comparisons such as #844 depend on it.

### Flashing the NVMe SSD

The SSD must be flashed with NVIDIA's external-storage flow, which
creates the full GPT partition set on the NVMe, including the EFI
system partition that holds L4TLauncher. A rootfs copied onto a single
ext4 partition is not bootable: the UEFI boot-manager entry for the
drive silently returns to the menu (observed on `jetson-nano-1`,
2026-10-29).

Use either SDK Manager (target storage: NVMe) or the BSP's initrd flash
from `Linux_for_Tegra/` on the host, with the board in USB Force
Recovery (J14 pins 9-10 shorted at power-on):

```bash
sudo ./tools/kernel_flash/l4t_initrd_flash.sh \
    --external-device nvme0n1p1 \
    -c tools/kernel_flash/flash_l4t_t234_nvme.xml \
    -p "-c bootloader/generic/cfg/flash_t234_qspi.xml" \
    --showlogs --network usb0 \
    jetson-orin-nano-devkit-super internal
```

This is the R36 quick-start form; check the flags against the Jetson
Linux Developer Guide for the BSP release in use. The command writes
QSPI as well (caveat 1 above).

### Preparing the microSD card

Write NVIDIA's JetPack SD card image for the same release as the SSD.
A matching release matters here too: the SD image's bootloader-update
service may apply its own firmware capsule to QSPI on first boot when
the versions differ, with the same BL31 consequence as caveat 1.

After first boot, enable root SSH login with the lab password, give
the install a distinct hostname (for example `jetson-nano-1-recovery`)
so a serial login prompt identifies which medium booted, and record
its address in labctl.

### Boot order

Leave the NVMe entry first in the UEFI boot order so power cycles land
in the development install. The recovery install is a one-shot choice
from the UEFI boot manager (F11 at the boot prompt); permanent order
changes live in UEFI Setup (ESC) → Boot Maintenance Manager. Both keys
work over the labctl serial console via `serial_send` with
`keys=["F11"]` or `keys=["ESC"]`.

### SLM-OS extras on the development install

| Item | Location on the Jetson | Guide |
|---|---|---|
| Root SSH login (lab password `slmos`) | `/etc/ssh/sshd_config` | — |
| Address recorded in labctl | — | labctl is the single source of truth for lab IPs |
| `kexec-tools` | system package (`apt install kexec-tools` if absent) | — |
| `slmos-kexec` helper | `/usr/local/bin/slmos-kexec` | [One-time setup](#one-time-setup-install-the-slmos-kexec-helper) below |
| SMMU preservation module (USB-A/XHCI work) | `/usr/local/lib/slmos/arm_smmu_noshutdown.ko` | One-time setup below |
| GPU channel helpers, shaders, weights | `/root/gpu-mnist/` | [`jetson-helper-setup.md`](jetson-helper-setup.md) |
| Patched `nvgpu.ko` | `/lib/modules/$(uname -r)/updates/nvgpu.ko` | `tools/nvgpu-patches/README.md` |
| Auto-kexec target (optional) | `/etc/systemd/system/slmos-autokexec.*` + extlinux entry | [Auto-kexec entry](#optional-add-an-extlinux-boot-menu-entry-that-auto-kexecs-into-slm-os) below |
| Patched BL31 | QSPI (shared by both media) | `tools/tfa-patches/README.md` |
| SLM-OS kernel | `/root/slmos.elf` | [Deploy workflow](#deploy-workflow) below |

The recovery install gets none of these.

---

## Prerequisites

| Item | Source / Version |
|---|---|
| Jetson Orin Nano Developer Kit with L4T installed | L4T 36.4.7 / JetPack R36 known to work. See `docs/lab-operations.md` for lab board state. |
| Jetson on the network (SSH reachable) | `ssh root@<JETSON_IP>` — password `slmos` in the capstone lab, or pre-shared key |
| SLM-OS source checkout | `make kernel PLATFORM=JETSON_ORIN_NANO` succeeds |
| ARM GNU Toolchain | `aarch64-none-elf-gcc` |
| Serial console set up in labctl | See `docs/lab-operations.md` — CP2102 adapter required for bidirectional serial on the debug header (CH340 is send-only on Jetson) |
| `slmos-kexec` helper installed on the Jetson | One-time setup, below |

---

## One-time setup: install the `slmos-kexec` helper

The `scripts/jetson-kexec-slmos.sh` helper does three things the raw `kexec` call cannot:

1. **GPU runtime-PM suspend + BPMP clock re-enable.** Prevents the TF-A RAS Uncorrectable Error that would otherwise kill the CPU core mid-kexec (issue #9). The suspend drains stale nvgpu DMA; the BPMP re-enable puts the GPU's clocks + powergate back on so SLM-OS sees `NV_PMC_BOOT_0` instead of `0xFFFFFFFF`.
2. **USB xHCI clock + powergate holds** (`--no-usb-hold` to skip). Keeps `xusba` / `xusbc` powergates and the `xusb_*` clock tree alive across the kexec so SLM-OS's XHCI driver finds the controller responsive (issue #266).
3. **Optional xusb arm-smmu preservation.** If `arm_smmu_noshutdown.ko` is installed in `/usr/local/lib/slmos/arm_smmu_noshutdown.ko` (or one of the helper's fallback search paths), the helper loads it before `kexec -e`. This preserves the xusb stream's live SMMU context and adds an identity mapping for SLM-OS's NC memory region, which is required for the current XHCI RUN/NO_OP path.

Install once:

```bash
scp scripts/jetson-kexec-slmos.sh root@<JETSON_IP>:/usr/local/bin/slmos-kexec
ssh root@<JETSON_IP> "chmod +x /usr/local/bin/slmos-kexec"
```

Optional but recommended for USB-A/XHCI work:

```bash
scp -r scripts/arm-smmu-noshutdown root@<JETSON_IP>:/root/
ssh root@<JETSON_IP> 'mkdir -p /usr/local/lib/slmos && \
    cd /root/arm-smmu-noshutdown && make && \
    install -m 0644 arm_smmu_noshutdown.ko /usr/local/lib/slmos/'
```

If the `.ko` is not present, `slmos-kexec` still runs, but the Jetson
USB-A host path may wedge at `USBCMD.RUN=1`.

Current status on `jetson-nano-2`: with the helper's wider XUSB hold
set, the SMMU preservation module installed, and the helper's USB2
root-hub cleanup enabled, SLM-OS now preserves a cleaned addressed
slot-1 handoff across `kexec`, adopts the retained Realtek root hub,
fresh-enumerates the downstream RTL8153, and brings networking up with
no manual unplug/replug. Current validation on the lab path:

- `net init` succeeds
- a single `ifconfig dhcp` request binds `192.168.4.5/24` with gateway `192.168.4.1`
- `ping 192.168.4.1 2` succeeds

The helper script is reasonably well commented. Run it with `--help` on the Jetson for the full flag list, or read the script header for rationale on each step.

### Optional: add an extlinux boot-menu entry that auto-kexecs into SLM-OS

If you want a "no login step" deploy path but still want Linux to remain
the default boot, install the repo's `systemd` target + service and add a
second Linux boot entry that lands in that target.

Install the units and default config once:

```bash
scp scripts/systemd/jetson/slmos-autokexec.target \
    root@<JETSON_IP>:/etc/systemd/system/
scp scripts/systemd/jetson/slmos-autokexec.service \
    root@<JETSON_IP>:/etc/systemd/system/
scp scripts/systemd/jetson/slmos-autokexec.env \
    root@<JETSON_IP>:/etc/default/slmos-autokexec
ssh root@<JETSON_IP> 'systemctl daemon-reload && systemctl enable slmos-autokexec.service'
```

The default config uses `/root/slmos.elf`. If you want a different path or
helper flags, edit `/etc/default/slmos-autokexec` on the Jetson. `SLMOS_KEXEC_ARGS`
may contain multiple helper flags separated by spaces.

Then add a second extlinux entry in `/boot/extlinux/extlinux.conf`. Keep the
existing `DEFAULT primary` line so normal boots still land in Linux.

Important: do not hardcode the rootfs stanza from another board. Start from the
working `primary` entry already on that Jetson, copy its `LINUX`, `INITRD`, and
especially its full `APPEND` line unchanged, then append only the two autokexec
markers shown below:

```conf
LABEL slmos-autokexec
      MENU LABEL Linux -> auto-kexec SLM-OS
      LINUX <same as primary>
      INITRD <same as primary>
      APPEND <copy the exact APPEND value from primary> \
             systemd.unit=slmos-autokexec.target slmos.autokexec=1
```

Behavior:

- `primary` remains the default Jetson boot path.
- Selecting `slmos-autokexec` from the UEFI/extlinux menu boots Linux just
  far enough to reach `multi-user.target`, then `systemd` runs
  `/usr/local/bin/slmos-kexec`.
- The serial console still shows the Linux boot first, then the usual
  `slmos-kexec` handoff, then SLM-OS.

This is the preferred unattended test path when you want the current Linux
cooperative `kexec` handoff, but do not want to SSH in and trigger
`slmos-kexec` manually.

---

## Deploy workflow

### Step 1 — Build

```bash
make kernel PLATFORM=JETSON_ORIN_NANO
```

Output: `build/kernel/slmos.elf`. Unlike Pi 5, Jetson takes the ELF form directly — kexec handles loading the segments to the entry address (`0x80000000`) itself.

For one-off Jetson bring-up diagnostics that exist as raw CMake
options rather than first-class Make variables, pass them through with
`EXTRA_KERNEL_CMAKE_ARGS`. Example:

```bash
make kernel-clean
make kernel PLATFORM=JETSON_ORIN_NANO \
    EXTRA_KERNEL_CMAKE_ARGS=-DJETSON_XHCI_REBOOT_ON_NOOP=ON
```

That specific flag builds a one-shot diagnostic image that PSCI-resets
back to Linux immediately after the XHCI driver reaches `NO_OP
round-trip OK`, which is useful when serial capture is unavailable.

### Step 2 — Copy to the Jetson

```bash
scp build/kernel/slmos.elf root@<JETSON_IP>:/root/
ssh root@<JETSON_IP> sync
```

The `sync` is required. The kexec in Step 3 does not flush the page
cache to disk, and the power cycle in Step 5 discards anything still
unwritten — so without `sync`, the next Linux boot can come up with the
*previous* `slmos.elf` and a later run silently tests a stale kernel.

### Step 3 — kexec

```bash
ssh root@<JETSON_IP> 'slmos-kexec /root/slmos.elf'
```

The helper prints its progress as it runs — GPU suspend, BPMP clock force-on, USB hold, final `kexec -e`. The SSH session terminates at the `kexec -e` step (the kernel is replaced underneath the running userspace). Expected: network goes away within ~2 s of the final line.

On the current `jetson-nano-2` lab setup, this path no longer needs a
manual USB unplug/replug after `kexec`; the helper's root-hub cleanup
and the retained slot-1 handoff are enough to get the Realtek USB
Ethernet chain back in SLM-OS automatically.

### Step 3a — Alternate: use the extlinux auto-kexec boot entry

If you installed the optional `slmos-autokexec` target/service above, you
can skip the SSH trigger step entirely:

1. Reboot or power-cycle the Jetson.
2. At the UEFI/extlinux menu, choose `Linux -> auto-kexec SLM-OS`.
3. Wait for Linux to reach the custom target and invoke `slmos-kexec`.

This keeps the default boot path unchanged (`primary`), but gives you a
menu-selectable "Linux first, then auto-kexec" option for unattended test
runs.

### Step 4 — Observe via serial

From the host (before or after issuing `kexec`):

```bash
labctl connect jetson-nano-1-console
```

Expected output on serial:

1. Kernel messages from SLM-OS's `uart_tegra.c` driver — via UARTC at `0x0C280000`, routed through the TCU HSP mailbox to the USB-C debug port.
2. SMP bring-up on 6 cores (Jetson is dual-cluster 2+4; MPIDRs `0x000`, `0x100`, `0x200`, `0x300`, `0x10200`, `0x10300`).
3. `slm>` shell prompt. Input works bidirectionally via the TCU mailbox.

### Step 4a — Run the networking smoke test

For the validated `jetson-nano-2` lab path, the host-side smoke script
automates the current regression check end-to-end:

```bash
scripts/tests/test-jetson-kexec-networking-smoke.sh \
    --ssh-target root@<JETSON_IP> \
    --console-port 4004
```

That script:

- copies `build/kernel/slmos.elf` and `scripts/jetson-kexec-slmos.sh`
- triggers `slmos-kexec`
- waits for `slmos>` on serial
- runs `net init`, sends one `ifconfig dhcp`, polls `ifconfig` until `DHCP(bound)`, and then runs `ping 192.168.4.1 2`
- verifies DHCP `192.168.4.5/24` with gateway `192.168.4.1`

It is intentionally scoped to the current lab topology rather than a
generic Jetson serial automation framework.

### Step 5 — Recovery back to Linux

Once work in SLM-OS is finished:

```bash
labctl power cycle jetson-nano-1
```

Wait for Linux to boot, then SSH is available again. A provisioned
board reaches the login prompt about 65 s after power-on (measured on
`jetson-nano-2`, 2026-10-29). A board that stays silent well past that
is most likely held in USB Force Recovery — see Troubleshooting. SLM-OS itself has no graceful-shutdown-back-to-Linux path — the kexec is a one-way handoff.

---

## Helper script flags

The `slmos-kexec` helper's default behavior suits most cases. The common non-defaults:

| Flag | When to use |
|---|---|
| `--no-gpu-suspend` | Path 3 / issue #190: preserves the GPU's ACR / Falcon security state across kexec so SLM-OS inherits Linux's already-running FECS / GPCCS / PMU. Risk: stale DMA may still trigger a TF-A RAS error, though in practice it hasn't fired when GPU consumers are stopped first. |
| `--no-usb-hold` | SLM-OS builds that don't drive the XHCI controller. The held clocks are otherwise harmless. |
| `--no-smmu-fix` | Skip loading `arm_smmu_noshutdown.ko`. Only useful if you are not exercising the USB-A/XHCI path or are deliberately reproducing the pre-fix failure. |
| `--no-usb-root-cleanup` | Skip the Linux-side USB2 root-hub `authorized=0` cleanup before `kexec`. Useful only if you are intentionally reproducing the old stale-slot path; the default cleanup is what makes the retained slot-1 handoff reproducible on `jetson-nano-2`. |

Full flag list in the script header.

---

## Troubleshooting

### Board silent and off the network after power-on (before any kexec)

Usually the board's labctl `recovery_mode` binding is asserted, which
holds it in USB Force Recovery: no console output, no network, and a
`0955:7523 NVIDIA Corp. APX` device on the lab host's USB. Check the
binding before anything else. The full checklist (live state, recovery
strap, port ownership, actuator permissions) is in
[`docs/lab-operations.md` §Troubleshooting](../lab-operations.md#verify-live-board-state-first).

### kexec succeeds but the Jetson reboots / resets instead of running SLM-OS

Almost always the TF-A RAS error (issue #9) — a stale nvgpu DMA operation fired after CPU had already entered SLM-OS's early boot, TF-A caught it at EL3, and powered off the core. Confirm `slmos-kexec` was used (not raw `kexec`), and that `--no-gpu-suspend` was NOT passed. The GPU suspend step is what prevents this.

### kexec runs but no serial output

1. **Wrong serial adapter on the debug header.** Jetson's SPE firmware only handles input from CP2102 (Silicon Labs). CH340-based adapters are send-only on Jetson and input characters never reach the SPE mailbox. CH340 is fine on Pi 5, but not Jetson.
2. **Serial output uses UARTC (`0x0C280000`), not UARTA.** UARTA on the 40-pin header is blocked by the CBB firewall at EL2. If a serial adapter is wired to the 40-pin header expecting UARTA, it will show nothing. The USB-C debug port routes UARTC via the TCU HSP mailbox.
3. **DSB-barrier issue.** Post-kexec, speculative MMIO reads can return stale LSR values. `uart_tegra.c` issues `dsb sy` before every LSR read. If a change to the driver removed that barrier, output is silent. See `docs/uart-hardware.md` §"Post-kexec MMIO Ordering".

### SSH still works right after `slmos-kexec` — SLM-OS didn't take over

Confirm the `kexec -e` step actually fired. Sometimes `kexec_load` succeeds but `kexec -e` fails silently if the kernel refuses the image — check `dmesg` on the Jetson. Also confirm the binary path: `/root/slmos.elf` is the expected location, but `slmos-kexec` takes a path argument so check which one was used.

---

## Related documents

- `docs/lab-operations.md` — labctl setup, network / SSH / serial config for each Jetson board
- `docs/jetson-boot.md` — low-level boot sequence reference
- `docs/archive/investigations/jetson-el2-bringup.md` — VHE / EL2 configuration after kexec
- `docs/archive/investigations/jetson-nvidia-support.md` — GPU / BPMP / PCIe state across kexec
- `scripts/jetson-kexec-slmos.sh` — the helper itself, with detailed in-file comments
