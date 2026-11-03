# Deploying SLM-OS to Raspberry Pi 5 — SDWire-First Model

This guide covers the **SDWire-first deploy model** for a Pi 5 equipped with an SDWire (or SDWireC) SD-card multiplexer. The SDWire switches the SD card between the development host (for flashing) and the Pi 5 (for booting) without physical intervention, so a full build → deploy → serial-capture cycle is scripted and hands-free.

This workflow also works on a card that carries the maintenance-OS
dual-boot layout: the host replaces partition-1 `kernel_2712.img`
directly, switches the card back to DUT mode, and the refreshed SLM-OS
image boots. Which boards currently have an SDWire is labctl
configuration (`lab://sbcs`), not something this guide records.

For first-time provisioning of a new SD card, see [`pi5-sdcard.md`](pi5-sdcard.md). That guide writes the initial FAT32 layout, Pi firmware, and bare-metal `config.txt`. This guide picks up from an already-provisioned card and only replaces `kernel_2712.img`.

This model should be preserved even if a dual-boot maintenance-OS
workflow also exists elsewhere in the lab. SDWire remains the preferred
fast iteration path when the hardware supports it.

---

## When to use this guide

| Scenario | Guide |
|---|---|
| New Pi 5, blank SD card | [`pi5-sdcard.md`](pi5-sdcard.md) first, then this |
| Pi 5 with SDWire, iterative development | This guide |
| Pi 5 without a dedicated SDWire path | [`pi5-sdcard.md`](pi5-sdcard.md) or [`../pi5-dual-boot-setup.md`](../pi5-dual-boot-setup.md) |
| Pi 5 with SDWire and a dual-boot card | This guide for host-driven kernel replacement, plus [`../pi5-dual-boot-setup.md`](../pi5-dual-boot-setup.md) for normal maintenance-OS boot control |

---

## Prerequisites

| Item | Source / Version |
|---|---|
| Pi 5 with SDWire installed | SDWire (FTDI, original) or SDWireC (Realtek, newer). Both are supported. |
| SDWire registered in labctl and assigned to the board | `lab://sbcs` lists an `sdwire` entry for the board |
| SD card provisioned for bare-metal SLM-OS | Per [`pi5-sdcard.md`](pi5-sdcard.md) |
| labctl CLI or MCP access | See `docs/lab-operations.md` |
| SLM-OS source checkout | `make kernel PLATFORM=RASPI5` succeeds |

---

## One-shot deploy and test

The fastest path is the `/deploy-and-test` slash command. It runs the full build → flash → boot → serial-capture cycle as a single logical operation:

```
/deploy-and-test <sbc>
```

`<sbc>` is a Pi 5 with an SDWire assigned in labctl. The skill
(`.claude/skills/deploy-and-test/SKILL.md` is the authoritative
description):

1. Builds the kernel for the Pi 5 (`make kernel PLATFORM=RASPI5`).
2. Checks the board's live state and claims it in labctl.
3. Reads `config.txt` for the kernel file name, then deploys with one
   `sdwire_update` call: the new kernel is copied in and the current
   one is kept as `<kernel>.prev`. labctl powers the board off first,
   switches the SDWire to the host and back, and does the mount and
   copy itself.
4. Boots the board and captures serial output until `slmos>` or a
   timeout, then runs any requested shell commands.
5. **Restores the previous kernel by default** and returns the board to
   its starting power state. Pass `--keep` to leave the new kernel
   installed.

No manual mount/copy/unmount cycles and no chance of writing to the wrong block device — labctl tracks the SDWire-to-SBC mapping and exposes the correct block device to the mount step.

---

## Manual labctl MCP workflow

When the slash command is not appropriate (e.g. running a one-off flash without capture, or capturing a longer boot), the MCP tools can be driven directly. The deploy step below is the same `sdwire_update` call `/deploy-and-test` makes, minus its backup and restore.

### Step 1 — Build

```bash
make kernel PLATFORM=RASPI5
```

Output: `build/kernel/slmos.bin`.

### Step 2 — Flash (via `sdwire_update`)

`sdwire_update` is the single tool that handles the power-off / switch-to-host / mount / copy / sync / unmount / switch-to-DUT sequence. As an MCP call:

```
sdwire_update(sbc_name="<sbc>", partition=1,
              copies=["<abs path>/build/kernel/slmos.bin:kernel_2712.img"])
```

CLI equivalent:

```
labctl sdwire update <sbc> --partition 1 \
    --copy build/kernel/slmos.bin:kernel_2712.img
```

The source path must be one labctl can read (its allowed read paths, or an uploaded image; `list_images` lists the options).

The destination name matters: the Pi 5 firmware loads the file named by `kernel=` in `config.txt` (`kernel_2712.img` by default), not `slmos.bin`. Check `config.txt` first (`sdwire_cat`). See `docs/archive/investigations/pi5-cross-cpu-dispatch-investigation.md:193` for the incident where a wrong kernel file name caused weeks of stale-binary boots that looked like real regressions.

### Step 3 — Power on

```
labctl power cycle <sbc>
```

labctl waits the board's stored minimum cycle delay between off and on (a smaller `--delay` is raised to it). The Pi 5 PMIC needs a few seconds to discharge fully; delays under about 2 s have been observed to produce intermittent boot failures, so the stored minimum for a Pi 5 should not go below that.

### Step 4 — Observe

```
labctl connect <sbc>
```

or for an automated capture (e.g. scripting a test):

```
labctl serial capture <sbc> --timeout 30 --until "slmos>"
```

The `--until "slmos>"` argument returns control when the shell prompt appears. This is the hand-off point from boot to userland — if it appears within the timeout, the kernel is healthy.

---

## Expected output

A healthy boot produces, in order:

1. Pi firmware banner (enabled by `uart_2ndstage=1` in `config.txt`) — a few lines about firmware version, DRAM size, and kernel load.
2. SLM-OS boot messages — MMU setup, VMM init, PMM init, scheduler start, SMP bring-up (cores 1–3 come up via PSCI).
3. `slmos>` shell prompt.

End-to-end the cycle takes ~30 seconds on the capstone lab hardware (Pi 5 boot is ~5 s; the rest is SDWire switch latency and serial settling).

---

## Alternative: in-place update from a running SLM-OS

When the Pi 5 is already booted and reachable over the
multi-session shell, the kernel can be replaced without involving
the SDWire at all. Use the `kernel` admin command surface and the
Pi 5 tryboot one-shot mechanism — see [`pi5-sdcard.md`](pi5-sdcard.md)
§"In-place update from a running SLM-OS" for the procedure.

That path is faster (no power-off, no SDWire switch latency, no
remount) but requires the board to already be running and serving
telnet. The SDWire path remains the recovery option when the
running kernel cannot bring up networking, when the board is
wedged, or when validating a first-boot regression on a fresh
image.

---

## Troubleshooting

### `sdwire_update` fails with "SBC is powered on"

labctl refuses to switch an SDWire to host mode while the SBC is drawing power, because SD-bus contention between the host and the SBC can corrupt the card. `sdwire_update` powers the board off first; if that power-off fails (for example, the smart plug rejects labctl's authentication), it refuses before touching the card. Fix the power path, then retry:

1. Check the plug and board state: `labctl health-check --sbc <sbc>`, or MCP `run_health_check`.
2. Power the board off: `labctl power off <sbc>`.
3. Rerun the `sdwire_update` call from Step 2.

Do not override the power check (`force`) unless the board is confirmed off by other means; that check is what protects the card from bus contention.

### SDWire block device changes (`/dev/sdc` → `/dev/sdd`)

When multiple USB devices are plugged in or the system reboots, Linux may reassign the SDWire's block device name. labctl re-queries the device each time, so `sdwire_update` always writes to the correct card. Manual mounts based on a hard-coded `/dev/sdX` are dangerous and must be verified against `labctl sdwire list` every time.

### Kernel builds but doesn't boot the Pi

Most common causes, in order:

1. **Copy destination doesn't match `kernel=` in `config.txt`.** The firmware loads only the file `kernel=` names (`kernel_2712.img` by default); a `slmos.bin`, or a kernel copied under another name, is ignored and the old kernel keeps booting. Check the destination in the Step 2 `copies` / `--copy` argument.
2. **`config.txt` missing required bare-metal keys.** Diff the card's `config.txt` against [`deploy/pi5/config.txt`](../../deploy/pi5/config.txt) — that file is the source of truth. Without `os_check=0`, `arm_64bit=1` and a `kernel=` line naming the deployed file, the firmware won't start SLM-OS. `uart_2ndstage=1` is what makes the firmware banner appear, so a card without it is silent until SLM-OS's own UART comes up. The reference file also sets `kernel_address=0x80000` and `pciex4_reset=0` (see [`docs/pi5-baremetal-status.md`](../pi5-baremetal-status.md) §Configuration), but the lab's pi-5-1 card boots without `kernel_address`, so a missing key is not by itself proof of the cause.
3. **Pi firmware on the card is newer than what the kernel was tested against.** Pi 5 firmware updates in 2025 changed the RP1 init sequence. Bare-metal SLM-OS has been validated against **March 2025 firmware** (see [`pi5-sdcard.md`](pi5-sdcard.md) §Troubleshooting for how to verify / revert). Newer firmware may silently break UART bring-up.

---

## Related documents

- `docs/lab-operations.md` — labctl setup, power, serial, full workflow reference
- `docs/pi5-baremetal-status.md` — authoritative `config.txt` spec, EEPROM firmware compatibility
- `docs/deploy/pi5-sdcard.md` — first-time provisioning + sneakernet path
