# Pi 5 boot-partition configs

Canonical SLM-OS configuration files that go on the Pi 5 SD card's
FAT32 boot partition. These are the source of truth — provisioning
guides ([`../../docs/deploy/pi5-sdcard.md`](../../docs/deploy/pi5-sdcard.md),
[`../../docs/deploy/pi5-sdwire.md`](../../docs/deploy/pi5-sdwire.md))
copy them onto the card; lab automation flashes them via
`labctl sdwire_update -p 1 -c deploy/pi5/config.txt:config.txt …`.

| File | Loaded when | Selects kernel |
|---|---|---|
| `config.txt` | Default (every normal boot) | `kernel_2712.img` |
| `tryboot.txt` | Tryboot flag armed (one-shot) | `tryboot.img` |

The Pi 5 firmware loads `tryboot.txt` *instead of* `config.txt` when
the bootloader tryboot flag is set; the flag clears after that boot
and the next power cycle falls back to `config.txt`. SLM-OS arms the
flag from the shell via `kernel activate`, which issues the BCM
mailbox `SET_REBOOT_FLAGS` (tag `0x00038064`) + `NOTIFY_REBOOT` (tag
`0x00030048`) pair and then `psci_system_reset`. See
[`../../docs/dynamic-kernel-replace-plan.md`](../../docs/dynamic-kernel-replace-plan.md)
for the full Stage-5 round-trip and verification log.

`[tryboot]` is **not** a config.txt filter section on Pi 5; the
mechanism is the separate `tryboot.txt` file documented above.
Adding `[tryboot]` to `config.txt` confuses some firmware versions
(observed on `pieeprom-2024-09-23.bin`) — leave it out.

If the candidate kernel fails to boot, the firmware does *not*
automatically roll back to `kernel_2712.img`. Recovery is a manual
power cycle (the tryboot flag is one-shot and clears on the boot
attempt regardless of success), or — if that wedges in firmware —
re-flashing the SD card from the host. The `kernel rollback` shell
command deletes `tryboot.img` so a future `kernel activate` fails
the staging precondition rather than re-running a known-bad image,
AND issues the BCM mailbox `SET_REBOOT_FLAGS(0)` tag (on RASPI5
builds) as a defensive belt against any future code path that
might arm the flag without firing the reset, or any firmware
where `notify_reboot` returns without resetting. The current
`kernel activate` path runs `psci_system_reset` immediately
after arming the flag, so there's no in-vivo window for the
flag-clear to matter — but having the call there means rollback
converges on "no candidate, flag clear" regardless of starting
state. Both clean-ups are best-effort.
