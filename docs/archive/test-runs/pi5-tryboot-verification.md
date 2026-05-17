# Pi 5 tryboot mailbox round-trip — verification log

Hardware-test log for #371 sub-task 4 (`kernel activate` →
firmware-honored tryboot → fall-back). Captures the empirical
evidence that the dynamic-kernel-replace plan's tryboot mechanism
actually works end-to-end on real hardware, and the wrong-paths
that were ruled out. Future re-verifications append a new section
here rather than overwriting earlier ones.

---

## 2026-04-27 — initial verification (claude-code, post-PR-#464 build)

**Board:** `pi-5-1`
**EEPROM:** `pieeprom-2024-09-23.bin`
**Kernel build base:** `origin/main` at `976bbb9` (post-#464 size-ceiling
bump rebase). `make kernel PLATFORM=RASPI5`, default features
(networking + telnetd autostart + Lua + lwIP + AI runtime), kernel
size ~4.9 MB. Two distinct builds were used so the build-stamp
swap could prove which file the firmware actually loaded:

| Role | Build stamp | Sha tag |
|---|---|---|
| `kernel_2712.img` | `20260427081608` | `976bbb9` |
| `tryboot.img`     | `20260427095520` | `976bbb9-dirty` (sub-task-4-tryboot-roundtrip branch tip after `touch kernel/src/main.c`) |

**Card state:** single FAT32 partition, the canonical Pi 5 firmware
files plus `deploy/pi5/config.txt` (kernel=kernel_2712.img),
`deploy/pi5/tryboot.txt` (kernel=tryboot.img), and the two kernel
images (build `081608` as `kernel_2712.img`, build `095520` as
`tryboot.img`).

### Round-trip results — 10/10

For each cycle: send `kernel activate` over UART, capture serial
until the boot banner prints, read the build stamp, compare against
the expected source file. Then `power_cycle` (no activate) and
capture again to confirm one-shot fall-back. Cycle count matches
the `labctl boot_test --count 10` requirement in the plan doc.

| Cycle | Pre-activate | After `kernel activate` | After natural power cycle |
|---|---|---|---|
| 1 | `081608` (kernel_2712.img) | `095520` (tryboot.img) ✅ | `081608` (kernel_2712.img) ✅ |
| 2 | `081608` | `095520` ✅ | `081608` ✅ |
| 3 | `081608` | `095520` ✅ | `081608` (verified via `kernel status`) ✅ |
| 4 | `081608` | `095520` ✅ | `081608` ✅ |
| 5 | `081608` | `095520` ✅ | `081608` ✅ |
| 6 | `081608` | `095520` ✅ | `081608` ✅ |
| 7 | `081608` | `095520` ✅ (after one shell-input glitch) | `081608` ✅ |
| 8 | `081608` | `095520` ✅ | `081608` (verified via `kernel status`) ✅ |
| 9 | `081608` | `095520` ✅ | `081608` ✅ |
| 10 | `081608` | `095520` ✅ (after one shell-input glitch) | `081608` ✅ |

**10/10 pass.** Firmware honors `tryboot.txt` on every armed boot,
and the flag is genuinely one-shot — subsequent natural power cycles
return to `config.txt` without intervention. The two "shell-input
glitches" recorded for cycles 7 and 10 are post-power-cycle artifacts
where the first character of the typed command was lost; retyping
worked. Not part of the tryboot mechanism — separate transient
serial-driver behaviour after boot.

### Wrong paths ruled out

These were tried earlier in the session and produced wedges severe
enough to file #462 before the right mechanism was identified:

- **`[tryboot]` filter section in `config.txt`** (instead of
  separate `tryboot.txt`). Firmware on this EEPROM appears to
  misparse the bracketed line and breaks boot — even commenting it
  with `#` doesn't help if the closing bracket is on the same line
  the parser scans.
- **`[tryboot]` *inside a `#` comment* in `config.txt`.** The
  firmware config parser is naive about comments and treats
  `# [tryboot]` as a real filter section, silently breaking boot.
- **No `tryboot.txt` and no `[tryboot]` section, just `kernel
  activate`.** With the flag set but no tryboot config to honor,
  firmware does not fall through to `[all]` / `config.txt`. The
  Pi 5 wedges in a way that survives multiple cold power cycles
  and SD-card content reverts; recovery requires user-side
  hardware intervention. See #462 for the deeper notes from that
  observation.

The mechanism that works is exactly the one Linux kernel's
`drivers/firmware/raspberrypi.c` documents:
**`SET_REBOOT_FLAGS(1)` + `NOTIFY_REBOOT` + system reset →
firmware loads `tryboot.txt` instead of `config.txt` for that one
boot.** SLM-OS's `kernel activate` already issues that exact tag
sequence; the missing piece was `tryboot.txt` on the card.
