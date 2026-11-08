# Rules for every #475 follow-up builder and critic (2026-11-08)

Read this file in full before doing anything. The orchestrator's brief overrides it only where it says so.

## Hardware

- Only jetson-nano-2 (SD Linux at 192.168.4.5). Never touch jetson-nano-1 or any other board.
- The orchestrator holds the labctl claim (agent `claude-code-475-followups`, until 15:46 UTC). Never call
  `claim_sbc`, `release_sbc`, `force_release_sbc` or `request_sbc_release`.
- Touch the board ONLY while the orchestrator has told you, in your brief or a later message, that you own
  it. When your hardware slot ends, leave the board at SD Linux (power_cycle; BootNext is consumed) unless
  told otherwise, and say in your report that you have released it. If you need the board and do not own
  it, finish everything else, then end your turn with a line `HW_REQUEST: <what, how long>`.
- Hardware only through the labctl MCP tools (load them with ToolSearch, e.g.
  `select:mcp__labctl__serial_send,mcp__labctl__serial_capture,mcp__labctl__power_cycle`), never the labctl CLI.
  Destructive tools return a confirm_token first; repeat the call with it. ssh to the board's SD Linux only
  for `scripts/jetson-uefi-deploy.sh`, the documented ESP/BootNext steps, kexec of a file you own, and the
  JetPack CUDA harness (`scripts/cuda/harness/`).
- Before your first hardware action: `run_health_check(jetson-nano-2)` and
  `actuation_status(jetson-nano-2, recovery_mode)`; `last_state` must be `open` (released). Never assert it.
- Always pass `capture_until="slmos>"` (literal `>`) to `serial_send` for shell commands.
- Never: mount/umount by hand outside the documented ESP steps, touch /dev/sd*, nc/picocom, ser2net,
  nvpmodel changes, BootOrder changes, deleting/creating efibootmgr entries, editing `/root/slmos.elf` or
  `/root/slmos.elf.prev`. Kexec work uses `/root/slmos-<issue>.elf` and deletes it afterwards.
- As-found state (the orchestrator restores it at the end; do not disturb it): SD Linux, nvpmodel 25W,
  ESP `\EFI\SLMOS\SLMOS.efi` sha256 789cba62... (copy at `/root/SLMOS.efi.found-20261108`), no
  `\EFI\BOOT\BOOTAA64.efi` on the NVMe ESP, BootOrder 0008,0001,0007,0009,..., `/root/slmos.elf` 098f3163...,
  a pre-existing `/root/slmos.elf.prev` a531a785....

## NVMe ESP hygiene (added 10:45 after FAT corruption was found)

- Never power_cycle while the ESP is mounted. Every ESP write is: mount, write, `sync`, `umount`, then
  `fsck.vfat -n /dev/disk/by-partlabel/SLMOS-ESP` must report no errors (a "Dirty bit" line alone is noted, not
  fixed), then a fresh read-only mount and `sha256sum` of the file you wrote must match. Do this BEFORE the
  reboot that boots it (for the deploy script: run the fsck/sha check in a second ssh call is impossible after it
  reboots, so instead run the check at the start of your next SD Linux session and record it).
- If fsck reports anything beyond the dirty bit, stop, do not repair, and report to the orchestrator.

## Booting SLM-OS UEFI-direct (single boot)

1. `power_cycle(jetson-nano-2)`, then `serial_capture(timeout=150, until_pattern="login:")`.
2. `scripts/jetson-uefi-deploy.sh 192.168.4.5 build/kernel/slmos.bin` (from your worktree).
3. `serial_capture(timeout=150, until_pattern="slmos>|Unhandled Exception|Powering off|ABORT|PANIC|panic")`.

## Cold-boot reliability, 10/10 (docs/deploy/jetson-uefi-direct.md, "To test cold boots with boot_test")

Copy the image to `\EFI\BOOT\BOOTAA64.efi` on the NVMe ESP (`/dev/disk/by-partlabel/SLMOS-ESP`, mount,
copy, sync, verify sha256, umount), then
`boot_test(jetson-nano-2, runs=10, expect_pattern="slmos>", timeout=180)` (check the tool's real argument
names). Count a run as a pass only if it reached `slmos>` and the model line shows the file verified.
Then return to Linux: power_cycle, at the firmware prompt `serial_send keys=["F11"]`, then
`["Down","Enter"]` for "UEFI SD Device" (read the menu first), wait for `login:`, delete BOOTAA64.efi,
sync, and prove a plain power_cycle reaches SD Linux `login:`. Never leave BOOTAA64.efi on the ESP.

## Performance gate (every change touching the GPU path or the decode path)

Follow `/home/john/slmos-ref/derivatives/475-followups/hw-gate.md` exactly (written by the baseline run on
current main c69438a2; the baseline numbers are next to it). Thresholds on every run, p1 to p3 x 3:
decode >= 49.0 tok/s, TTFT <= 80 ms, prefill <= 75 ms; GPC 1020 MHz and EMC 3199 MHz confirmed after the
runs. Plus `/ci` green (all steps of `.claude/commands/ci.md`) and 10/10 cold boots. Greedy text on p1 to
p3 must be byte-identical to main's unless the issue intends a numeric change (then compare against the
exact CPU reference). Performance is compared with current main measured the same way.

## Build

- `export PATH=/opt/arm-gnu-toolchain/bin:$HOME/.cargo/bin:$PATH`. One build at a time per worktree.
  QEMU only through make targets. Jetson build: `make kernel-clean && make kernel PLATFORM=JETSON_ORIN_NANO`;
  shaders come from host CUDA 12.0 (check the build log line `oplib-shaders: nvcc /usr/bin/nvcc (CUDA 12.0)`;
  a stub-shader build is invalid for any GPU measurement).
- `/ci` means every step in `.claude/commands/ci.md` of your worktree, run in order; record each step's
  pass line in your evidence.

## Branches, PRs, commits

- One branch and one PR per issue, named `gauntlet/<issue>-<slug>`. Open the PR as a DRAFT with the
  acceptance criteria in its body BEFORE writing code (push an empty commit to open it). Criteria may be
  made more specific, never weaker. Do not merge, force-push, rebase pushed history, or reopen an issue the
  user closed. To take upstream fixes, `git merge` (no rebase).
- `Fixes #N` only when the PR fully meets the issue's "Done when"; otherwise `Refs #N`. Never write
  fix/close/resolve next to an issue number you do not close, even negated.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. PR bodies end with
  `🤖 Generated with [Claude Code](https://claude.com/claude-code)`. No model names elsewhere.
- Shared files have one owner at a time. The GPU-chain files are `kernel/gpu/**`, `kernel/src/slm_ffi.c`,
  `kernel/include/slm_ffi.h`, `runtime/src/slm/gpu_program.rs`, `kernel/sched/sched.c`. Edit them only
  while the orchestrator has named you the GPU-chain owner.

## Evidence

- Put raw evidence in `/home/john/slmos-ref/derivatives/475-followups/<issue>/`: command lines and their
  full output (test logs, serial captures, benchmark tables), the diff stat, and a `MANIFEST.md` that maps
  each acceptance criterion to the files that support it. No reasoning or narrative in the evidence dir:
  critics must judge from raw outputs only.
- Report honestly. A test that was not run is "not run", with the reason.

## Writing

- No "you/your" in docs. ADRs in `docs/adr/`, under a page, with alternatives and reasons for rejecting
  them, no em-dashes. TODO markers per CLAUDE.md.
- Add regression tests with every change, and show that each new test fails without the fix (mutation or
  pre-fix run).
- File unrelated defects with `gh issue create` (labels, priority, milestone Backlog) and leave them alone.
  Link #475 defects to #475.
- Stop and report if the same failure recurs twice without a new strategy, or two rounds in a row show no gain.
