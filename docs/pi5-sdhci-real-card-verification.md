# Pi 5 real-card SDHCI R/W/V — verification log

Hardware-test log for #371 sub-task 5 ("real-card SDHCI quirks…
read/write/verify loops on a guarded region of the boot
partition"). Captures empirical evidence that the BCM2712 SDHCI
driver (`sdhci_create_bcm2712()` from PR #422 + the #457 settle
delay) supports the `kernel stage`/`kernel rollback` admin path
on real Pi 5 hardware, and the bug found while exercising the
loop.

---

## 2026-04-27 — initial verification (claude-code, post-#468 build)

**Board:** `pi-5-1`
**EEPROM:** `pieeprom-2024-09-23.bin`
**Kernel build base:** `origin/main` post-#468 + the kernel_cmd →
boot_media migration in this PR.

### Bug found while testing — `kernel_cmd.c` bypass of `boot_media`

The first stage→rollback cycle worked. The second cycle worked.
The third hit:

```
[ERROR] kernel/drivers/bcm_mailbox.c:259: mailbox: unexpected response code 0x00000000;
        buf=[00000020 00000000 00038001 00000008 00000000 00000001 00000001 00000000]
[ERROR] kernel/drivers/sdhci.c:1194: sdhci_bcm2712: emmc clock enable failed (-1)
stage: boot volume unavailable
```

Buffer decode:
- `0x00000020` — buffer size (32 bytes)
- `0x00000000` — response code (FAIL — should be `0x80000000`)
- `0x00038001` — tag = `RPI_FIRMWARE_SET_CLOCK_STATE`
- payload: clock id 1 (EMMC), state 1 (on)

Root cause: `kernel_cmd.c`'s `boot_volume_mount` was calling
`sdhci_create_bcm2712()` *directly* on every subcommand, bypassing
`boot_media_acquire`/`release` and the keep-alive ref it pins.
Each create→destroy cycle reissued the SET_CLOCK_STATE mailbox
tag. The `pieeprom-2024-09-23.bin` firmware tolerates a few rapid
toggles and then starts returning failure. Symptoms hidden in
sub-task 4 because each `kernel activate` ends in `psci_system_reset`
— there's never a rapid second mailbox call within one SLM-OS
boot.

**Fix in this PR:** route `boot_volume_mount` /
`boot_volume_unmount` through `boot_media_acquire` / `release`.
The keep-alive ref pins the controller after the first successful
create, subsequent acquires return the cached device, and
SET_CLOCK_STATE is issued exactly once per kernel boot.

### Stage / rollback round-trip — 5/5 after fix

Test setup: write a known 36-byte payload to `/mnt/files/sdhci-test.bin`
via the `write` shell command, then run `kernel stage` /
`kernel rollback` repeatedly.

Payload: `SLM-OS sub-task 5 R/W/V test payload` (36 bytes)
Expected SHA-256: `99aa8d7cf00a4c045de5e88e562eb235c64736079cd7252f3051e9b660e8b999`

| Cycle | `kernel stage` | SHA matches | `kernel rollback` |
|---|---|---|---|
| 1 | `staged 36 bytes ... sha = 99aa8d7c…` | ✅ | `cleared staged image` ✅ |
| 2 | `staged 36 bytes ... sha = 99aa8d7c…` | ✅ | `cleared staged image` ✅ |
| 3 | `staged 36 bytes ... sha = 99aa8d7c…` | ✅ | `cleared staged image` ✅ |
| 4 | `staged 36 bytes ... sha = 99aa8d7c…` | ✅ | `cleared staged image` ✅ |
| 5 | `staged 36 bytes ... sha = 99aa8d7c…` | ✅ | `cleared staged image` ✅ |

After the final stage, the SD card was inspected via `sdwire_cat`
to confirm the bytes landed correctly:

| File | Expected | Observed |
|---|---|---|
| `tryboot.img` size | 36 bytes | 36 bytes ✅ |
| `tryboot.img` content | `SLM-OS sub-task 5 R/W/V test payload` | matches ✅ |
| `tryboot.sha` | `99aa8d7cf00a…0e8b999\n` (65 bytes) | matches ✅ |

After `kernel rollback`, the card directory listing showed
neither `tryboot.img` nor `tryboot.sha` — both files cleanly
deleted by the kernel-side path.

**Pre-fix:** failed at cycle 3 with the mailbox error above.
**Post-fix:** 5/5 cycles passed end-to-end. The earlier
`[INFO] sdhci: probing emmc2 ...` / `[INFO] sdhci: destroy emmc2`
log lines that bracketed every command are gone — confirming the
controller is now pinned across commands.

### Boot-reliability smoke check — 9/9 attempted boots

After the kernel_cmd → boot_media migration, ran
`labctl boot_test --count 10 --expect_pattern 'SLM-OS Debug Shell'`
to confirm the migration didn't regress the boot path. Result:
**9/10 PASS, 1 lab-infra fail.** The single non-PASS was a Kasa
power-plug authentication error from labctl (`AuthenticationError`
from the smart-plug driver), not a Pi 5 boot failure — the run
never reached the Pi. Effective: **9/9 actual boots reached
the shell prompt at the expected pattern within 25 s.**

### Conclusion

Real-card SDHCI R/W via `sdhci_create_bcm2712` works as
designed once the controller is pinned across calls. The bug
was an architectural bypass in `kernel_cmd.c`, not a hardware
quirk. The fix is small, code-local, and unifies the boot-media
access path across all in-kernel users (blob_autoload,
persistent_lfs_store, kernel_cmd).
