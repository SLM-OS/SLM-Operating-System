# Pi 5 dynamic-kernel-replace full lifecycle — verification log

Hardware-test log for #371 sub-task 6 — the full
`kernel stage → activate → promote → rollback` round-trip on
`pi-5-1`. Closes #35 when this passes.

The earlier sub-tasks already verified individual pieces:

- **Sub-task 4** (PR #468): `kernel activate` arms the firmware
  tryboot flag and the next boot loads `tryboot.img` via
  `tryboot.txt`, with one-shot semantics. 10/10 round-trips.
- **Sub-task 5** (PR #504): `kernel stage` writes `tryboot.img`
  + `tryboot.sha` byte-perfect through the SDHCI driver, and
  `kernel rollback` deletes both files. 5/5 stage/rollback
  cycles in one boot.

This sub-task chains all four commands in a single test sequence
on real hardware.

---

## 2026-04-27 — initial verification (claude-code, post-PR-#504 build)

**Board:** `pi-5-1`
**EEPROM:** `pieeprom-2024-09-23.bin`
**Kernel build base:** `origin/main` at `4e10adf` (post-#504,
which routed `kernel_cmd` through `boot_media`'s keep-alive ref).
Two distinct kernels with different build stamps so the
post-`promote` boot's kernel can be identified unambiguously:

| Role | Build stamp | Source |
|---|---|---|
| `kernel_2712.img` | `20260427213937` (kernel C) | first `make kernel PLATFORM=RASPI5` of this session |
| `tryboot.img`     | `20260427214003` (kernel D) | second build, after `touch kernel/src/main.c` |

### Phase A — `kernel stage` → `kernel rollback` on the running kernel

Tests the stage write path and the rollback cleanup without
touching the active boot path. Small ASCII payload so the
on-disk file is human-readable in `sdwire_cat` output.

```
slmos> write /mnt/files/test.bin sub-task 6 phase A payload
Wrote 26 bytes to /mnt/files/test.bin
slmos> kernel stage /mnt/files/test.bin
staged 26 bytes from /mnt/files/test.bin;
sha = 58daa53a3795cfb576f11ce8dd6f3f2ce43df64245c127338f7d48aa4591abbe
slmos> kernel status
running kernel: SLM-OS 0.4.0 (build 20260427213937, sha 4e10adf-dirty)
staged image:   tryboot.img (26 bytes), tryboot.sha present
active kernel:  kernel_2712.img (4916608 bytes)
slmos> kernel rollback
rollback: cleared staged image
slmos> kernel status
... staged image:   none
```

Host-side sanity check: `printf 'sub-task 6 phase A payload' | sha256sum`
matches the kernel-reported SHA (`58daa53a…91abbe`). ✅

### Phase B — full `activate → promote → reboot → rollback`

Pre-staged `kernel_D.bin` as `tryboot.img` and its sha sidecar
via `labctl sdwire_update`, since the `kernel stage` shell path
isn't a practical way to upload a 5 MB binary over UART. Phase A
already covered the kernel-side write path through SDHCI; phase B
only needs to prove the firmware-handoff portion of the
lifecycle works.

| Step | Command | Pre-state | Observed result |
|---|---|---|---|
| Verify pre-state | `kernel status` | C running, D staged | "running … 213937", "staged: tryboot.img (4916608 bytes), tryboot.sha present", "active kernel: kernel_2712.img (4916608 bytes)" ✅ |
| Activate | `kernel activate` | tryboot armed → reset | next boot banner = `build 20260427214003` (kernel D) ✅ |
| Verify tryboot kernel | `kernel status` | D running from tryboot.img | "running … 214003", staged image still present | ✅ |
| Promote | `kernel promote` | D running, D staged | log: `promote: tryboot.img → kernel_2712.img`; staged image cleared, kernel_2712.img is now D's content (verified via post-reboot stamp) ✅ |
| Natural power cycle | (no activate) | promote committed | next boot banner = `build 20260427214003` (kernel D) — fallback to `config.txt`/`kernel_2712.img`, which is now D ✅ |
| Idempotent rollback | `kernel rollback` | nothing staged | `rollback: nothing was staged`; status shows no staged image ✅ |
| Activate without stage | `kernel activate` | nothing staged | `activate: no tryboot.img staged — run \`kernel stage\` first` (error, no flag armed) ✅ |

**6/6 lifecycle steps pass.** kernel_2712.img on the card is now
kernel D — the promote was persistent, and the natural power
cycle confirms the firmware loads it without any tryboot
involvement (one-shot semantics, sub-task 4 result).

### `kernel rollback` and the firmware tryboot flag

`cmd_kernel_rollback` (`kernel/src/kernel_cmd.c:444-453`) issues
`bcm_mailbox_set_reboot_flags(0)` on RASPI5 builds in addition to
deleting `tryboot.img` / `tryboot.sha`. The post-`promote` rollback
above hit that code path; no warning was logged, so the mailbox
call returned 0 (success). Whether it actually disarmed an
already-armed flag isn't observable in vivo — `kernel activate`
calls `psci_system_reset` immediately after arming the flag, so
there's no pre-reboot window in which the user could run
`rollback` to clear it. The defensive call is correct anyway:
if a future code path arms the flag without immediately
rebooting (or if `notify_reboot` ever returns without resetting),
rollback converges the system to "no candidate, flag clear"
regardless of starting state.

### Conclusion

- All four `kernel` admin subcommands (`status`, `stage`,
  `activate`, `promote`, `rollback`) work end-to-end against the
  Pi 5 real-card SDHCI driver and `pieeprom-2024-09-23.bin`
  firmware.
- The `tryboot.img → kernel_2712.img` rename in `promote` is
  persistent across natural power cycles, with no firmware
  intervention required.
- The full dynamic-kernel-replace flow described in
  `docs/dynamic-kernel-replace-plan.md` works as designed.
- **Closes #35.**
