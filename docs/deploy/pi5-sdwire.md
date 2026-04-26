# Deploying SLM-OS to Raspberry Pi 5 — SDWire-First Model

This guide covers the **SDWire-first deploy model** for a Pi 5 equipped with an SDWire (or SDWireC) SD-card multiplexer. The SDWire switches the SD card between the development host (for flashing) and the Pi 5 (for booting) without physical intervention, so a full build → deploy → serial-capture cycle is scripted and hands-free.

This workflow has been re-validated on `pi-5-2` using the shared
`pc-sdwire` path: the host replaced partition-1 `kernel_2712.img`
directly, switched the card back to DUT mode, and the refreshed SLM-OS
image booted successfully afterward. `pi-5-2` still keeps its
maintenance-OS dual-boot path as the normal day-to-day recovery model;
the point here is that the SDWire-assisted host-driven update path also
works and should stay supported.

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
| `pi-5-2` with shared `pc-sdwire` attached | This guide for host-driven kernel replacement, plus [`../pi5-dual-boot-setup.md`](../pi5-dual-boot-setup.md) for normal maintenance-OS boot control |

---

## Prerequisites

| Item | Source / Version |
|---|---|
| Pi 5 with SDWire installed | SDWire (FTDI, original) or SDWireC (Realtek, newer). Both are supported. |
| SDWire registered in labctl | `labctl sdwire list` shows the device; `lab://sdwire-devices` has an `assigned_to` mapping |
| SD card provisioned for bare-metal SLM-OS | Per [`pi5-sdcard.md`](pi5-sdcard.md) |
| labctl CLI or MCP access | See `docs/lab-operations.md` |
| SLM-OS source checkout | `make kernel PLATFORM=RASPI5` succeeds |

---

## One-shot deploy and test

The fastest path is the `/deploy-and-test` slash command. It runs the full build → flash → boot → serial-capture cycle as a single logical operation:

```
/deploy-and-test pi-5-1
```

This:

1. Builds the kernel for the Pi 5 (`make kernel PLATFORM=RASPI5`).
2. Powers off the Pi 5 (required — labctl refuses to switch an SDWire to host mode while the SBC is powered on).
3. Switches the SDWire to host mode.
4. Mounts the FAT32 boot partition, copies `build/kernel/slmos.bin` to `kernel_2712.img`, syncs, unmounts.
5. Switches the SDWire back to DUT mode.
6. Powers on the Pi 5.
7. Captures serial output until either a pattern match or a timeout.

No manual mount/copy/unmount cycles and no chance of writing to the wrong block device — labctl tracks the SDWire-to-SBC mapping and exposes the correct block device to the mount step.

---

## Manual labctl MCP workflow

When the slash command is not appropriate (e.g. running a one-off flash without capture, or capturing a longer boot), the MCP tools can be driven directly. This is also the canonical reference for what `/deploy-and-test` does under the hood.

### Step 1 — Build

```bash
make kernel PLATFORM=RASPI5
```

Output: `build/kernel/slmos.bin`.

### Step 2 — Flash (via `labctl sdwire_update`)

`sdwire_update` is the single tool that handles the power-off / switch-to-host / mount / copy / sync / unmount / switch-to-DUT sequence:

```
labctl sdwire_update pi-5-1 \
    --kernel build/kernel/slmos.bin \
    --kernel-name kernel_2712.img
```

The `--kernel-name` argument is required — the Pi 5 firmware looks for `kernel_2712.img` specifically, not `slmos.bin`. See `docs/archive/investigations/pi5-cross-cpu-dispatch-investigation.md:193` for the incident where a missing `--kernel-name` caused weeks of stale-binary boots that looked like real regressions.

### Step 3 — Power on

```
labctl power cycle pi-5-1 --delay 2
```

The 2-second off-delay lets the Pi 5 PMIC fully discharge before reboot. Shorter delays have been observed to produce intermittent boot failures on the Pi 5.

### Step 4 — Observe

```
labctl connect pi-5-1-console
```

or for an automated capture (e.g. scripting a test):

```
labctl serial_capture pi-5-1 --timeout 30 --pattern "slm>"
```

The `--pattern "slm>"` argument returns control when the shell prompt appears. This is the hand-off point from boot to userland — if it appears within the timeout, the kernel is healthy.

---

## Expected output

A healthy boot produces, in order:

1. Pi firmware banner (enabled by `uart_2ndstage=1` in `config.txt`) — a few lines about firmware version, DRAM size, and kernel load.
2. SLM-OS boot messages — MMU setup, VMM init, PMM init, scheduler start, SMP bring-up (cores 1–3 come up via PSCI).
3. `slm>` shell prompt.

End-to-end the cycle takes ~30 seconds on the capstone lab hardware (Pi 5 boot is ~5 s; the rest is SDWire switch latency and serial settling).

---

## Troubleshooting

### `sdwire_update` fails with "SBC is powered on"

labctl refuses to switch an SDWire to host mode while the SBC is drawing power, because SD-bus contention between the host and the SBC can corrupt the card. `sdwire_update` is supposed to power-off first, but in some edge cases (e.g. Kasa smart plug auth failure) the power-off silently fails. Manual fix:

```
labctl power off pi-5-1
labctl sdwire_update pi-5-1 --kernel build/kernel/slmos.bin --kernel-name kernel_2712.img
```

If that still fails, `force=true` overrides the safety check — but verify the SBC is actually powered off first.

### SDWire block device changes (`/dev/sdc` → `/dev/sdd`)

When multiple USB devices are plugged in or the system reboots, Linux may reassign the SDWire's block device name. labctl re-queries the device each time, so `sdwire_update` always writes to the correct card. Manual mounts based on a hard-coded `/dev/sdX` are dangerous and must be verified against `labctl sdwire list` every time.

### Kernel builds but doesn't boot the Pi

Most common causes, in order:

1. **`--kernel-name` not set to `kernel_2712.img`.** Pi firmware ignores `slmos.bin` as a filename; only `kernel_2712.img` is recognized.
2. **`config.txt` missing required bare-metal keys.** Re-verify the card against [`pi5-sdcard.md`](pi5-sdcard.md) §First-time provisioning — all six of `arm_64bit=1`, `kernel_address=0x80000`, `kernel=kernel_2712.img`, `pciex4_reset=0`, `uart_2ndstage=1`, `os_check=0` must be present.
3. **Pi firmware on the card is newer than what the kernel was tested against.** Pi 5 firmware updates in 2025 changed the RP1 init sequence. Bare-metal SLM-OS has been validated against **March 2025 firmware** (see [`pi5-sdcard.md`](pi5-sdcard.md) §Troubleshooting for how to verify / revert). Newer firmware may silently break UART bring-up.

---

## Related documents

- `docs/lab-operations.md` — labctl setup, power, serial, full workflow reference
- `docs/pi5-baremetal-status.md` — authoritative `config.txt` spec, EEPROM firmware compatibility
- `docs/deploy/pi5-sdcard.md` — first-time provisioning + sneakernet path
