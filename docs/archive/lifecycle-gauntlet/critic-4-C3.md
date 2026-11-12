# Critic verdict, lifecycle run 4, component C3 (#1199, #1224; related #1109, #1207, #1228, #1231)

Critic: `lifecycle-4-c3-critic`, model **Fable 5.2** (`claude-fable-5-2`), 2026-11-12, 06:58 to 07:50 UTC.
Heads: main 6c1f4c9c (= `origin/main` and the merge base at 07:46 UTC); C3 branch
`origin/c3/jetson-suite` 8a3406ea27e1045714be94b01db5e824fea6952b (5 commits on main).
Evidence read from `origin/gauntlet-4-hw-jetson`: counted at ed786bd2, then re-fetched at c9c194fc
(one added file, `pi5-boot-test-8a3406ea.txt`). Path `docs/archive/lifecycle-gauntlet/run4-hw/c3/`.
Critic captures: `.../scratchpad/critic-c3-runs/` (listed under "Reproduction").
`lifecycle-4.md` was not opened. No PR exists for `c3/jetson-suite` (`gh pr list --head`: empty).

**Verdict: FAIL** on the definition-of-done row "the suite reaches its last test in 10 of 10 boots
on kexec" (9 of 10; `final-8a3406ea-kexec-suite-run06.txt`), and **fail-on-completeness** on two
further rows (draft PR not opened; /ci run at d512f40b, not at the PR head 8a3406ea). The two root
causes, both fixes, the UEFI-direct 10 of 10, the production boots and the issue accounting of the
24 failing checks are confirmed.

## Blindness

Sections "Fix review" and "Own counts" were written from the diff, the critic's builds and the raw
captures before the builder's commit messages and issue comments were opened. Leaks, disclosed:
line 1 of every evidence file is the builder's RESULT summary, and those lines were seen while
listing files (repro files, `final-8a3406ea-kexec-suite-run01/06`, `test-1199-*`, `build-guard-1199`)
before the critic's own count of each file was complete. Every number below was recomputed from
the capture bodies (lines 3 onward) or from the critic's own builds; where a recount includes the
header line it says so. The evidence branch commit subjects ("9 to the end, 1 test_pmm halt") were
seen after the recount.

## Fix review (before commit messages)

### (a) #1199, Lua `memchr` without a prototype

- Capture on main: `repro-main-6c1f4c9c-kexec-run02.txt`: PAGE FAULT (READ) at 0xffffffff827a8e29,
  ELR 0x8005f134, x19 0x827a8e28, x27 0xffffffff827a8e28, banner `(6c1f4c9c)`. The file's own
  disassembly shows `bl memchr` at 80223080 followed by `sxtw x27, w0` at 80223088.
- Reproduced by the critic, not taken from that file: main 6c1f4c9c built in a critic worktree
  (QEMU_VIRT test kernel, elf sha256 25cf3f55...) has `bl <memchr>` followed by `sxtw x0, w0`,
  `sxtw x28, w0`, `sxtw x27, w0` and `cbz/cbnz w0` at the four lstrlib.c call sites. At 8a3406ea
  (elf 2c05c4f6...) the same sites are `cbnz x0`, `mov x28, x0`, `mov x20, x0`: no `sxtw`.
  main's Jetson production build log has 3 implicit-declaration warnings, all in the Lua target
  (lauxlib.c:255, lstrlib.c:681, lvm.c:384), which match the three prototypes added (strerror,
  memchr, strcoll); the 8a3406ea Jetson test build has 0.
- Fix: `kernel/include/string.h` declares `memchr`, `strcoll`, `strerror`; `CMakeLists.txt` adds
  `-Werror=implicit-function-declaration -Werror=int-conversion -Werror=int-to-pointer-cast` to
  the Lua target in both the x86 and the non-x86 branch. Correct for the mechanism, and the flag
  closes the class for Lua. Production impact that the issue title hides: the Lua library is in
  the production kernel too, so `string.find(..., true)` and `gsub` were broken on Jetson
  production, not only in the test kernel.
- Test fails on the old code: main 6c1f4c9c plus only `kernel/tests/test_lua.c` from 8a3406ea,
  `make test` (Makefile QEMU guard), one run: banner `(6c1f4c9c-dirty)`, `[PASS] test_lua_strings`,
  then PAGE FAULT at 0xffffffffc00eabd1, ELR 0x400443f4, System halted, `make test` rc=2, 2275
  [PASS] lines (`critic-c3-runs/qemu-make-test-main-6c1f4c9c-plus-new-test.log`). At 8a3406ea, one
  run: `[PASS] test_lua_string_find_above_2gb`, "All test suites passed!", 3016 [PASS], 0 [FAIL],
  18 [IGNORE], rc=0 (`critic-c3-runs/qemu-make-test-8a3406ea.log`). Note: on the old code the test
  "fails" by halting the suite (a fault), not by a [FAIL] line.
- Row (a): **PASS**.

### (b) #1224, orphan `.got` past `__data_end`

- main's Jetson image (critic build, production, elf e73ec823...): `.data` [0x80880000, 0x808a4400),
  `__data_end` = 0x808a4400, `.got` at 0x808a4400 size 0x30, `.got.plt` at 0x808a4430 size 0x18,
  `.bss` from 0x808b0000. `_kernel_file_size` = 0x8a4400. The PE `.data` header in `slmos.bin` has
  SizeOfRawData 0x24400 at file offset 0x880000 (ends 0x8a4400) while the file is 0x8a4448 long.
  `boot.S:353` copies `_kernel_file_size` bytes; `.bss` clear starts at `__bss_start`. So after a
  relocating UEFI-direct boot [0x808a4400, 0x808b0000) is never written. The `.got` entries resolve
  (nm) to `hailo_fw_end`, `mock_camera_frame_start`, `hailo_control_signal_driver_shutdown`,
  `hailo_fw_start`, `mnist_onnx_start`/`mock_camera_frame_end`. Mechanism confirmed. It applies
  to the production image as well (the `.got` is in the production ELF).
- Fix: `*(.got .got.plt .igot .igot.plt)` inside `.data`, before `. = ALIGN(0x200); __data_end = .;`
  in `kernel-jetson.ld` and `kernel.ld`. At 8a3406ea (critic build, elf 79820dc7...) there is no
  separate `.got` section, `_GLOBAL_OFFSET_TABLE_` = 0x808a4278 (inside `.data`, below `__data_end`
  0x808a4400), entries populated with link-time absolute addresses (static non-PIC link, no
  dynamic relocations needed, image runs at its link address). 8-byte alignment holds (`.data`
  Al 8). The existing `ASSERT((__data_end - __data_start) % 0x200 == 0)` still holds. No other
  output section changed. `kernel-raspi5.ld` and the three x86 scripts are untouched.
  Residual, not verified by the critic: `kernel-raspi5.ld` also derives `_kernel_file_size` from
  `__data_end` and has no `.got` rule; the new check 12 is not run on a Pi 5 image.
- Test fails on the old image: `LAYOUT_ELF=<main 6c1f4c9c Jetson elf> scripts/test-jetson-uefi-layout.sh`
  (script from 8a3406ea): rc=1, exactly two `✗` lines (`.got [0x808a4400, 0x808a4430)` and
  `.got.plt [0x808a4430, 0x808a4448)` past `__data_end`), all older checks pass
  (`critic-c3-runs/layout-check-on-main-6c1f4c9c-prod-image.log`). At 8a3406ea the full script
  (builds the kernel): "no section with contents past __data_end", "All checks passed.", rc=0
  (`critic-c3-runs/layout-check-8a3406ea.log`).
- Runtime: `test_camera_open_mock` is [PASS] in both critic UEFI boots and in 10 of 10 builder UEFI
  files; it is [FAIL] followed by a fault in `repro-main-6c1f4c9c-uefi-run01..03.txt`.
- Row (b): **PASS**.

### (c) Camera changes (12079f3d, 8e3d6cc2, 8a3406ea)

- `imx219_power_on()` and `imx219_capture_one_frame()` return -1 first thing when
  `boot_source_is_uefi()`; nothing else in the functions changed. kexec behaviour is unchanged:
  the kexec failing set at 8a3406ea is exactly #1109's 15 names, and
  `test_tegra_i2c_dump_status_bounds` is [PASS] on kexec (critic run 02 and the 9 complete builder
  kexec files).
- `test_tegra_i2c_dump_status_bounds` uses `TEST_IGNORE_MESSAGE` under `boot_source_is_uefi()`;
  the UEFI captures print `[IGNORE] test_tegra_i2c_dump_status_bounds - UEFI-direct boot: camera
  I2C controller not powered` (critic UEFI run 02 line 7294 of the raw capture; 51 [IGNORE] on
  UEFI against 50 on kexec). Honestly reported.
- Weak test: `test_camera_imx219_refused_after_uefi_boot` asserts `camera_open("imx219-0") == -3`,
  the code `camera_open` returns for any failed capture. It cannot tell a refusal from a hardware
  failure, so on a kexec boot of the old code it would pass whenever the capture fails for another
  reason; the builder's own interim file shows it passing at 8e3d6cc2 while bringing RCE up
  (`interim-8e3d6cc2-uefi-suite-run01.txt`). The test also writes the global `slmos_boot_source`
  on a live SMP system for the duration of the call.
- Incomplete refusal (code reading, not run on hardware, so a hypothesis): `cmd_csidiag`
  (`kernel/src/shell_sys.c:12831`) calls `camera_open("imx219-0")` and then, unconditionally at
  line 12923, `imx219_streaming_disable()`, which writes to the camera I2C controller through
  `tegra_i2c_write_reg16` with no boot-source check. By the builder's own two interim captures
  (EL3 exception and RAS error on the first access to 0x03180000 after UEFI-direct), `csidiag`
  after a UEFI-direct boot is expected to stop the board on the production kernel. `shell_sys.c`
  is unchanged by C3, and before C3 the same command stopped earlier, so this is not a regression,
  but "refuse cleanly" does not cover it and no test or production boot exercises it.
- "Nothing powers the camera I2C controller" is stated as fact in the code comments, the commit
  messages and #1224's comment. The captures show the fault address (RAS `ADDR = 0x8000000003180040`)
  and the EL3 exception; the power or clock state was not read. Reasonable, but inferred.
- No issue tracks the missing camera I2C cold bring-up after UEFI-direct (searched "imx219 UEFI",
  "camera I2C UEFI-direct": only #1224). The two board stops are mentioned in a #1224 comment.
  `docs/deploy/jetson-uefi-direct.md` gained the GOT item but no line saying imx219 capture is now
  refused after a UEFI-direct boot.
- Row (c): **PASS** on the three questions asked (guard correct, kexec unchanged, skip reported),
  with the findings above.

## Own counts from C3's files (capture bodies only)

### Integration suite at 8a3406ea

| Path | Files | Banner `(8a3406ea)` | Reached `test_work_stealing_distributes_load` + Test Suite Summary | [PASS] / [FAIL] lines | Failing checks |
|---|---|---|---|---|---|
| kexec | `final-8a3406ea-kexec-suite-run01..10.txt` (elf f193a8e3 in all 10) | 10/10 (build 20261112033116) | **9/10** (not run06) | 3033 / 22 in the 9 | 15, all in #1109 |
| UEFI-direct | `final-8a3406ea-uefi-suite-run01..10.txt` (bin fbb29127 in all 10) | 10/10 | **10/10** | 3021 / 33 in all 10 | 22 = 13 of #1109 + 9 of #1231 |

The ordered list of every `[PASS]/[FAIL]/[IGNORE]` line (3100 lines) is identical between the
critic's kexec capture and each of the 9 complete builder kexec files, and between the critic's
SASS UEFI capture and each of the 10 builder UEFI files (diff = 0 lines, 19 comparisons).

Halt: `final-8a3406ea-kexec-suite-run06.txt`: PAGE FAULT (WRITE) at 0x1000008096ada7, ESR
0x96000044, ELR 0x80024b6c, x30 0x800fca30, task main CPU 0, in `[TEST] PMM Buddy Allocator Tests`
after `[PASS] test_checkerboard_fragmentation`, System halted; 1065 of 3100 status lines printed.
Recorded on #1207 (comment 2026-11-12T06:26:57Z). Not in code C3 touched (C3 touches no mm file;
on Jetson the new Lua test uses its static arena in place and maps nothing). Same signature family
on unmodified main (`repro-main-6c1f4c9c-kexec-run01.txt`, 1 of 3, #1228) and at d512f40b
(`final-d512f40b-kexec-suite-run05.txt`, 1 of 10, fault in `pmm_alloc_pages_low`). So: pre-existing,
filed, roughly 1 in 10 kexec boots across this run's files (main 1/3, d512f40b 1/10, 8a3406ea 1/10;
0/10 on UEFI-direct; 0/1 in the critic's full kexec boot).
The bar as written: "may be filed rather than fixed, but the suite must run past it". On run06
the suite did not run past it. **Bar not met on kexec: 9 of 10.** The builder's files say the same
(run06 header: "did NOT complete (counts as 1 failure in the 10)"); the criterion was not redefined.
Whether this is a "test-kernel defect" at all is open: the faulting values (0x0010_0000_8096_ada7,
0x0010_0000_8091_ae67) look like a corrupted free-list link, #1228 is P1-high, and #1207/#1228 call
the common cause a hypothesis.

### Failing checks against issue numbers

- #1109 lists 15 names for Jetson kexec. All 15, and nothing else, fail in the critic's kexec boot
  and in each of the 9 complete builder kexec files; run06 shows the first 10 of them before the halt.
- #1231 lists 9 names. All 9 fail in the critic's SASS UEFI boot and in 10 of 10 builder UEFI files,
  with 13 of #1109's 15 (`test_coalesce_enables_larger_allocation` and `test_recursive_coalescing`
  pass on UEFI-direct). In neither issue: 0 in every file.
- New observation from the critic's first UEFI boot (kernel without SASS): only 4 of #1231's 9 fail
  and the three `test_slm_gpu_*_off_jetson` checks of #1109 pass. The failing set on UEFI-direct
  depends on whether the operator library is embedded; `make kernel-test PLATFORM=JETSON_ORIN_NANO`
  in a fresh worktree embeds none (`ensure-oplib-shaders.sh` runs only under `make kernel`), so the
  command in the builder's headers does not by itself reproduce the builder's image. Not in #1231.

### Production boots at 8a3406ea (the Jetson `boot_test --count 10` substitute)

| Path | Files counted | Banner `(8a3406ea)` | `slmos>` reached | Fault lines in body |
|---|---|---|---|---|
| kexec | boot01, boot03..boot11 (elf 92fdc3e8) | 10/10 (build 20261112033313), `Boot source: direct` | 10/10 | 0 |
| UEFI-direct | boot01..boot10 (bin cb2e7160) | 10/10, `Boot source: UEFI-direct (EFI stub)` | 10/10 | 0 |

`prod-8a3406ea-kexec-boot02-TAILONLY.txt` has no banner (capture taken with tail 3), is labelled
NOT COUNTED and replaced by boot11. Every counted file states that labctl `boot_test` cannot kexec
or set BootNext and that power_cycle, trigger and capture were separate calls; UEFI boots 02..10
used a private `unext.sh` (ESP sha check, BootNext, reboot), disclosed in each header. `boot_test`
itself was not used on Jetson.

### Pi 5

Required: the PR changes `kernel/include/string.h` and the Lua compile flags in the non-x86 CMake
branch, and the ARM64 Lua objects change (the `sxtw` removal above). Evidence
`pi5-boot-test-8a3406ea.txt` (added at 5833ae8d/c9c194fc, after the critic's first fetch): labctl
`boot_test` reply 10/10 PASS (12.6 to 12.8 s), 10 per-run captures each with banner `(8a3406ea)`
(build 20261112065724) and one `slmos>`; card restore described. Content: agree, 10/10. Process:
the per-run captures were copied from `/var/lib/labctl/output/l4c3-pi5-8a3406ea/`, disclosed on
line 3 of the file. The restore line ("booted commit b42b2c1d") quotes no capture.

### /ci

`ci-d512f40b.txt` and `ci/summary.txt`: head d512f40be899..., 12 steps, 11 rc=0; step 05
(`make test AI_SCHED=ON`) rc=2 on `test_litmus_patterns_pass`, rerun 05r rc=0 (attributed to #1230).
Only the rc summary is committed, no step logs. The PR head is 8a3406ea: three later commits change
`kernel/drivers/camera/imx219.c` and `kernel/tests/test_camera.c`. **/ci was not run at the PR head.**
Partial critic runs at 8a3406ea (not a substitute): `make test` rc=0, `make test PLATFORM=X86_64`
rc=0, `make check-braces` 0 violations, `scripts/test-jetson-uefi-layout.sh` rc=0, Jetson test
kernel builds (logs in `critic-c3-runs/`). Not run by the critic: AI_SCHED=ON, runtime-test,
test-qemu-uefi, build-order, the `AI_WEIGHTS=real` Jetson builds.

## Reproduction (critic, jetson-nano-2, test kernel at 8a3406ea, critic worktree)

| Run | File in `.../scratchpad/critic-c3-runs/` | Path | Image | Result |
|---|---|---|---|---|
| 1 | `critic-8a3406ea-kexec-run01-TAILONLY.txt` | kexec | elf 7ce205c5 (no SASS) | NOT COUNTED: capture began about 3.5 min late (1484 lines, no banner); tail shows `[FAIL] 14 total failure(s)` and main exiting |
| 2 | `critic-8a3406ea-kexec-run02.txt` | kexec after power_cycle | elf 7ce205c5 (no SASS), board sha256 matched | banner `build 20261112065830 (8a3406ea)`; 3033 [PASS], 22 [FAIL], 50 [IGNORE] lines; last test `[PASS] test_work_stealing_distributes_load`; `[FAIL] 14 total failure(s)`; no fault |
| 3 | `critic-8a3406ea-uefi-run01-noSASS.txt` | UEFI-direct, BootNext 000E | bin 21b4d47f (no SASS) | banner `(8a3406ea)`, `Boot source: UEFI-direct (EFI stub)`; 3030 / 24 / 51; last test reached; `[FAIL] 13 total failure(s)`; no fault; not the builder's configuration |
| 4 | `critic-8a3406ea-uefi-run02-SASS.txt` | UEFI-direct, BootNext 000E | bin e9afd85c, elf 4e83bc1c (SASS, 12211464 B like the builder's) | banner `build 20261112072235 (8a3406ea)`; 3021 / 33 / 51; last test reached; `[FAIL] 21 total failure(s)`; no fault |

Every test name: 3100 status lines per complete capture, 2589 of them `test_*` names; lists saved
as `n-ck.txt` (kexec) and `n-cu.txt` (UEFI) in the scratchpad. New tests seen:
`test_lua_string_find_above_2gb` [PASS] on both paths; `test_camera_imx219_refused_after_uefi_boot`
[PASS] on both; `test_camera_open_mock` [PASS] on both; `test_tegra_i2c_dump_status_bounds` [PASS]
on kexec, [IGNORE] on UEFI; `test_exhaustion_recovery` and `test_runner_parks_without_a_lock`
[PASS] on both.

Failing checks, kexec run 2 (15, all #1109): test_gpu_jetson_uses_stub_driver, test_gpu_get_info_valid,
test_slm_gpu_run_mnist_returns_negative_off_jetson, test_slm_gpu_set_mnist_input_returns_negative_off_jetson,
test_slm_gpu_set_mnist_input_fill_returns_negative_off_jetson, test_builtin_commands_grouped_and_sorted,
no help entry for built-in 'nvcsi', test_every_command_has_help_entry,
test_coalesce_enables_larger_allocation, test_recursive_coalescing, test_multiple_cores_isolated,
test_timer_irq_is_physical, test_round_robin_distributes_tasks, test_net_driver_registered,
gpu: capabilities detect valid.

Failing checks, UEFI run 4 (22): the 13 of #1109 without the two coalesce tests, plus #1231's 9:
test_oplib_pool_get_sass_gpu_va_unstaged_returns_null, test_oplib_pool_gpu_va_base_zero_when_unstaged,
test_oplib_pool_scratch_base_unstaged, test_boot_source_default_is_direct, test_no_models_means_no_mount,
test_slm_launch_defaults_to_gpu_auto, test_slm_launch_gpu_on_requires_a_program,
test_session_open_failures, test_unavailable_reason_names_the_cause.

Board handling: recovery_mode binding `released`, relay open, before the first boot. ESP as found
(`esp-asfound-critic.txt`): fsck clean, `EFI/L4T/l4t.efi` 91db497c, `EFI/SLMOS/SLMOS.efi` 789cba62
(8864840 B), identical to the builder's `esp-asfound.txt`. Two ESP writes for the boots, each
followed by `fsck.vfat -n` rc=0 and a fresh read-only mount sha256; two restores of the as-found
image after a power cycle, each followed by fsck rc=0 and a fresh-mount listing identical to the
as-found one (in run files 3 and 4). No power cycle while mounted. `/root/slmos-l4-c3critic.elf`
removed; the `/root/*.elf` list (45 files) is identical to as found. First release 07:21:22 UTC,
second claim 07:24:15, final release **07:31:37 UTC**, powered on, SD Linux (BootCurrent 0001).

## Builder's claims, agree or not

| Claim (file header, issue comment or commit) | Critic |
|---|---|
| #1199: missing `memchr` prototype, `sxtw` after `bl memchr`, heap above 0x80000000 | agree (own disassembly of main and of 8a3406ea) |
| New Lua test faults on the old code under QEMU at 0xffffffffc00eabd1 | agree (same address in the critic's run) |
| #1224: orphan `.got` after `__data_end`, not copied on UEFI-direct | agree (own readelf, PE header, boot.S) |
| Layout script fails on main's image | agree (rc=1, two errors, on main's production image) |
| kexec suite at 8a3406ea: 9 to the end, run06 halted in test_pmm (#1207) | agree; this is 9/10, bar not met |
| UEFI suite at 8a3406ea: 10/10, 3021 [PASS], 33 [FAIL], 22 checks | agree |
| kexec: 3033 [PASS], 22 [FAIL], 15 checks (#1109) | agree (the `grep -c` over a whole file gives 3034/23 because line 1 is counted) |
| Production boots 10 + 10 at 8a3406ea | agree |
| pi-5-1 boot_test 10/10 at 8a3406ea | agree on content |
| /ci green | at d512f40b only, one step green on rerun; not at the PR head |
| #1228 comment: "0 of 11 kexec suite boots at c3/jetson-suite ... reached the same signature" | the run06 fault at 8a3406ea has the same address shape, as #1207's later comment itself says; the #1228 comment predates run06 and is now out of date |

## Failure-mode rows

| Mode | Finding |
|---|---|
| Selective summary | Not found. Headers list every failing check; the halts at d512f40b run05 and 8a3406ea run06 are in the headers, the commit subject and #1207. |
| Truncated capture | `prod-8a3406ea-kexec-boot02-TAILONLY.txt`, disclosed and replaced. Critic run 1 truncated, not counted. |
| Assembled register file | No register table in C3. Capture files are assembled by the builder's scripts from saved tool results (one from the session transcript, `lastcap.py`); bodies carry labctl's own `[Captured N lines ...]` line. |
| Redefined criterion | Not found in the files. The Jetson "boot_test equivalent" is a disclosed substitution. The gate must not read 9/10 as met. |
| Hypothesis stated as finding | "camera I2C controller is not powered" (code comments, commits, #1224 comment): inferred from the fault address. #1231 and #1207 label their hypotheses. |
| Stale base | No: merge base 6c1f4c9c = `origin/main`. |
| Number without a file | /ci: rc summary only, no step logs. Pi 5 restore check quotes no capture. |
| Uncommitted tree | No: 8a3406ea is pushed; `-dirty` banners appear only in the two old-code test files, as labelled. |
| Banner hash mismatch | None: 60 capture files checked, body banner = head in the file name in each; Pi 5 file 10/10 `(8a3406ea)`. |
| Bar computed from one cell | No: 10 files per path, each recounted. |
| Path never exercised | UEFI-direct: `test_tegra_i2c_dump_status_bounds` ignored; real imx219 open refused, so no camera hardware path runs after UEFI-direct; `csidiag` after UEFI-direct not run (see (c)). Production boots stop at the prompt. Critic kexec boot used the no-SASS image. |
| Undisclosed rule break | None found. Disclosed by the builder: read of `/var/lib/labctl/output/...`; private `unext.sh` BootNext helper; pi-5-1 claimed 06:57:41, inside the critic's Jetson window (the likely cause of the critic's late first capture, since labctl serializes calls; inference). |

Critic's own deviations: board address taken from the builder's `esp-asfound.txt` (no tool to read
`lab://sbcs`); ESP writes and BootNext done with critic scripts that repeat
`scripts/jetson-uefi-deploy.sh` step for step but split it so that `fsck.vfat -n` and a fresh-mount
sha256 run before the reboot; a second claim (07:24 to 07:31) for the SASS UEFI boot; `make
kernel-test` alone produced a no-SASS image, so run 4's image needed `ensure-oplib-shaders.sh` first.
Worktrees removed; no branch edited; no QEMU left running.

## Per row

| Definition-of-done row | Verdict | File |
|---|---|---|
| Both root causes written from captures | PASS | `repro-main-6c1f4c9c-kexec-run02.txt`, `repro-main-6c1f4c9c-uefi-run01.txt`; critic disassembly and readelf |
| Fixes with tests that fail on the old code | PASS | `critic-c3-runs/qemu-make-test-main-6c1f4c9c-plus-new-test.log`, `layout-check-on-main-6c1f4c9c-prod-image.log` |
| Suite to its last test, kexec 10 of 10 | **FAIL (9/10)** | `final-8a3406ea-kexec-suite-run06.txt` |
| Suite to its last test, UEFI-direct 10 of 10 | PASS (10/10) | `final-8a3406ea-uefi-suite-run01..10.txt` |
| Every failure accounted for by an issue number | PASS for the 24 checks and the halt (#1109, #1231, #1207/#1228); gap: no issue for the unpowered camera I2C controller after UEFI-direct | captures above |
| Draft PR | INCOMPLETE (none opened) | `gh pr list --head c3/jetson-suite` |
| /ci at the PR head | INCOMPLETE (run at d512f40b; one step green on rerun; no logs) | `ci-d512f40b.txt` |
| boot_test 10 on both Jetson paths at the PR head | PASS (10/10 each, by separate calls, disclosed) | `prod-8a3406ea-*` |
| boot_test 10 on Pi 5 | PASS on content (10/10); disclosed read under `/var/lib/labctl` | `pi5-boot-test-8a3406ea.txt` |
| Critic booted one per path and read every test name | PASS | `critic-c3-runs/critic-8a3406ea-kexec-run02.txt`, `critic-8a3406ea-uefi-run02-SASS.txt` |
