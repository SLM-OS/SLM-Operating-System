# Known failures 1: every accepted suite failure gets an explanation and a home

Date: 2026-11-14. Branch: `structural/known-failures-1` (from main dc51107c).

Scope: the integration suite (the test kernel) run on hardware carries 14 to 21
failures per boot that are accepted as "expected" (#1109, #1231). This session
read each one against the code and the most recent complete captures,
classified it, and gave it exactly one issue.

Method: code, logs and issues only. No labctl, no QEMU, no builds, no
`make test`, no code changes; another session held the boards and the host.
Every statement below that is not a direct quote from a capture or a source
line is marked as a reading or a hypothesis.

---

## 1. Sources

| Source | What it is |
|---|---|
| #1109 | Original list: Jetson kexec 14, Pi 5 17 plus one flake, unmodified main 60698898 |
| #1231 | Nine more that fail only after a UEFI-direct boot |
| `gauntlet-5-hw-jetson` 47993ea4, `docs/archive/switch-gauntlet/run5-hw/closeout/kexec-suite-6a4cfa93-boot01..05.txt` | Jetson kexec, 5 complete boots (Entry 39) |
| same branch, `closeout/uefi-suite-6a4cfa93-boot01..05.txt` | Jetson UEFI-direct, 5 complete boots (Entry 39) |
| same branch, `run5-hw/1262-1244/kexec-suite-661204e6-boot01..10.txt` | Jetson kexec, 10 boots, 3 complete (Entry 41) |
| `gauntlet-5-hw-pi5` 5cff7385, `closeout/pi5-suite-6a4cfa93-boot01..05.txt` | Pi 5, 5 complete boots (Entry 39) |
| `closeout/known-failures-*.txt`, `classify.sh.txt` on both branches | The accepted-failure lists the lab session compares against |

Test kernel in the close-out captures: 6a4cfa93 (`gauntlet/switch-5`). Code
references in this file are main dc51107c. The test files cited are identical
between the two commits except `kernel/tests/test_scheduler.c`, which is 528
lines longer at 6a4cfa93; for that file the capture's line number and main's
line number are both given.

Boards: jetson-nano-2 (6 CPUs), pi-5-1 (4 CPUs).

## 2. Counts per boot

| Path | Complete boots | Failures per boot | Distinct tests |
|---|---|---|---|
| Jetson kexec, close-out | 5 of 5 | 14 in all five | 14 |
| Jetson kexec, 1262-1244 | 3 of 10 | 14 in all three | the same 14 |
| Jetson UEFI-direct | 5 of 5 | 21 in all five | 21 |
| Pi 5 | 5 of 5 | 18 in four boots, 3 in boot 04 | 18 |

Three corrections to the numbers the lists carry:

1. **UEFI-direct is 12 + 9, not 13 + 9.** The two PMM coalescing tests fail on
   kexec only; both pass in all five UEFI-direct boots. 14 - 2 = 12 from #1109,
   plus the 9 of #1231, is the 21 the summary line prints.
2. **The accepted lists hold check lines as well as tests.** `no help entry for
   built-in 'nvcsi'` / `'emmc-bringup'` are prints inside
   `test_every_command_has_help_entry`, and `mlp_finish_nan_never_wins` is a
   sub-check inside `test_eviction_run_tests_passes`. None of the three is
   counted by the summary line.
3. **Pi 5 is not a constant 18.** Boot 04 had 3 failures. The other 15 follow
   whether the SD card probes (section 4, rows 24 to 38).

Across the three paths there are **38 distinct failing tests**.

The seven incomplete boots of the 1262-1244 set stopped in #1207 (boots 04,
07) and #1238 (five boots) before the summary. They are that session's
subject and are not classified here.

## 3. Classes

| Class | Meaning |
|---|---|
| kernel defect | the code is wrong |
| test defect | the test is wrong or depends on something it should not |
| platform limitation | the test legitimately cannot pass there; it should be an explicit skip with a reason |
| environment | depends on lab state, network or firmware |
| unknown | not determined |

## 4. The table

Paths: K = Jetson kexec, U = Jetson UEFI-direct, P = Pi 5. Rate is failing
boots over complete close-out boots.

| # | Test | Paths and rate | Class | Conf. | Home |
|---|---|---|---|---|---|
| 1 | `test_timer_irq_is_physical` | K 5/5, U 5/5, P 5/5 | test defect | high | #1267 |
| 2 | `test_every_command_has_help_entry` | K 5/5, U 5/5, P 5/5 | kernel defect | high | #1268 |
| 3 | `test_builtin_commands_grouped_and_sorted` | K 5/5, U 5/5, P 5/5 | kernel defect | high | #1269 |
| 4 | `test_gpu_jetson_uses_stub_driver` | K 5/5, U 5/5 | test defect | high | #1270 |
| 5 | `test_gpu_get_info_valid` | K 5/5, U 5/5 | test defect | high | #1270 |
| 6 | `gpu: capabilities detect valid` | K 5/5, U 5/5 | test defect | high | #1270 |
| 7 | `test_slm_gpu_run_mnist_returns_negative_off_jetson` | K 5/5, U 5/5 | test defect | high | #1271 |
| 8 | `test_slm_gpu_set_mnist_input_returns_negative_off_jetson` | K 5/5, U 5/5 | test defect | high | #1271 |
| 9 | `test_slm_gpu_set_mnist_input_fill_returns_negative_off_jetson` | K 5/5, U 5/5 | test defect | high | #1271 |
| 10 | `test_multiple_cores_isolated` | K 5/5, U 5/5 | test defect | high | #1272 |
| 11 | `test_round_robin_distributes_tasks` | K 5/5, U 5/5 | test defect | medium | #1272 |
| 12 | `test_net_driver_registered` | K 5/5, U 5/5 | environment | medium | #1273 |
| 13 | `test_coalesce_enables_larger_allocation` | K 5/5, U 0/5 | test defect | medium | #1207 (comment) |
| 14 | `test_recursive_coalescing` | K 5/5, U 0/5 | test defect | medium | #1207 (comment) |
| 15 | `test_boot_source_default_is_direct` | U 5/5 | platform limitation | high | #1231 |
| 16 | `test_oplib_pool_get_sass_gpu_va_unstaged_returns_null` | U 5/5 | platform limitation | high | #1231 |
| 17 | `test_oplib_pool_gpu_va_base_zero_when_unstaged` | U 5/5 | platform limitation | high | #1231 |
| 18 | `test_oplib_pool_scratch_base_unstaged` | U 5/5 | platform limitation | high | #1231 |
| 19 | `test_no_models_means_no_mount` | U 5/5 | test defect | high | #1231 |
| 20 | `test_slm_launch_defaults_to_gpu_auto` | U 5/5 | test defect | medium | #1231 |
| 21 | `test_slm_launch_gpu_on_requires_a_program` | U 5/5 | test defect | medium | #1231 |
| 22 | `test_session_open_failures` | U 5/5 | test defect | high | #1231 |
| 23 | `test_unavailable_reason_names_the_cause` | U 5/5 | test defect | high | #1231 |
| 24 | `test_blob_autoload_init_creates_conf` | P 4/5 | test defect | high | #1274 |
| 25 | `test_blob_autoload_init_marks_existing_fat_config_authoritative` | P 4/5 | test defect | high | #1274 |
| 26 | `test_blob_autoload_init_recovers_missing_fat_blob_from_lfs` | P 4/5 | test defect | high | #1274 |
| 27 | `test_blob_autoload_set_get_clear_round_trip` | P 4/5 | test defect | high | #1274 |
| 28 | `test_blob_autoload_fat_update_invalidates_lfs_fallback` | P 4/5 | test defect | high | #1274 |
| 29 | `test_blob_autoload_set_ignores_unrelated_stale_lfs_entry` | P 4/5 | test defect | high | #1274 |
| 30 | `test_blob_autoload_init_migrates_lfs_entries_to_fat` | P 4/5 | test defect | high | #1274 |
| 31 | `test_blob_autoload_set_migrates_all_lfs_entries_to_fat` | P 4/5 | test defect | high | #1274 |
| 32 | `test_blob_autoload_shell_commands` | P 4/5 | test defect | high | #1274 |
| 33 | `test_blob_autoload_overwrites_existing_conf` | P 4/5 | test defect | high | #1274 |
| 34 | `test_blob_autoload_recovers_from_backup_conf` | P 4/5 | test defect | high | #1274 |
| 35 | `test_blob_boot_autoload_skips_tampered_managed_blob` | P 4/5 | test defect | high | #1274 |
| 36 | `test_blob_autoload_rejects_malformed_metadata_suffix` | P 4/5 | test defect | high | #1274 |
| 37 | `test_blob_autoload_accepts_max_length_paths_across_all_slots` | P 4/5 | test defect | high | #1274 |
| 38 | `test_eviction_run_tests_passes` (sub-check `mlp_finish_nan_never_wins`) | P 4/5 | test defect | medium | #1275 |

For rows 24 to 37 "high" is the confidence that the suite depends on the real
SD card (it passes exactly when the card fails to probe); the per-test detail
of each of the 14 assertions was not traced and is medium.

### Counts per class

| Class | Tests |
|---|---|
| kernel defect | 2 |
| test defect | 31 |
| platform limitation | 4 |
| environment | 1 |
| unknown | 0 |
| total | 38 |

## 5. Failure messages and mechanisms

Line references are `file:line` on main dc51107c unless marked "capture".

### Rows 1 to 3: all three paths

**1. `test_timer_irq_is_physical`** — capture `test_scheduler.c:3376`,
expected 30, actual 26. The test asserts the literal 30 with the comment
"defined in platform.h as 30 for all platforms"
(`kernel/tests/test_scheduler.c:2851-2855`). Pi 5 and Jetson define 26, the Hyp
physical timer (`kernel/include/platform.h:837`, `:171`). The kernel is right.

**2. `test_every_command_has_help_entry`** — `test_shell.c:2770`, preceded by
`no help entry for built-in 'emmc-bringup'` (Pi 5) or `'nvcsi'` (Jetson). The
commands are registered at `kernel/src/shell.c:171` and `:195`;
`kernel/src/help.c` has no entry for either. The test is right. It returns at
the first gap, so later platform-only commands are unchecked.

**3. `test_builtin_commands_grouped_and_sorted`** — `test_shell.c:2674`,
"entries within a category must be alphabetized". Out-of-order platform-gated
rows: `irqtest` after `timdiag` on Pi 5 (`shell.c:219-222`, present because
`PI5_IRQ_DIAG` defaults ON, `CMakeLists.txt:286-288`); `csidiag` after `rcediag`
(`:205-206`), `clocks` and `thermal` after `rcediag`/`rtldiag` (`:211-212`),
`cdcdiag` after `xhcidiag` (`:225-226`) on Jetson. This settles #1109's "Pi 5:
not identified".

### Rows 4 to 12: Jetson, both paths

**4 to 6. Stub-driver expectations** — `test_gpu.c:679` strings not equal;
`test_gpu.c:58` expected 0, actual 11; `gpu: capabilities detect valid`
(`runtime/src/lib.rs:9930-9937`). Jetson registers the NVIDIA driver
(`kernel/src/main.c:737`); the tests still expect `"stub"`
(`kernel/tests/test_gpu.c:677-680`), `GPU_CAP_NONE` (`:57-58`) and a status of
`NotAvailable` or `DetectedNoCompute`. The capture prints "Compute ready".

**7 to 9. `slm_gpu_*_returns_negative_off_jetson`** — `test_gpu.c:858`, `:878`,
`:889`, all `rc < 0`. The Jetson branch assumes no GPU handoff
(`test_gpu.c:852-858`). Both lab boot paths have a channel; the capture shows
`[gpu-kind=0] channel bring-up: inherit 2072 us, channel 2 us, warmup 355 us`
directly before the first failure (kexec boot01:2781). The calls succeed. Side
effect: a real MNIST dispatch runs and the live input buffer is overwritten.

**10. `test_multiple_cores_isolated`** — capture `test_scheduler.c:1797`,
expected 0, actual 4. The test isolates CPUs 1 to 3 and expects CPU 0
(`test_scheduler.c:1779-1797`); a six-CPU board correctly offers CPU 4. A failed
assert is a `return` (`kernel/tests/unity.h:209-214`), so `irq_restore`,
`scheduler_remove_task`, the three `sched_unisolate_core` calls and
`task_destroy` (`:1798-1808`) are skipped. The capture confirms it: CPUs 1 to 3
are isolated at kexec boot01:8957-8959 and return only when later tests cycle
them (CPU 1 at 9038, CPU 2 at 9104, CPU 3 at 9132).

**11. `test_round_robin_distributes_tasks`** — capture `test_scheduler.c:4085`
(main `:3563`), "Round-robin should distribute across multiple CPUs". It runs
at capture line 9096 with CPUs 2 and 3 still isolated from row 10, and counts
only CPUs below 4 (`test_scheduler.c:3545-3552`). With CPU 0 holding the shell
task, CPU 1 is the only countable target. Reading, not run: it passes once
row 10 stops leaking.

**12. `test_net_driver_registered`** — `test_net.c:1039`, `drv expected not
NULL`. Jetson's only driver registers on USB probe
(`kernel/usb/class/cdc_ecm.c:958`); every capture, production included, prints
`cdc_ecm: no USB device — will retry on hot-plug` (`cdc_ecm.c:809`). Twenty-five
further network tests are ignored behind it (55 tests: 29 pass, 1 fail,
25 ignored). Whether an adapter is attached and failing to enumerate was not
checked.

### Rows 13 and 14: Jetson kexec only

`test_pmm.c:264` "Expected splits during allocation"; `test_pmm.c:318`
"Expected at least 4 merges for recursive coalescing". Both assume the single
pages they allocate come out of the block they just freed
(`kernel/tests/test_pmm.c:245-264`, `301-318`). `buddy_alloc` takes the smallest
order with a free block (`kernel/mm/pmm.c:313-321`), and the kexec heap starts
with 121 order-0 and 106 order-1 free blocks (kexec boot01:1717-1718; Pi 5 has
1 order-0 block). The sibling `test_split_creates_buddies` ignores itself under
exactly this condition (`test_pmm.c:410-412`) and is ignored in the same
captures. Deterministic on kexec (8 of 8 complete boots), passing on
UEFI-direct, counters and not faults: this reads as a test assumption, not as
#1207's free-list corruption. Not confirmed: `free_counts[0]` at test entry.
Per the session's instructions these two are not filed separately; they are
recorded in a comment on #1207 together with the observation that the
fragmented kexec heap is the layout #1207's current hypothesis needs.

### Rows 15 to 23: Jetson UEFI-direct only

Per-test table in the #1231 comment. Summary:

- **15** `test_efi_reloc.c:222`, expected 1, actual 2: the boot source is UEFI
  by design (`kernel/arch/arm64/efi_stub.c:726`).
- **16 to 18** `test_oplib_pool.c:76`, `:95`, `:107`: the tests need an
  unstaged operator library; the cold bring-up staged it
  (`kernel/gpu/oplib_cold.c:293`).
- **19** `test_boot_models.c:1174`: `/boot/models` is really mounted. The test
  also zeroes the real boot-model table and never restores it
  (`kernel/tests/test_boot_models.c:1171`).
- **20, 21** `test_slm_shell.c:342` (expected 6, actual 0) and `:366`: both
  expect "no GPU program on this boot".
- **22, 23** `test_slm_prog.c:1093` (expected -1, actual 0) and `:1174`
  (expected 0, actual 1): `slm_prog_backend_set(NULL)` clears only the test
  override and `backend()` falls back to the real cold channel
  (`kernel/gpu/slm_prog.c:501-503`). Row 22 leaves a session open on it.

### Rows 24 to 38: Pi 5

**24 to 37. `blob_autoload`** — first message `test_blob_autoload.c:538`,
`read_text_file("/mnt/files/blob_autoload.conf", ...) > 0`; then nine "strings
not equal" and four value mismatches; `Tests: 26 Passed: 12 Failed: 14`. In
boot 04 the SDHCI bring-up failed (`sdhci.c:1194: emmc clock enable failed`,
boot04:3691-3699) and all 26 passed. `setUp` clears the test device
(`kernel/tests/test_blob_autoload.c:527-532`); tests without a RAM-disk fixture
then acquire the real card through `boot_media_acquire()`
(`kernel/src/blob_autoload.c:106`, `kernel/src/boot_media.c:28`). The same path
lets `blob_autoload_set()` write `0:/slmstore/` on the card
(`blob_autoload.c:596-630`, `1130-1156`). Production kernels on that card
activate three eviction blobs from it at every boot
(`pi5-prod-661204e6-boot01.txt:204-206`). No write was observed directly.

**38. `test_eviction_run_tests_passes`** — `[FAIL] mlp_finish_nan_never_wins`,
then `test_eviction.c:506` expected 0, actual 1. Fails in the same four boots.
The check assumes GPU scores decide (`runtime/src/lib.rs:3231-3235`); with a
runtime MLP blob active they are ignored by design (`lib.rs:3246-3260`). The
card's MLP blob is activated during the blob_autoload suite (boot01:3726) and
that suite's reset clears only `sched` kinds
(`test_blob_autoload.c:1768-1783`). Hypothesis, not run.

## 6. What the list was hiding

The suspicion that started this (two PMM tests that look like #1207) did not
hold for those two, but the list did hide things that matter:

1. **A test that changes scheduler state for the rest of the Jetson suite**
   (row 10, #1272): cores left isolated, IRQs left masked, a task left queued.
   #1238's failing test is in the same file and runs after it. Relation
   untested.
2. **A suite that reaches the lab SD card** (rows 24 to 37, #1274, P1), with
   production boots loading eviction blobs from the same directory.
3. **Tests that alter real boot state when they fail** on UEFI-direct: the
   boot-model table (row 19) and a session on the real GPU channel (row 22).
4. **Two real kernel defects** nobody could see on QEMU (rows 2, 3).
5. **25 network tests silently ignored** on Jetson behind row 12.
6. **"Intermittent" Pi 5 failures that are deterministic** given whether the
   card probes. The 1-in-5 SDHCI bring-up failure itself (mailbox no response)
   is not triaged here and has no issue; it is noted in #1274.

Not found: any accepted failure that is a kernel defect corrupting state.

## 7. Issues

New (all `origin:review`, `kind:gap`, Backlog):

| Issue | Rows | Class | Priority |
|---|---|---|---|
| #1267 | 1 | test defect | P3 |
| #1268 | 2 | kernel defect | P2 |
| #1269 | 3 | kernel defect | P2 |
| #1270 | 4, 5, 6 | test defect | P3 |
| #1271 | 7, 8, 9 | test defect | P3 |
| #1272 | 10, 11 | test defect | P2 (leaks state into later tests) |
| #1273 | 12 | environment | P3 |
| #1274 | 24 to 37 | test defect | P1 (reaches persistent lab media) |
| #1275 | 38 | test defect | P3 |

Existing: #1231 (rows 15 to 23, per-test comment added), #1207 (rows 13, 14,
comment added). Related, not homes: #512 (cleanup skipped when an assert
fails; rows 10, 19, 22 are instances), #983 (toy blob on the Pi 5 card),
#533 (QEMU blob_autoload failures; the close-out QEMU run at 6a4cfa93 shows
none, so it may be stale; not examined).

Priority rule applied: kernel defects P2 unless they corrupt state. Neither
does. P1 and P2 on #1274 and #1272 are this session's judgement on test
defects with side effects and are open to change.

## 8. Proposed end state for #1109

**Close #1109 once the lab session confirms the table**, with a closing
comment that links each entry to its issue (posted there as a proposal). A
tracking list would keep a second copy of the same information, and a list
is what let these failures sit unexplained. In its place:

1. The accepted-failure files that `classify.sh` reads
   (`known-failures-jetson-1109.txt`, `known-failures-jetson-uefi-1109-1231.txt`,
   `known-failures-pi5-1109.txt`) gain the issue number on each line, so a
   failure can only be "known" by naming its issue. An entry whose issue closes
   is removed.
2. The check lines leave the lists (`no help entry ...`,
   `mlp_finish_nan_never_wins`); they are not tests.
3. The Pi 5 list is split by card state, or #1274 is done first; today a boot
   with 3 failures and a boot with 18 both count as clean.

**Explicit skips proposed (not made).** Four tests cannot pass after a
UEFI-direct boot and should ignore with a reason instead of failing:

| Test | Condition | Reason text |
|---|---|---|
| `test_boot_source_default_is_direct` | `boot_source_is_uefi()` | boot source is UEFI by design; the UEFI side is covered by `make test-qemu-uefi` |
| `test_oplib_pool_get_sass_gpu_va_unstaged_returns_null` | pool already staged | operator library staged by the cold bring-up |
| `test_oplib_pool_gpu_va_base_zero_when_unstaged` | pool already staged | same |
| `test_oplib_pool_scratch_base_unstaged` | pool already staged | same |

One conditional candidate: `test_net_driver_registered` on Jetson when no USB
device is present, if #1273 decides the lab board carries no adapter. The
other 33 are fixes, not skips: in each the property is testable on the
platform once the test (or, for rows 2 and 3, the kernel) is corrected.

## 9. Blind critic

A second Fable session classified 10 tests drawn at random (`shuf -n 10`) from
the 38. It was given the repository, the 15 close-out captures and the class
definitions, and was told not to read issues, `docs/archive` or the
known-failures files. It did not see this session's classifications.

Sample: rows 10, 30, 16, 28, 36, 22, 9, 17, 31, 7.

| Measure | Result |
|---|---|
| Paths and rates | 10 of 10 agree |
| Mechanism (cause, with file:line) | 10 of 10 agree |
| Class label | 8 of 10 agree |

The two label differences are rows 16 and 17 (oplib "unstaged" tests after a
UEFI-direct boot). The critic called them test defects "on the boundary with
platform limitation" and gave the same remedy this log proposes: skip when the
pool is already staged. Its reason for "test defect" is new information: on
kexec the two tests pass only because their suite runs (capture line 2181)
before `test_oplib_gpu_as` stages a pool (line 2725), so the tests are
order-dependent on kexec as well. The table keeps "platform limitation",
because the proposed change is a skip, and records the order dependence here.

The critic found no kernel defect in the sample, which matches (rows 2 and 3
were not drawn).

Corrections and additions taken from the critic, checked against the captures:

1. Row 10: the actual CPU is 4 in seven boots and 5 in three, never an
   isolated core. #1272's body says 4.
2. Rows 24 to 37: a write to the card **is** visible in the capture: `Set
   eviction autoload xgboost -> 0:/slmstore/autoload/eviction-xgboost.blob`
   (Pi 5 boot01:3754), printed by `test_blob_autoload_shell_commands` with no
   RAM disk installed. Section 5's "no write was observed directly" is
   therefore too weak; #1274 is corrected in a comment.
3. Row 22: after the leaked session, the next test's `slm_prog_backend_set`
   zeroes the session and mapping tables (`kernel/gpu/slm_prog.c:538-551`), so
   the real mappings are orphaned, not just left open.
4. Outside the sample: `test_pool_stage_with_as_scratch_base` calls
   `oplib_pool_unstage()` on the real staging after a UEFI-direct boot
   (`kernel/tests/test_oplib_gpu_as.c:244`, `270`). It passes, so it is not on
   any list. Not examined further.
5. Pi 5 boot 04: 149 mailbox errors after the first timeout. The critic also
   reads this as a real kernel or firmware-interaction problem. It has no
   issue yet.

---

## 10. Follow-up: end state approved (2026-11-15)

The end state proposed in section 8 was approved: #1109 is ended when the
known-failures PR merges, and from then on every known failure names its
issue. Same limits as before: a lab session holds the boards, so nothing was
built, booted or run under QEMU, and nothing under `kernel/` was changed.

Correction to section 5, rows 24 to 37: "No write was observed directly" is
wrong. Section 9 item 2 and the comment on #1274 give the observed write.

## 11. The two gaps, filed

| Issue | What | Class | Labels |
|---|---|---|---|
| #1280 | Pi 5: one mailbox property-call timeout makes every later call in that boot fail; when it lands on the SD card bring-up the card never probes | kernel defect, confidence medium on the mechanism | `kind:gap`, P2, `platform:pi5`, `sub:drivers` |
| #1281 | `test_pool_stage_with_as_scratch_base` unstages the real operator-library pool after a UEFI-direct boot and leaves it unstaged | test defect with a side effect on live state | `kind:gap`, P2, `platform:jetson`, `sub:gpu`, `sub:ci` |

### #1280: what the history shows

`kind:gap`, not `kind:regression`. The capture history has no point at which
the card probed reliably and then stopped:

- `mailbox: no response` appears in 3 capture files out of the 845 on the
  five Pi 5 evidence branches (`gauntlet-1193-hw-pi5`, `gauntlet-2..5-hw-pi5`)
  that contain the suite's mailbox test banner. The 845 were not each checked
  for completeness.
- Close-out boot 04 (`pi5-suite-6a4cfa93-boot04.txt:3691`) is the only one
  where the timeout lands on the first SD card bring-up. Counts there: 1
  `no response`, 148 `unexpected response code 0x00000000` (149 mailbox
  errors, last at line 9219), 146 `emmc clock enable failed`. 3 of the 148
  are the MAC address tag `0x00010003`, so the fault is in the mailbox
  driver's state, not in the SD card path.
- The two older ones (`pi5-rebased-RT-test-B07.txt:8815`,
  `pi5-removal-test-W11.txt:8713`, coherency run c3) time out late in the
  suite, after the card had probed, and the two mailbox calls after each also
  return code 0.

So "about 1 in 5" is the close-out sample (1 of 5). Over the saved history
the timeout is rare, and in 3 of 3 cases every later call in the boot failed.

Suspected mechanism, from code only: `mbox_property_call` returns on timeout
(`kernel/drivers/bcm_mailbox.c:218-221`) without draining `MBOX0`. A late
response then stays in the FIFO; the next call reads it, passes the channel
and bus-address checks (`:223`, `:236`; every call uses the same static
buffer) and inspects the buffer before the firmware has processed the new
request, which is why `buf=` prints the unmodified request (`:255-264`). The
driver stays one response behind. Not explained: why the first response is
missing within the 100 ms budget (`:144`).

### #1281: what the capture shows

`uefi-suite-6a4cfa93-boot01.txt`: the cold bring-up stages the library at
`gpu_va=0x4266000`, `phys=0xbdc80000` (line 2904). The test then stages it at
`gpu_va=0x4000000`, `phys=0x234501000` through its mock address space (line
3804) and unstages it (`kernel/tests/test_oplib_gpu_as.c:270`). No later
`stage_to_gpu` line follows. `oplib_pool_unstage()` only zeroes bookkeeping
(`kernel/gpu/oplib_pool.c:325-335`), so the first pool stays mapped in the
real VM and unreferenced. Whether a later test or shell path gets a different
result because of it is not confirmed.

## 12. Known-failures lists and classify.sh

Branch `tests/known-failures-issues` from main dc51107c, commit d41b1e1b,
draft PR #1282 (not merged). The lists and the script existed only as
evidence files on the `gauntlet-5-hw-*` branches; they now live in
`scripts/hw-suite/`.

- Entry format `<test name><TAB>#<issue>`. `classify.sh` rejects an entry
  without an issue number (exit 2).
- The three check lines are no longer entries. They are `check:` annotations
  (`no help entry for built-in 'nvcsi'` and `'emmc-bringup'` -> #1268,
  `mlp_finish_nan_never_wins` -> #1275), which the classifier skips. Removing
  them outright would have made every boot report them as new failures.
- UEFI-direct list: 21 = 12 + 9, stated in its header. The file inherited
  from the close-out already lacked the two PMM tests; the wrong "13 + 9" was
  in the issue text, not in the file.
- `classify.sh` now also prints failing tests per issue and listed tests that
  did not fail, and exits 1 on a failure that is not in the list.

Offline run on the 15 saved close-out suite captures (the script reads saved
files; nothing was built):

| Path | Boots | Suite total | Failing tests found | Not in list | Listed, not failing |
|---|---|---|---|---|---|
| Jetson kexec | 5 | 14 | 14 | none | none |
| Jetson UEFI-direct | 5 | 21 | 21 | none | none |
| Pi 5, boots 01, 02, 03, 05 | 4 | 18 | 18 | none | none |
| Pi 5, boot 04 | 1 | 3 | 3 | none | #1274 x14, #1275 x1 |

The old script printed `distinct FAIL names 15` against `14 total failure(s)`
on kexec and 4 against 3 on Pi 5 boot 04, because it counted the check lines.

Negative checks: the UEFI-direct list against a kexec capture reports the two
PMM tests as not in the list and `#1231 x9` as listed, not failing (exit 1);
the old list without issue numbers is rejected with its 15 lines (exit 2).

Not checked: a capture from a current boot, and awk implementations other
than GNU awk 5.2.1.

## 13. Hand-off list for phase 2

Each item gives the place, the change, and how to show the failure before and
the pass after. Line numbers are main dc51107c. None of these changes was
made. "Suite" means the test kernel's full run on that path, classified with
`scripts/hw-suite/classify.sh`.

### 13.1 #1274: blob_autoload tests use the real SD card (P1)

**Where.** `kernel/tests/test_blob_autoload.c`: `setUp()` at `:527-532`
clears the boot-media test device before every test, and the 14 failing tests
never install one, so `boot_media_acquire()` (`kernel/src/boot_media.c:45-99`)
falls through to `boot_media_create()` (`:22-34`), which on Pi 5 is the real
card (`sdhci_create_bcm2712()`), cached for the kernel's lifetime (`:95-96`).

**Change.**

1. `kernel/src/boot_media.c` and `kernel/include/boot_media.h`: add a test
   switch, for example `boot_media_test_set_no_device(bool)`, checked in
   `boot_media_acquire()` after the test-device branch (`:49-54`) and before
   the production-cache branch (`:56-61`); when set, return NULL. The existing
   create hook (`boot_media_test_set_create_hook`, `:155`) is not enough alone:
   once the production device is cached, `:56-61` returns it without calling
   the hook.
2. `test_suite_blob_autoload()` (`test_blob_autoload.c:1785`): set the switch
   before the first `RUN_TEST` (`:1788`) and clear it after the last
   (`:1814`), next to `blob_autoload_suite_reset_runtime_state()`. Tests that
   need a volume keep installing their RAM disk
   (`boot_media_test_set_device`, for example `:573`), which takes precedence.
3. New test in the same suite: with a counting create hook installed, run
   `blob_autoload_init()` and one `blob_autoload_set()` and assert the hook
   was called 0 times and `boot_media_acquire()` returns NULL.
4. Remove the 14 `#1274` lines from `scripts/hw-suite/known-failures-pi5-1109.txt`.

**Before.** The new test, written first and run without step 2, fails on QEMU
(hook count above 0). On Pi 5 with a card that probes: the 14 tests fail, and
between `[TEST] ...test_blob_autoload.c` and its summary the capture has
`sdhci: probing emmc2` and
`Set eviction autoload xgboost -> 0:/slmstore/autoload/eviction-xgboost.blob`
(close-out boot01:3706, 3754).

**After.** New test passes in `make test`. Pi 5 suite, 10 boots: 0 of the 14
fail whether or not the card probes; no `sdhci: probing emmc2` line inside
the blob_autoload suite; `classify.sh` shows no `#1274` under "by issue". The
listing and checksums of `0:/slmstore/` taken through labctl before and after
a suite boot are identical. Expect #1275's result to change as well (no
eviction blob is activated from the card during the suite); if it then
passes, its line goes too.

### 13.2 #1272: test_multiple_cores_isolated leaks isolation, masked IRQs and a queued task (P2)

**Where.** `kernel/tests/test_scheduler.c:1776-1807`. The test isolates CPUs
1 to 3 only (`:1779-1781`), so on Jetson's 6 CPUs the task lands on CPU 4 or
5 and the assert at `:1796` fails inside `irq_save()` (`:1794`). The return
skips `scheduler_remove_task` (`:1797`), `irq_restore` (`:1798`), the three
`sched_unisolate_core` calls (`:1801-1803`) and `task_destroy` (`:1806`).
`test_round_robin_distributes_tasks` (`:3535-3565`) then sees only CPUs 0, 4
and 5 available and counts only CPUs below 4 (`:3545`, `:3552`).

**Change.**

1. `:1779-1786`: isolate every CPU from 1 to `cpu_count - 1` in a loop and
   build `expected` the same way.
2. `:1788` and `:1796`: read `sched_get_isolated_cores()` and
   `t->assigned_cpu` into locals, run the whole cleanup (`:1797-1806`), then
   assert on the locals. Same for `TEST_ASSERT_NOT_NULL(t)` at `:1792`:
   unisolate before asserting.
3. `:3545`, `:3552`, `:3560`: size `cpu_hits` by `MAX_CPUS` (8,
   `kernel/include/config.h:15`) so CPUs 4 and 5 count.
4. Remove the two `#1272` lines from both Jetson lists.

**Before.** Jetson suite, either path: `test_multiple_cores_isolated` fails
at `test_scheduler.c:1796` with actual 4 or 5, and
`test_round_robin_distributes_tasks` fails. To show the leak itself without
the fix: a temporary check after the test that `sched_get_isolated_cores()`
is 0 reports `0xe`.

**After.** Both pass on kexec and UEFI-direct, 5 boots each;
`sched_get_isolated_cores()` is 0 after the test; `make test` (4 CPUs)
unchanged. Because the scheduler suite no longer runs its remaining tests
with CPUs 1 to 3 isolated and IRQs masked, compare the rest of the scheduler
suite's results and the #1238 rate before and after, and note any change on
#1238.

### 13.3 Skip: test_boot_source_default_is_direct

**Where.** `kernel/tests/test_efi_reloc.c:220-224`. The EFI stub sets
`BOOT_SOURCE_UEFI` by design (`kernel/arch/arm64/efi_stub.c:726`).

**Change.** At the top of the test, `TEST_IGNORE_MESSAGE("booted through the
EFI stub; boot source is UEFI by design (checked by make test-qemu-uefi)")`
when the kernel was booted through the stub.

**Caution on the condition.** `boot_source_is_uefi()` alone is the value under
test. The test exists to catch a stray store that makes a kexec or QEMU boot
read UEFI; guarding on the same variable turns that case into a skip. The
condition needs a second witness of a UEFI boot that does not come from
`slmos_boot_source`. No such symbol exists today (the boot-model record is
zeroed by `test_no_models_means_no_mount`, and may be empty on a UEFI boot
without models).

**Decision (2026-11-15): a second marker written by the stub.** Chosen over
the firmware DTB address and over accepting the lost coverage.

1. `kernel/include/boot_source.h`: declare
   `extern volatile uint64_t slmos_efi_stub_marker;` with two non-zero
   constants, `EFI_STUB_MARKER_UNSET` (the initialiser) and
   `EFI_STUB_MARKER_RAN`, and a comment that it is a test witness: drivers
   keep using `boot_source_is_uefi()`.
2. `kernel/arch/arm64/efi_stub.c`: define the marker there, initialised to
   `EFI_STUB_MARKER_UNSET`. It must be in `.data` for the same reason as
   `slmos_boot_source` (`kernel/src/main.c:116-119`): written before the
   image is cleaned and copied, and before `.bss` is cleared. Defining it in
   `efi_stub.c` and not next to `slmos_boot_source` in `main.c` keeps the two
   in different objects, so one stray store or one misplaced section does not
   move both. `test_efi_reloc.c` is built only where `efi_stub.c` is
   (`kernel/tests/test_harness.c:235`), so no other platform needs the symbol.
3. `kernel/arch/arm64/efi_stub.c:726`: store `EFI_STUB_MARKER_RAN` next to the
   `slmos_boot_source` store, before the first `efi_dcache_clean_inval` at
   `:728`.
4. `kernel/tests/test_efi_reloc.c:220-224`: three outcomes.
   - Marker is `EFI_STUB_MARKER_RAN`: assert `slmos_boot_source ==
     BOOT_SOURCE_UEFI` (so a UEFI boot that lost its boot source fails), then
     `TEST_IGNORE_MESSAGE`.
   - Marker is `EFI_STUB_MARKER_UNSET`: the two asserts as today.
   - Any other value: fail.
   Update the comment at `:214-219`, which says the test kernel is only ever
   loaded with QEMU `-kernel`.
5. Remove the test's line from
   `scripts/hw-suite/known-failures-jetson-uefi-1109-1231.txt`.

**Before / after.** UEFI-direct suite: `[FAIL]` at `test_efi_reloc.c:222`
(expected 1, actual 2) becomes `[IGNORE]` with the reason; `classify.sh`
shows `#1231 x8`. Kexec suite and `make test`: still `[PASS]`, not
`[IGNORE]`. `make test-qemu-uefi` still passes (it checks the relocated boot
prints "Boot source: UEFI-direct", `Makefile:1003`). To show the guard is not
vacuous, two temporary edits on QEMU, each of which must give `[FAIL]` and
not `[IGNORE]`: `slmos_boot_source = BOOT_SOURCE_UEFI` before the test
(marker unset), and `slmos_efi_stub_marker = EFI_STUB_MARKER_RAN` before the
test (boot source still direct).

### 13.4 Skips: the three oplib "unstaged" tests

**Where.** `kernel/tests/test_oplib_pool.c`:
`test_oplib_pool_get_sass_gpu_va_unstaged_returns_null` (`:70-77`, assert
`:76`), `test_oplib_pool_gpu_va_base_zero_when_unstaged` (`:92-96`, assert
`:95`), `test_oplib_pool_scratch_base_unstaged` (`:102-110`, failing assert
`:107`). After a UEFI-direct boot the cold bring-up has staged the pool
(`kernel/gpu/oplib_cold.c:293`).

**Change.** In each, before the unstaged asserts:

```c
if (boot_source_is_uefi() && oplib_pool_gpu_va_base() != 0) {
    TEST_IGNORE_MESSAGE("operator library already staged by the cold bring-up");
}
```

Both halves are needed: a kexec or QEMU boot with a nonzero base is a real
failure and must stay one, and a UEFI-direct boot whose staging failed still
runs the asserts. In `test_oplib_pool_scratch_base_unstaged`, place the guard
after the two NULL-argument asserts (`:105-106`), which hold in either state,
so they keep running. Remove the three lines from
`scripts/hw-suite/known-failures-jetson-uefi-1109-1231.txt`.

**Before / after.** UEFI-direct suite: three `[FAIL]` (`:76` expected -1
actual -8; `:95` expected 0 actual 69623808; `:107` expected -1 actual 0)
become three `[IGNORE]` with the reason; `classify.sh` shows `#1231 x5` (x6
if 13.3 is not done). Kexec suite and `make test`: the three still `[PASS]`,
not `[IGNORE]`.

**Order dependence.** These tests pass on kexec only because nothing has
staged the pool when they run (section 9), and #1281's test unstages the pool
later in the suite. Fix #1281 in the same change or before it, so the skip
condition is not satisfied or defeated by another test's side effect.

### 13.5 Not on the hand-off list

`test_net_driver_registered` (#1273) becomes a skip only if #1273 decides the
board carries no adapter. The remaining entries are fixes in their own issues
(#1267 to #1271, #1275, the five non-skip tests in #1231, and the two PMM
tests in #1207).

## 14. Model change (2026-11-15)

Sections 1 to 13 were written on Fable. From this point the session runs on
Opus, to conserve Fable usage, and builders and other helper subagents inherit
Opus. Critics stay on Fable: each critic is spawned with the model set to
fable, as its brief says. If a critic hits a Fable usage limit, it is paused
and the user is told; it is not switched to Opus. The blind critic in section 9
ran on Fable, before this change.
