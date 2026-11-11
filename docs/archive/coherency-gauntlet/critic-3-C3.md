# Critic verdict, coherency run 3, component C3 (#1193, #1216)

Critic: `coherency-3-C3-critic`, 2026-11-11, 12:40 to 13:10 UTC.
Evidence read from `origin/gauntlet-3-hw-jetson` at 71634eb2 (builder's last push);
critic captures pushed in ef0caa43 under `run3-hw/critic-c3/`.
PR facts from `gh pr view 1222 --json headRefName,headRefOid,baseRefName,isDraft,state`:
head `coherency-nc-axis-pi5` e5b24894e740cf588e33a488da3e85a1ccd6dda6, base `main`, draft, OPEN, not merged.

## Blindness

Sections "Own checks" to "Failure-mode rows" were written before `coherency-3.md`, the PR #1222
body, issue comments and `run3-hw/c3/split-pi5-integration-rows.txt` (the builder's own table)
were opened. One leak, disclosed: `git log` on the evidence branch showed the builder's commit
subject c3b0a47b "Pi 5 integration at e5b24894 (7/10)" before the Pi 5 files were counted. The
count below was taken from the raw files; it happens to agree with that subject.

## Own checks, per definition-of-done row

### Row 1: PR and diff

`gh`: one draft PR, head `coherency-nc-axis-pi5`, base `main`, open, unmerged. e5b24894 is a
single commit on 7acee1fc (`git log`; `git merge-base --is-ancestor 7acee1fc` true).

`git diff --stat 7acee1fc e5b24894`: 16 files, +92/-130. Code beyond `ncmem.h`, judged:

- `kernel/include/ncmem.h`: `PLATFORM_HAS_NC_MEMORY` now needs
  `defined(PLATFORM_JETSON_ORIN_NANO) && !defined(NC_AXIS_OFF)`. This is the definition.
- `kernel/include/spinlock.h`: removes `SPINLOCK_DRAM_FLUSH` and its three DC CIVAC asm
  variants. At 7acee1fc it was defined only for `PLATFORM_RASPI5 && !NC_AXIS_OFF`, so it was a
  Pi 5 path that existed only for the NC axis; with Pi 5 never on the axis, the Pi 5 default
  build at e5b24894 takes the code a 7acee1fc Pi 5 `NC_AXIS=OFF` build took. Follows from the
  definition. Jetson and QEMU never took it, so no change there.
- `kernel/src/kprintf.c`, `kernel/include/sched_diag.h`, `runtime/src/msg_router.rs`: comment
  text only.
- `CMakeLists.txt`, `Makefile`: comments, the option description, and the Makefile example line
  (`RASPI5 NC_AXIS=OFF` replaced by `RASPI5 SETWAY_INVAL=OFF`). `NC_AXIS_OFF` is still defined
  for a Pi 5 `NC_AXIS=OFF` build, where it now changes only the `smp.c:741` info line. Harmless.
- Regression test: `kernel/tests/test_nc_axis.c` gains three `#error` guards (no
  `SPINLOCK_DRAM_FLUSH`; Pi 5 never defines `PLATFORM_HAS_NC_MEMORY`; Jetson defines it unless
  `NC_AXIS_OFF`). The runtime placement tests follow the macro; the critic's Pi 5 test boot
  shows `[nc_axis] CPU-shared state in cacheable memory` and 7/7 placement tests passing
  (`run3-hw/critic-c3/pi5-integration-run01.txt` lines 8156-8169).
- Docs: `CLAUDE.md`, `kernel/CLAUDE.md`, `runtime/CLAUDE.md`, `docs/fact-sheets/{memory,smp}.md`,
  `docs/smp.md`, `docs/scheduler.md`, `docs/work-stealing-bench.md` updated to the split; the
  integration-test NC-slot claim matches `test_integration.c` (gated on the macro, line 88).

No other behaviour change found. Row 1: agree.

### Row 2: /ci at the PR head

`run3-hw/c3/ci-e5b24894/head.txt` = e5b24894e740...; `status.txt` lists 11 steps rc=0
(01-build-arm64 through 11-test-work-stealing, 11:20:16 to 11:38:55) and DONE. The pushed directory
holds only these two files. Commit 71634eb2 is titled "/ci logs at e5b24894" but adds only
`head.txt` and `status.txt`: `git check-ignore -v` shows `.gitignore:39:*.log` excludes the step
logs. The 13 step logs exist unpushed in the session scratchpad (`.../scratchpad/c3/ci-e5b24894/`);
read there, they agree with green (runtime-test "308 passed; 0 failed" and "328 passed; 0 failed";
check-braces "0 violation(s), 0 file(s) unparsed", 12 configurations; Jetson UEFI layout "All checks
passed.", 0 `!` lines; the four `make test` logs "PASSED"). The row asks for the logs on
`origin/gauntlet-3-hw-jetson`, and they are not there. Row 2: content green, evidence incomplete.
The critic did not re-run `/ci` and did not push the builder's logs.

### Row 3: gate, macro-kept Jetson build, 10 + 10 boots

Own count from the 20 files in `run3-hw/c3/`:

| Path | Files | Banner `(e5b24894)` | Reached `slmos>` | Fault lines outside headers | `run queues in NC memory` |
|---|---|---|---|---|---|
| kexec | `split-kexec-boot01..10.txt` | 10/10 (build 20261111102554) | 10/10 | 0 | 10/10 |
| UEFI-direct | `split-uefi-boot01..10.txt` | 10/10 (build 20261111102554) | 10/10 | 0 | 10/10 |

Images: kexec ELF c794e298 on every trigger (`trigger-log.txt`, 10 "remote sha ok c794e298f271d574"
lines, `stage-log.txt`); UEFI `slmos.bin` 977bafa0 staged before each of the 10 boots (`uefi-log.txt`).
No `NC_AXIS=OFF` line in any file, and every boot logs its run queues in the NC block, so all 20
are the macro-kept build.

Completeness: every kexec file contains Linux's `kexec_core: Starting new kernel` before the
SLM-OS banner, so each covers the trigger to `slmos>`. Judged individually:
- boot01: two replies; reply 2 is only `cat /sys/version`. Capture began 10:28:15.755Z, trigger
  10:28:18Z. Complete.
- boot02: capture 1 from power-on stopped on OP-TEE's "TA panicked" line during Linux boot (the
  pattern included lowercase `panic`); capture 2 began 10:33:49.674Z, before the 10:34:06Z trigger,
  and runs to `slmos>`. Split before the trigger, disclosed in the header. Complete for the
  trigger-to-prompt window.
- boot09: capture began 10:58:10.788Z, 20 s after the 10:57:50Z trigger, but still inside Linux's
  shutdown (six dmesg lines before `kexec_core`). Late relative to the trigger, not relative to
  SLM-OS; accepted, noted.
- UEFI boots 01-10: each runs from the firmware banner ("Jetson System firmware version", 1 per file)
  to `slmos>`; boots 02-10 also include the preceding Linux reboot. Complete.

All kexec captures are power_cycle or trigger plus a separate serial_capture, each disclosed in
the header with "no other labctl caller". Row 3: 10/10 kexec, 10/10 UEFI-direct. Agree.

### Row 4: Pi 5 integration suite at the PR head (no pass bar)

Known set: the 18 `test_*` names failing in `run2-hw/c1/removal-step6-run01.txt`
(`origin/gauntlet-2-hw-pi5`): `test_builtin_commands_grouped_and_sorted`,
`test_every_command_has_help_entry`, 14 `test_blob_autoload_*`/`test_blob_boot_*`,
`test_eviction_run_tests_passes`, `test_timer_irq_is_physical`.

| File | Banner | Ends | Failures | Beyond the 18 | Missing of the 18 | slm_live / cleanup_ctx / task_lifecycle | `[test_wait]` TIMEOUT |
|---|---|---|---|---|---|---|---|
| run01 | e5b24894 | KERNEL PANIC, System halted | n/a | n/a | n/a | PASS / not reached / not reached | 1 (bench_context) |
| run02 | e5b24894 | 18 total failure(s) | 18 | none | 0 | PASS/PASS/PASS | 0 |
| run03 | e5b24894 | 18 total failure(s) | 18 | none | 0 | PASS/PASS/PASS | 0 |
| run04 | e5b24894 | KERNEL PANIC, then blank lines to timeout | n/a | n/a | n/a | PASS / not reached / not reached | 0 |
| run05 | e5b24894 | 18 total failure(s) | 18 | none | 0 | PASS/PASS/PASS | 0 |
| run06 | e5b24894 | KERNEL PANIC, then blank lines to timeout | n/a | n/a | n/a | PASS / not reached / not reached | 0 |
| run07 | e5b24894 | 18 total failure(s) | 18 | none | 0 | PASS/PASS/PASS | 0 |
| run08 | e5b24894 | 18 total failure(s) | 18 | none | 0 | PASS/PASS/PASS | 0 |
| run09 | e5b24894 | 18 total failure(s) | 18 | none | 0 | PASS/PASS/PASS | 0 |
| run10 | e5b24894 | 18 total failure(s) | 18 | none | 0 | PASS/PASS/PASS | 0 |

All files `run3-hw/c3/split-pi5-integration-runNN.txt`, build 20261111103233, image
`slmos-r3-c3-test-e5b24894.bin` (header sha 60cd278a), sources `r3-c3-pi5-int-a/run_01..05`
(runs 01-05) and `r3-c3-pi5-int-b/run_01..05` (runs 06-10).

Count: 7 of 10 complete (02, 03, 05, 07, 08, 09, 10), each with exactly the 18 known failures and
the three tests passing. 3 of 10 do not complete. All three stop at the same point: right after
`[PASS] test_rapid_task_exit_no_panic` and `CPU 1: returned to general scheduling`, i.e. inside
`test_isolated_core_latency`. Last lines:
- run01 (line 7335): garbled CPU 1 panic "KERNEL PANIC ... did not complete within timeout ...
  CPU: 1", ELR_EL1 0x8a048; output continues interleaved (`[test_wait] bench_context: TIMEOUT after
  1000000 us`, `test_timer_irq_is_physical` fail), then a second panic: "EL1 Synchronous Exception,
  EC 0x22 (PC alignment fault), ESR 0x8a000000, FAR/ELR 0xe00000000000089b", stack dump, "System halted."
- run04 (lines 7334-7346): "KERNEL PANIC ... tasks did not complete ... schedule returned!",
  "Task Name: ... (id=214) exiting", a second "KENNE PANIC" banner, then 1,833,418 blank lines to
  the 330 s timeout.
- run06 (lines 7304-7316): "KERNEL PANIC ... not complete within timeout ... core_latency ...
  'bench_b' (CPU4) exiting", a second panic banner, then 1,765,125 blank lines.

For comparison, the 7 completing boots and the critic's boot log "Skipped: cross-CPU tasks did not
complete within timeout" in `test_isolated_core_latency` and then pass on; the run 2 removal step-6
boot (`run2-hw/c1/removal-step6-run01.txt`) logs the same skip, and run 2's 9 removal step-6 boots
all completed (`run2-hw/c1/force-and-step6-boots.txt`). Whether the panics are new to e5b24894 is
not separated by any experiment here; stated as an observation only.

### Row 5: ADR 0007 status

`git log 59a85926..origin/fix/boot-path-1193`: one commit, fd2a361c. `git diff 59a85926
origin/fix/boot-path-1193`: one file, one line: "Status: proposed; the removal is pending on both
boards." becomes "Status: accepted for Pi 5 and QEMU; deferred for Jetson pending #1216;
implementation in PR #1222." Names the PR; nothing else changed. Agree.

### Row 6: restores

- Jetson ESP (`run3-hw/c3/esp-restore.txt`): SLMOS.efi 789cba62...a1f7d staged from
  `/root/slmos-efi-pre1193.efi` with `scripts/jetson-uefi-deploy.sh`, one boot of it
  (`esp-restore-boot.txt`), then from SD Linux at 11:53:48Z: no BootNext line, `fsck.vfat -n` rc=0,
  SLMOS.efi 789cba62, l4t.efi 91db497c, ESP unmounted. Matches the row. The ESP as found at C3's
  start (`esp-takeover.txt`, 10:25:46Z) held SLMOS.efi 405c7b43, not 789cba62; after the verdict
  was drafted, `coherency-3.md` (C2 hand-back entry) showed 405c7b43 is C2's cell F image and
  789cba62 the state C2 found, so 789cba62 is the run's as-found state. Resolved.
- Critic check after its own kexec boot (`run3-hw/critic-c3/jetson-restore-check.txt`, 12:49:16Z):
  root `/dev/mmcblk0p1`, uptime 2 min, `efibootmgr` shows no BootNext. ESP hash not re-read.
- pi-5-1 (`run3-hw/c3/pi5-restore-boot.txt`): banner b42b2c1d (the card's original), reaches
  `slmos>`. The critic's `sdwire_ls` at 12:50 (before its own deploy) showed kernel_2712.img and
  kernel8.img 6935256 B, mtime 2026-10-29T04:54:04Z, no leftover `*.prec3c3`; consistent with the
  builder's restore.
- Both boards were found powered on by the critic (health checks 12:43 and 12:49).
Row 6: agree.

## Reproduction

Lab held one board at a time, claims `coherency-3-C3-critic`, both released.

1. Jetson kexec, 1 boot (`run3-hw/critic-c3/kexec-boot01.txt`): critic-built
   `make kernel PLATFORM=JETSON_ORIN_NANO` at e5b24894, ELF sha256 7a5ee64b...028cba (build
   20261111124117; not byte-identical to the builder's c794e298 because the build timestamp is
   embedded). scp to `/root/slmos-r3-c3critic.elf`, `sync`, remote sha equal. Trigger 12:43:52Z,
   detached `slmos-kexec` (see breaks). Captured from Linux's last dmesg lines through
   `kexec_core: Starting new kernel` to `slmos>` in 99.6 s; banner e5b24894; 6 CPUs online; "SMP: run
   queues in NC memory at 0xbde0d300"; `cat /sys/version` commit e5b24894. Power-cycled back to SD
   Linux (`jetson-restore-check.txt`). Result 1/1, agrees with row 3.
2. Jetson UEFI-direct: not run, so the restored ESP stays as the builder left it.
3. pi-5-1 integration suite, 1 boot (`run3-hw/critic-c3/pi5-integration-run01.txt`): critic-built
   `make kernel-test PLATFORM=RASPI5` at e5b24894, `slmos-r3-c3critic-test.bin` sha256
   33d1ca71...4f43, staged temp-name-then-rename; power_off, `sdwire_info`, `sdwire_ls`, originals
   renamed `*.prec3cc`; `boot_test` runs=1, timeout 330 s: completed in 152.7 s with
   "[FAIL] 18 total failure(s)", exactly the 18 known names, the three tests PASS
   (`slm_live` 9768 us, `c4_multi` 10597 us, `task_lifecycle` met after 0 us), no TIMEOUT. Card
   restored and booted once to `slmos>` on b42b2c1d (`run3-hw/critic-c3/pi5-restore-boot.txt`).
   Including this boot, 8 of 11 Pi 5 integration boots at e5b24894 complete.
4. Not done: a `/ci` re-run; a Jetson UEFI-direct boot; more than one boot per path.

## Disclosed process breaks

- Builder: every kexec boot is a power_cycle or trigger followed by a separate serial_capture,
  disclosed in each header as "no other labctl caller". `esp-check.sh` (fsck -n, read-only mount,
  sha256, efibootmgr) is an extra ssh helper beyond the two documented deploy paths; disclosed in the
  UEFI file headers and `esp-restore.txt`, read-only, does not set BootNext. Recorded, not failed.
- Critic: the kexec trigger wrapped `slmos-kexec` in `setsid nohup sh -c 'sleep 10; exec ...'` so the
  separate serial_capture could start first; power_cycle with a separate check; two read-only ssh
  commands after the boot (`uptime; findmnt; /proc/cmdline` and `efibootmgr; systemctl is-active
  slmos-autokexec`) beyond the documented path; `power_off` of pi-5-1 so `sdwire_info` could run. No
  other component was calling labctl during any of these.

## Failure-mode rows

Run 1:
- Selective summary: files counted, not rows; 10+10 Jetson and 10 Pi 5 files all counted, the 3
  non-completing Pi 5 boots included above; the builder's rows file and PR body carry all 10. Clear.
- Truncated capture: no `tail`-cut captures; runs 04 and 06 carry their full blank tails. Clear.
- Assembled register file: no register claims in C3's gate. Not applicable.
- Redefined criterion: gate applied as written (10/10 per path). Clear.
- Hypothesis stated as finding: the critic states no cause for the Pi 5 panics. The PR body states
  none either. Log Entry 15 calls #1223 "same family as #1213" with no separating experiment named;
  that is the orchestrator's text, not C3's evidence, and is read here as a hypothesis.
- Stale base: all 30 builder files and both critic boots carry e5b24894. Clear.

Run 2 additions:
- No critic: this file. Not applicable.
- Per-CPU table rebuilt from raw: C3 has no per-CPU decisive table in its rows; the critic's Jetson
  boot shows all 6 CPUs online. Not applicable beyond that.
- Number without a filename: every number in this file names its file. The PR body's `/ci` numbers
  (runtime-test 308 and 328, check-braces 0 violations and 12 configurations) cite
  "`ci-e5b24894/status.txt` and logs"; `status.txt` carries none of them and the logs are not
  pushed. Struck until the logs are pushed. This is the row 2 finding.
- Observation tree never committed: Jetson and Pi 5 captures pushed at 71634eb2, critic files at
  ef0caa43; the `/ci` step logs are not committed (`.gitignore` `*.log`). Same finding as above.
- Critic read the builder's claim first: no; the commit-subject leak is disclosed above.
- Labctl database or private BootNext helper: none seen; BootNext only through
  `scripts/jetson-uefi-deploy.sh` (`uefi-log.txt`); `esp-check.sh` disclosed and read-only.
- Stale head: banners checked in every file before counting. Clear.

## Builder's claims, read after the sections above

Read at 12:59 to 13:01 UTC: `run3-hw/c3/split-pi5-integration-rows.txt`, `coherency-3.md`
Entries 13 to 15, the PR #1222 body. The row 2 log check (`git check-ignore`, the scratchpad
logs) and the ESP 405c7b43 resolution in row 6 were done after this reading, prompted by it.

| Claim (quoted) | Source | Critic | Agree |
|---|---|---|---|
| "Jetson kexec, production kernel, boots to `slmos>`: 10/10" | PR body | 10/10, row 3 | yes |
| "Jetson UEFI-direct ... 10/10, ESP fsck rc=0 after each of the 10 writes" | PR body | 10/10 boots, row 3; fsck rc=0 seen in `uefi-log.txt` for the checks read | yes |
| "Pi 5 integration suite (test kernel): 7/10 completed; the 7 show 18 failures each, all among the known #1109 ones, the three named tests passing, 0 `[test_wait]` timeouts; 3/10 kernel panics" | PR body | 7/10; same three runs (01, 04, 06); same 18; run01 also has one `[test_wait]` TIMEOUT (bench_context), which the rows file records and the PR's "0" correctly limits to the 7 | yes |
| "Runs 01, 04 and 06 ended in `KERNEL PANIC` right after `[INFO] CPU 1: returned to general scheduling (SPIs restored)` ...; run 01 printed `System halted.`" | PR body | same, row 4 | yes |
| Rows file: runs 01/04/06 marked "NO", fails column "0", missing "1 (test_timer_irq_is_physical)" for 04 and 06 | `split-pi5-integration-rows.txt` | the "0" is a count of a run that never printed a total, not zero failures; the NO column makes that clear. Missing count matches the critic's | yes |
| "/ci: steps 1 to 10 passed, plus `make test WORK_STEALING=ON`; runtime-test 308 and 328 passed; check-braces 0 violations, 0 unparsed, 12 configurations; Jetson UEFI layout `All checks passed.`" with files "`ci-e5b24894/status.txt` and logs" | PR body | true by the unpushed logs; not backed by any pushed file | content yes, evidence no |
| "On Jetson this head changes no compiled code: every code change in e5b24894 is under a Pi 5 condition (`PLATFORM_RASPI5` in `ncmem.h` and `spinlock.h`) or in comments" | PR body | inference holds (the `ncmem.h` test evaluates as before on Jetson, `SPINLOCK_DRAM_FLUSH` was Pi 5 only), but the `ncmem.h` condition names `PLATFORM_JETSON_ORIN_NANO`, not `PLATFORM_RASPI5`; wording inexact, stated as inference, binaries not compared | yes, wording note |
| "#1223 ... 3 of 10 integration boots ...; not seen on the NC kernel or the full removal (20 of 20 each)" | `coherency-3.md` Entry 15 | 3 of 10 agrees; the 20-of-20 comparison was not rechecked by this critic | partly checked |
| Pi 5 20/20 bar "measured on the full removal 79c10261 ... This head's Pi 5 evidence is the 7/10 above" | PR body | the PR states the count faithfully and marks itself "Not ready for review" on it | yes |

Row 4 is stated faithfully in every C3 summary read (PR body, rows file, log Entry 15).

## Verdict

**fail-on-completeness**, definition-of-done row 2, file `run3-hw/c3/ci-e5b24894/` on
`origin/gauntlet-3-hw-jetson` @ 71634eb2: only `head.txt` and `status.txt` are pushed; the step logs
the row requires are excluded by `.gitignore` `*.log`, so the PR body's `/ci` numbers name no pushed
file. The logs exist and read green; pushing them with `git add -f` (from the session scratchpad
`c3/ci-e5b24894/`) closes the row with no change to the result.

Rows 1, 3, 5 and 6 pass. Row 4 (no pass bar): 7 of 10 builder boots complete, 8 of 11 with the
critic's boot; the 3 panics (runs 01, 04, 06) are reported in full and faithfully summarised by
the builder. Reproduction agrees: Jetson kexec 1/1 at e5b24894 (`run3-hw/critic-c3/kexec-boot01.txt`),
Pi 5 integration 1/1 complete with the 18 known failures (`run3-hw/critic-c3/pi5-integration-run01.txt`);
UEFI-direct not reproduced. Both boards left powered on: jetson-nano-2 at SD Linux with no BootNext,
pi-5-1 on its original card at `slmos>`. Claims released.
