# Structural review chain, re-measure 1 (2026-11-17)

Wrap-up of the structural-review chain, 2026-11-08 to 2026-11-17. GitHub data
and the repository only: no lab, no QEMU, no builds, no code changes.

Source review: "SLM-OS Structural Review: Why the Ticket Count Grows"
(2026-11-08), read from the Claude Docs copy. Method: Track A's, from
`docs/archive/structural/parallel-1.md` Entry 5 on branch
`structural/parallel-1` (d9df7a9c). Close-outs read: `coherency.md`
(main 98e17440), `coherency-2.md` (b6f0beed), `coherency-3.md` (eb7050f6),
`lifecycle-4.md` (aa3cd858), `switch-5.md` (d46daf8b), `known-failures-1.md`
(694e7a47).

## Cut times

| Column | Cut (UTC) | Note |
|---|---|---|
| Review | 2026-11-08 21:15Z | the review's tracker snapshot (13:15 PT) |
| Track A | 2026-11-14 07:25Z | Track A's snapshot (2026-11-13 about 23:25 PT) |
| Now | 2026-11-17 22:06:34Z | this pass |

Data: `gh issue list --state all --limit 2000 --json
number,title,body,labels,milestone,createdAt,closedAt,state,stateReason`
(617 issues) and `gh pr list --state merged --limit 2000 --json
number,title,mergedAt,createdAt` (632 PRs), both taken at the "now" cut. Open
at time t means created at or before t and not closed by t. Windows are
half-open, in UTC.

**Reproduction check.** With these cuts the method gives exactly Track A's
figures: 91 opened and 45 merged from Nov 1 to the review, 40 and 19 from
the review to Track A, 214 open at the review, 236 at Track A, and every
cell of Track A's hub table (24/21/23/12/6 at the review, 34/31/23/13/8 at
Track A). The hub regexes ran against today's issue text, so edits made
since then would show up here; none changed a count.

## 1. Re-measure

### Issues opened per PR merged (UTC months)

| Period | Review | Track A | Now |
|---|---|---|---|
| 2026-04 | 288 / 284 = 1.01 | 1.01 | 1.01 |
| 2026-05 | 145 / 260 = 0.56 | 0.56 | 0.56 |
| 2026-06 to 09 | 0 / 0 | 0 / 0 | 0 / 0 |
| 2026-10 | 13 / 14 = 0.93 | 0.93 | 0.93 |
| 2026-11 to the cut | 91 / 45 = 2.02 | 131 / 64 = 2.05 | 171 / 74 = 2.31 |

| Window | Opened | Merged | Ratio |
|---|---|---|---|
| Nov 1 to the review cut | 91 | 45 | 2.02 |
| Review cut to Track A | 40 | 19 | 2.11 |
| Track A to now | 40 | 10 | 4.00 |

Of the 29 PRs merged since the review cut, 10 are #475 follow-ups merged in
one batch on Nov 9 (06:55 to 07:00Z) that the chain did not produce.

### Open issues

| | Review | Track A | Now |
|---|---|---|---|
| Open | 214 | 236 | 213 |

| Window | Opened | Closed | Net |
|---|---|---|---|
| Nov 1 to the review cut | 91 | 54 | +37 |
| Review cut to Track A | 40 | 18 | +22 |
| Track A to now | 40 | 63 | −23 |

50 of the 63 closes since Track A are the approved triage batch of
2026-11-14 08:07 to 08:09Z (parallel-1 Entry 9). Without it the open count
would be 263.

### Hub share of open issues

Track A's patterns, title plus body:

| Hub | Review (of 214) | Track A (of 236) | Now (of 213) |
|---|---|---|---|
| NC, coherency, cache maintenance | 24 (11.2%) | 34 (14.4%) | 26 (12.2%) |
| Build flags and configuration | 21 (9.8%) | 31 (13.1%) | 35 (16.4%) |
| FFI, `rust_*`, `extern "C"` | 23 (10.7%) | 23 (9.7%) | 18 (8.5%) |
| Shell (`sub:shell` label) | 12 (5.6%) | 13 (5.5%) | 12 (5.6%) |
| Names `shell_sys.c` | 6 (2.8%) | 8 (3.4%) | 7 (3.3%) |

What moved them between Track A and now:

- **NC.** 11 closed, all 11 in the triage batch (#27, #202, #475, #539, #655,
  #665, #666, #678, #1131, #1211, #1223); 3 new (#1256, #1261, #1280). No NC
  issue was closed by NC work after Track A: the removal (#1225) had merged
  on Nov 11, before Track A's cut.
- **Configuration.** 9 closed (7 in the triage batch, plus #1234 and #1244);
  13 new, 6 of them from the CI build matrix (#1246, #1247, #1249, #1250 and
  #1256 from its first run, #1257 about the matrix itself).
- **FFI**, **shell** and **`shell_sys.c`**: every close in the triage batch.
  New: none for FFI; #1268, #1269, #1278 for shell; #1246, #1279 for
  `shell_sys.c`.

The NC pattern counts mentions, not causes. Of the 26 open NC matches now,
10 name the NC axis itself (`PLATFORM_HAS_NC_MEMORY`, `NC_AXIS`, NC memory or
#1193). Most cite #1193 only as context (for example #1200, #1201, #1214,
#1256, #1261, #1280). The code shows the removal directly:
`PLATFORM_HAS_NC_MEMORY` appears in 23 files under `kernel/` and `runtime/`
at 85f51ffd (main before the removal) and in 1 at 2a77f395 (current main;
`kernel/tests/test_nc_axis.c`). Over the same span, `kernel/sched/sched.c`
went from 84 preprocessor conditionals to 57.

### Origin mix of issues opened in each window

Labels as they stand, plus a classification from the text, made in this
pass, for each issue with no `origin:*` label. Track A's rules: origin is
judged from the text; `review` means found by code review, a critic or an
audit; `hardware` means found by running the kernel in a lab or gauntlet run;
`agent-followup` means filed by an agent from its own build or fix work;
`user` means a user request or decision; `golden` means found by a
golden-reference comparison.

| Origin | Nov 1 to review (91) | Review to Track A (40) | Track A to now (40) |
|---|---|---|---|
| review | 34 (37.4%) | 16 (40.0%) | 25 (62.5%) |
| hardware | 25 (27.5%) | 23 (57.5%) | 8 (20.0%) |
| agent-followup | 25 (27.5%) | 1 (2.5%) | 3 (7.5%) |
| user | 4 (4.4%) | 0 | 4 (10.0%) |
| golden | 3 (3.3%) | 0 | 0 |
| Classified here (no label) | 39 | 15 | 6 |

60 issues were classified in this pass (listed in the appendix). Track A labelled only the 236 issues open on
Nov 13, so issues closed before that pass have no origin label.

**One rule needed a choice.** Track A labelled QEMU findings made during
gauntlet runs `hardware` (#1230, #1232, #1233). Its critic moved #1240 (a
QEMU diagnostic run) to `agent-followup`, and QEMU flakes from feature work
carry `agent-followup` (#1137, #1163). This pass follows the gauntlet rule:
a QEMU failure in a gauntlet's test runs is `hardware`. Eight issues depend
on that choice (#1162, #1213, #1219, #1229, #1230, #1232, #1233, #1260).
Counted as `agent-followup` instead, hardware becomes 24 / 17 / 7 and
agent-followup 26 / 7 / 4 across the three windows.

## 2. What the chain found

All 80 issues opened from the review cut to now came out of a chain session.
Run keys:

| Key | Session | Dates | Issues |
|---|---|---|---|
| coh-1 | coherency gauntlet run 1 (#1193) | Nov 8 to 9 | 13 |
| coh-2 | coherency run 2 | Nov 9 to 10 | 8 |
| coh-3 | coherency run 3 and close-out | Nov 11 | 5 |
| lc-4 | lifecycle gauntlet run 4 | Nov 12 | 10 |
| sw-5 | switch gauntlet run 5, its #1263/#1207 and #1284 lab sessions, the #1207 lab session and the reviews of PRs #1265 and #1293 | Nov 13 to 17 | 18 |
| par-1 | structural parallel session 1 (triage, CI matrix, architecture record; no lab) | Nov 13 to 14 | 13 |
| kf-1 | known-failures triage 1 (#1109; lab rows) | Nov 15 to 16 | 13 |

Verdicts:

- `kernel`: a real defect in kernel, runtime or vendored code, or in a
  configuration the kernel ships.
- `test`: a defect in a test or the test harness.
- `tooling`: a gap in the build system, CI, lab, docs or process.
- `dup`: the same defect as another issue.
- `undetermined`: open, with too little evidence to decide (mostly single
  occurrences).
- `not-defect`: a decision, an enhancement or a tracking issue.

"Seen" says how the defect is known: `seen` means it failed in a run,
`probe` means an instrumented run showed it, and `code` means code reading
only.

Classes:

- `lock/ordering`: races, IRQ state, memory ordering, lock placement.
- `memory/allocator`.
- `inherited`: inherited boot or device state (caches, the firmware or
  Linux hand-off, the boot path, the DTB).
- `test-isolation`: a test that depends on environment, timing, shared
  state or the boot path.
- `other`.

States are as of the "now" cut.

| Issue | Run | Verdict | Seen | Subsystem | Class | State | What |
|---|---|---|---|---|---|---|---|
| #1193 | coh-1 | not-defect | - | sched/smp | - | closed (PR #1225) | the gauntlet tracking issue (NC premise) |
| #1194 | coh-1 | kernel | code | smp | other | closed (PR #1208) | tests the A72 SMPEN bit on A76/A78AE |
| #1195 | coh-1 | tooling | seen | lab (SDWire) | other | open | pi-5-1 boot needs SDWire round trip |
| #1196 | coh-1 | kernel | code | boot/mm | inherited | closed (PR #1209) | stale WB lines over the NC block after hand-off |
| #1197 | coh-1 | kernel | code | boot | inherited | closed (PR #1208) | caches-off writes never invalidated by VA |
| #1198 | coh-1 | kernel | code | mm (boot) | lock/ordering | closed (PR #1208) | mmu_enable unmasks all of DAIF |
| #1199 | coh-1 | kernel | seen | libc/ai-runtime | other | closed (PR #1239) | implicit-int memchr: sign-extended pointer |
| #1200 | coh-1 | undetermined | seen | sched (bench) | other | open | bench smp reports the wrong CPU; not investigated |
| #1201 | coh-1 | kernel | code | drivers (UART) | lock/ordering | open | unbounded spin under UART lock, IRQs masked |
| #1202 | coh-1 | test | seen | tests (shell) | test-isolation | closed (PR #1215) | yield-count wait |
| #1203 | coh-1 | dup | seen | sched | lock/ordering | closed | symptom; re-scoped to #1216 |
| #1204 | coh-1 | undetermined | seen | smp (perf) | other | open | per-boot UEFI litmus slowness; Track A proposed not-worth-doing |
| #1205 | coh-1 | test | seen | tests (components) | test-isolation | closed (PR #1215) | yield-count wait |
| #1206 | coh-2 | kernel | code | boot (linker) | other | closed (PR #1239) | .got outside the relocated range |
| #1207 | coh-2 | kernel | seen | mm/gpu | inherited | closed (PR #1293) | Linux GPU L2 dirty lines over PMM pages |
| #1210 | coh-2 | kernel | code | sched | other | open | steal deque capacity below MAX_TASKS; never reached |
| #1211 | coh-2 | test | seen | tests (sched) | test-isolation | closed (PR #1215) | leak-check window |
| #1212 | coh-2 | kernel | code | mm | inherited | open | redundant clean of caches-off page tables, wrong comment |
| #1213 | coh-2 | kernel | seen | sched | lock/ordering | closed (PR #1237) | task slot reclaimed while its CPU still uses it |
| #1214 | coh-2 | test | code | tests/ci | other | open | test never built under QEMU |
| #1216 | coh-2 | kernel | probe | sched | lock/ordering | closed (PR #1225) | steal early-out with tick-only wakes |
| #1219 | coh-3 | dup | seen | sched | lock/ordering | closed | symptom; root cause #1229 |
| #1220 | coh-3 | undetermined | seen | tests (shell) | other | open | one Pi 5 failure |
| #1221 | coh-3 | undetermined | seen | tests (sched) | other | open | one Pi 5 failure |
| #1223 | coh-3 | dup | seen | sched | lock/ordering | closed duplicate | duplicate of #1213 |
| #1224 | coh-3 | dup | seen | boot (linker) | other | closed (PR #1239) | same .got defect as #1206 |
| #1228 | lc-4 | undetermined | seen | mm | memory/allocator | open | signature matches #1207; not confirmed |
| #1229 | lc-4 | kernel | probe | sched | lock/ordering | closed (PR #1237) | preempt_disabled cleared for the wrong CPU |
| #1230 | lc-4 | test | seen | tests (smp) | test-isolation | open | litmus rows assume vCPU timing |
| #1231 | lc-4 | test | seen | tests | test-isolation | open | 9 tests assume a kexec boot; PR #1282 skips 4, 5 remain |
| #1232 | lc-4 | undetermined | seen | tests (shell) | other | open | one QEMU failure |
| #1233 | lc-4 | test | seen | tests (sched) | test-isolation | open | tests assume a queued task is not stolen |
| #1234 | lc-4 | kernel | code | sched | lock/ordering | closed (PR #1266) | switch-in before the old CPU saved context |
| #1235 | lc-4 | kernel | seen | drivers (camera I2C) | inherited | open | UEFI-direct: unpowered I2C reachable |
| #1236 | lc-4 | kernel | code | sched (x86) | lock/ordering | closed (PR #1237) | resched IPI re-enters schedule() |
| #1238 | lc-4 | kernel | seen | sched | lock/ordering | closed (PR #1266) | idle-task fault; fixed by the #1234 switch window |
| #1240 | sw-5 | kernel | code | gpu/mm | memory/allocator | open | 2 MB-aligned GPU buffers leak half a block |
| #1241 | sw-5 | kernel | code | sched | lock/ordering | open | migrate rewrites assigned_cpu of a task queued elsewhere |
| #1242 | sw-5 | kernel | probe | sched | lock/ordering | closed (PR #1266) | schedule() keyed on a stale CPU id |
| #1244 | sw-5 | kernel | code | boot/mm | inherited | closed (PR #1265) | kexec DTB inside the PMM heap |
| #1246 | par-1 | kernel | seen | build (config) | other | open | ENABLE_NETWORKING=OFF does not link |
| #1247 | par-1 | tooling | seen | build (config) | other | open | GSP firmware object always compiled |
| #1248 | par-1 | tooling | seen | build (config) | other | open | Debug + LTO Rust flags |
| #1249 | par-1 | test | seen | build (config) | other | open | EL1 smoke test does not link |
| #1250 | par-1 | kernel | seen | net (vendored) | other | open | implicit declarations in wolfSSH/wolfCrypt (warnings only; latent) |
| #1252 | par-1 | not-defect | - | boot | - | open | user decision: retire kexec (epic) |
| #1253 | par-1 | not-defect | - | ai-runtime | - | open | user decision: runtime weights |
| #1254 | par-1 | not-defect | - | ai-runtime | - | closed | user decision: freeze Hailo |
| #1255 | par-1 | dup | - | net | - | closed duplicate | duplicate of #1258 |
| #1256 | par-1 | tooling | seen | build (config) | other | open | COOP_PREEMPT=ON silently dropped |
| #1257 | par-1 | tooling | - | ci | other | open | hand-copied CI defaults |
| #1258 | par-1 | tooling | - | licensing | other | open | GPL components in an MIT repo |
| #1259 | sw-5 | tooling | seen | sched (trace) | other | open | SLOT_TRACE dumps 4 of 6 CPUs |
| #1260 | sw-5 | undetermined | seen | tests (smp) | other | open | one QEMU timeout |
| #1261 | par-1 | tooling | - | docs | other | open | stale documentation |
| #1262 | sw-5 | undetermined | seen | sched | lock/ordering | open | one stall in 600; cause unknown |
| #1263 | sw-5 | kernel | seen | sched/smp | lock/ordering | closed (PR #1285) | rq_lock words share a cache line |
| #1264 | sw-5 | tooling | seen | build | other | open | test image depends on build history |
| #1267 | kf-1 | test | seen | tests (sched) | other | open | hardcoded TIMER_IRQ 30 |
| #1268 | kf-1 | kernel | seen | shell | other | open | missing help entries |
| #1269 | kf-1 | kernel | seen | shell | other | open | command table out of order (no runtime effect) |
| #1270 | kf-1 | test | seen | tests (gpu) | other | open | tests expect the stub GPU driver |
| #1271 | kf-1 | test | seen | tests (gpu) | test-isolation | open | tests assume no GPU handoff |
| #1272 | kf-1 | test | seen | tests (sched) | test-isolation | closed (PR #1282) | test leaks isolated CPUs |
| #1273 | kf-1 | tooling | seen | lab (net) | other | open | no USB network device on the board |
| #1274 | kf-1 | test | seen | tests (vfs) | test-isolation | closed (PR #1282) | tests use (and write) the real SD card |
| #1275 | kf-1 | test | seen | tests (eviction) | test-isolation | open | selftest sees an active runtime blob |
| #1276 | sw-5 | kernel | code | mm | memory/allocator | open | mid-page reserve loses its partial page |
| #1277 | sw-5 | kernel | code | boot (DTB) | inherited | open | scanned DTB never parsed |
| #1278 | sw-5 | undetermined | seen | lua | memory/allocator | open | one Lua GC abort in 20 QEMU runs |
| #1279 | sw-5 | not-defect | - | sched | - | open | enhancement: true-shared counters |
| #1280 | kf-1 | kernel | seen | drivers (mailbox) | other | open | one timeout poisons every later call |
| #1281 | kf-1 | test | seen | tests (gpu) | test-isolation | closed (PR #1282) | test unstages the real oplib pool |
| #1284 | sw-5 | test | probe | tests (sched) | lock/ordering | open | decided as the test's own race |
| #1287 | kf-1 | tooling | - | sched (diagnostic) | other | open | land the PD_WITNESS check |
| #1288 | kf-1 | undetermined | seen | sched | other | open | wake placement after padding; cost unmeasured |
| #1290 | sw-5 | test | seen | tests (components) | test-isolation | open | assumes ctx pages never reused |
| #1291 | sw-5 | kernel | seen | gpu/mm | inherited | open | 64-byte device write into a free page (kexec) |
| #1292 | sw-5 | undetermined | seen | ai-runtime | other | open | one golden logit out of tolerance |
| #1294 | sw-5 | kernel | code | gpu/boot | inherited | open | early L2 flush can land on image/BSS/DTB |

### Summary

**Verdicts (80):** 31 kernel, 17 test, 11 tooling, 5 duplicate, 11
undetermined, 5 not a defect. Leaving out par-1, the one session with no
lab runs, the other 67 split 29 kernel, 16 test, 5 tooling, 4 duplicate,
11 undetermined and 2 not a defect.

**Kernel defects found and fixed: 16 of 31**, by class:

| Class | Found | Fixed | Open |
|---|---|---|---|
| lock or ordering | 11 | 9 | 2 (#1201, #1241) |
| inherited boot or device state | 9 | 4 | 5 (#1212, #1235, #1277, #1291, #1294) |
| memory or allocator | 2 | 0 | 2 (#1240, #1276) |
| other | 9 | 3 | 6 (#1210, #1246, #1250, #1268, #1269, #1280) |
| **Total** | **31** | **16** | **15** |

The fixed 16 landed in PRs #1208, #1209, #1225, #1237, #1239, #1265, #1266,
#1285 and #1293. One more kernel defect was fixed without an issue of its
own: the PMM buddy-merge defect in PR #1283, found under #1207 (switch-5,
"Part B"). It was not what halted the board.

How the 31 are known: 12 failed in a run, 3 were shown by a probe, and 16
come from code reading, mostly by critics. Of the 16 fixed, 8 came from
code reading.

**Where the kernel defects cluster:**

| Area | Kernel defects | Fixed | Issues |
|---|---|---|---|
| Scheduler and SMP | 11 | 9 | #1194, #1210, #1213, #1216, #1229, #1234, #1236, #1238, #1241, #1242, #1263 |
| Boot path and handed-over state | 7 | 5 | #1196, #1197, #1198, #1206, #1212, #1244, #1277 |
| GPU memory after kexec, allocators | 5 | 1 | #1207, #1240, #1276, #1291, #1294 |
| Drivers (UART, camera I2C, mailbox) | 3 | 0 | #1201, #1235, #1280 |
| Build configuration, libc, vendored code | 3 | 1 | #1199, #1246, #1250 |
| Shell | 2 | 0 | #1268, #1269 |

Nine of the 11 lock and ordering defects are in the scheduler. Five of the 9
inherited-state defects are specific to the kexec path (#1196 on both paths,
#1207, #1244, #1291, #1294), and one is the reverse case, a UEFI-direct
path missing what Linux set up (#1235).

**Test defects:** 17, of which 12 are test isolation; 6 are closed (PRs
#1215, #1282). **Tooling and process:** 11, all open. **Duplicates:** 5, all
closed. Four of them are symptom issues filed before their root-cause issue
(#1203 for #1216, #1219 for #1229, #1223 for #1213, #1224 for #1206); the
fifth is #1255 for #1258. **Undetermined:** 11, all open. Eight are single
occurrences (#1220, #1221, #1232, #1260, #1262, #1278, #1288, #1292); #1228
probably matches #1207 by signature but is not confirmed.

## 3. The original question

The question asked on Nov 8: is the growing ticket count a symptom of
architectural problems, or of complexity plus more capable models?

**Opened per merged rose:** 2.02 (Nov 1 to 8), 2.11 (to Nov 13), 4.00
(Nov 13 to 17); 2.31 for November. The ratio is not a quality signal for
this period. The chain was built to find defects: gauntlets, critics, a CI
build matrix and a triage. It merged few, mostly small PRs, and 10 of the 29
merges since the review were an unrelated batch. What the ratio measured
here is how hard the project looked.

**Hub shares did not fall where the hub was removed, by this metric.** The
NC axis is gone from the code: 23 files to 1, and 84 to 57 conditionals in
`sched.c`. Its keyword share went 11.2% → 14.4% → 12.2%. The rise came from
gauntlet issues that mention #1193 as context. The fall came from the triage
batch, not from the removal. The configuration share rose from 9.8% to 16.4%,
because the first CI matrix run found six configuration defects in one pass.
That supports the review's claim that the matrix was a real hub. FFI and
shell moved only by triage, and no work touched them.

**The origin mix shifted toward hardware and review.** Hardware plus review
went from 65% (Nov 1 to 8) to 97.5% (Nov 8 to 13) to 82.5% (Nov 13 to now).
Agent follow-ups fell from 27.5% to 2.5% and then 7.5%. That is the chain's
design showing through, not an independent trend.

**The loops mostly found real defects.** Of 80 issues, 48 are real defects:
31 kernel and 17 test. 16 are noise or bookkeeping (5 duplicates, 11
undetermined) and 16 are process items or decisions. Half of the kernel
defects were fixed within the nine days. They cluster where the review
pointed: scheduler lock and ordering, inherited boot state, the
configuration matrix.

**What the data supports:** the defects the verification loops found are
concentrated in the review's named hubs (scheduler/NC, configuration) and in
one area it did not name, kexec-inherited state. Most are real.

**What it does not support:**

- Any claim that removing a hub lowers the ticket rate. The NC removal is
  six days old at this cut, and the keyword metric counts mentions.
- Any separation of "model capability" from "architecture". One model
  family ran the whole chain, so there is no contrast.
- Any trend in opened-per-merged. Nine days of deliberate defect-hunting is
  one regime, not a trend.

## 4. The review's nine recommendations

| # | Recommendation | Status | Evidence |
|---|---|---|---|
| 1 | Re-validate the coherency premise; delete the NC axis if it holds | **Done** | #1193 (closed); PRs #1208, #1209, #1215, #1217, #1225 (removal, main 98e17440), #1227; ADR 0007; `check-cpu-cache-maint` gates `make test` |
| 2 | Dissolve `shell_sys.c` into per-subsystem registration | **Not started** | no issue or PR |
| 3 | Generate the FFI; make the boundary the only path | **Not started** | no issue or PR |
| 4 | Convert the top prose rules into checks | **Partial** | New checks: `check-cpu-cache-maint` in `make test`, `bench stealing --assert-shares` (#1227), per-CPU layout tests (#1285), image_size test (#1286), boot-time DTB overlap check (#1265), `PMM_CHECK` verifier (#1283). Not done: the four rows the review named. There is no `_Static_assert` for `TASK_CONTEXT_OFFSET`, no lockdep-lite, and no UART-under-dispatch-lock assertion. `runtime-test` exists but is not a `make test` prerequisite. The `PD_WITNESS` witness is #1287 (open) |
| 5 | Shrink and automate the configuration matrix | **Partial** | Report-only build matrix merged (#1251); its failures filed as #1246 to #1250 and #1256, all open; #1257 (hand-copied defaults) open. Not done: `COOP_PREEMPT` not folded, no option retired, warnings not errors |
| 6 | Give `kernel/src` owners (split into subdirectories) | **Not started** | no issue or PR |
| 7 | Measure the question directly (labels, triage, monthly tracking) | **Partial** | Origin and kind labels on all 236 issues open at Track A; 138 milestones; triage closed 50 (parallel-1 Entries 5 to 9). Tracking is manual (Track A, this document). 6 of 40 issues since Track A have no origin label. Defects per thousand lines is not tracked |
| 8 | Refresh the architecture record | **Done** | #1245 (architecture.md, ADRs 0009 to 0017, ADR README with the numbering rule). Stale text from it tracked in #1261 (open). The December 2025 document is outside the repository |
| 9 | Treat #1047 as an epic; audit lock sections for the pattern | **Not started** | no tracking issue. The per-CPU audit in #1263 / #1279 looked at cache-line placement, not the #1047 pattern |

## 5. Next-chain options

| Candidate | Evidence from step 2 | Rough size |
|---|---|---|
| **Lock discipline: Rec 4's lockdep-lite and IRQ-state assertion, Rec 9's lock-section audit, the open scheduler items (#1241, #1210, #1262, #1288, #1287)** | Lock and ordering is the largest class: 11 of 31 kernel defects, 9 of the 16 fixes, 9 of them in the scheduler. Each was found by gauntlet stress or a critic's code reading (#1234, #1236, #1241). None was caught by a check. #1201 (UART lock) is the same pattern in a driver | about one gauntlet run: 2 to 3 days, plus one lab day to run the assertions on both boards |
| #1252 kexec retirement (Jetson, x86-64) | 5 of the 9 inherited-state defects are kexec-only (#1196, #1207, #1244, #1291, #1294). The two hardest open ones (#1291, #1294) and the probable #1207 twin #1228 go away with it, as do the tests that assume kexec (#1231). Against: gate (a) needs UEFI-direct parity, and #1235 is an open UEFI-side gap | large: several runs over weeks (three gates, two x86 loaders) |
| Scheduler and PMM backlog alone | Scheduler: #1241, #1210, #1262, #1288 (the first two from code reading). PMM: #1240, #1276, and #1228 to confirm or close | small to medium: 1 to 2 days plus lab; overlaps the first row |
| Rec 5 remainder (fold `COOP_PREEMPT`, retire options, warnings as errors, fix #1246 to #1250, #1256, #1257) | One matrix run found 6 configuration defects; configuration share rose to 16.4% | small: 1 to 2 days, no lab |
| #1258 licences (replace wolfSSH/wolfCrypt) | No step-2 defect; legal exposure for `NET_SSHD=ON` images, not ticket reduction | medium to large: choose a stack, port the IO hooks, RNG and password auth |
| #1253 runtime model weights | No step-2 defect | large |
| User/kernel split | No step-2 defect at the user/kernel boundary | very large |
| Rec 2 (`shell_sys.c`) | 2 shell defects, both trivial (#1268, #1269); shell share flat | medium, mechanical |
| Rec 3 (FFI generation) | No FFI defect found by the chain; the FFI share fell only by triage | large |
| Rec 6 (`kernel/src` split) | No step-2 evidence | medium to large, mechanical |

**Recommendation: lock discipline.** It targets the class that produced the
most real defects, in the subsystem where they cluster. It turns the
gauntlet's way of finding them (stress plus a critic) into a debug-build
assertion that fails at the author's desk, which is the review's Finding 6
mechanism applied where the data says it pays. It is also the one open
recommendation with direct step-2 evidence, and it is small enough for one
run.

**Runner-up: #1252 kexec retirement.** It would remove a whole defect
source rather than add a check, and its evidence is nearly as strong: 5
kexec-only defects, including the two hardest open ones. It ranks second
on size and on its parity gate.

## 6. Blind critic

A Fable critic, run blind and read-only. It was given Track A's method,
the cut times, the regex, the ten issue numbers and the verdict and class
definitions. It was not given this document's numbers or classifications,
and it fetched its own data at 22:15Z; nothing changed after the cut.

**Step-1 numbers: 3 of 3 agree.**

| Number | This document | Critic |
|---|---|---|
| Opened / merged, Track A to now | 40 / 10 = 4.00 | 40 / 10 = 4.0 |
| Open at now, and at Track A | 213, 236 | 213, 236 |
| NC hub share at now | 26 of 213 (12.2%) | 26 of 213 (12.2%), same 26 issues |

**Step-2 classifications, ten drawn at random** (`random.seed(20261117)`,
`random.sample` over the 80): #1196, #1213, #1224, #1229, #1231, #1242,
#1249, #1250, #1268, #1288.

| Field | Agreement |
|---|---|
| Verdict | 8 of 10 |
| Class | 9 of 10 |
| Subsystem | 10 of 10 |
| State (and closing PR) | 10 of 10 |
| Whole row | 8 of 10 |

The two disagreements:

- **#1224.** The critic called it a kernel defect, class inherited. This
  document calls it a duplicate of #1206, class other. The critic's own
  evidence names the same root cause as #1206: an orphan `.got` past
  `__data_end`, fixed in the linker scripts by PR #1239. So the defect is
  the same, and the difference is whether the symptom issue counts
  separately. Kept as a duplicate, so it is not counted twice. The critic
  called its class choice defensible either way.
- **#1249.** The critic called it a kernel defect, because an opt-in build
  option fails to link. This document calls it a test defect, because the
  failing file is `kernel/tests/jetson_el1_smoke.S`. The critic listed
  `test` as arguable. Kept as a test defect. Counted as kernel instead, the
  totals would be 32 kernel and 16 test.

Critic notes taken:

- #1231 is partly addressed: PR #1282 skips 4 of its 9 tests, and 5 remain
  listed. The row now says so.
- #1196 and #1250 are latent: neither was seen failing on hardware. That
  matches their "code" or "seen (warnings only)" entries.
- #1288 could also be read as a tracking item rather than undetermined.

## Appendix: origin classifications made in this pass

Issues with no `origin:*` label at the "now" cut. The notes give the
evidence used.

| Origin | Issues |
|---|---|
| review | #1065 to #1072 (outside audit, July 2026); #1103, #1111, #1113, #1173 (code review); #1193 (the review's Recommendation 1); #1194, #1196, #1197, #1198 (coherency run 1 audit); #1236 (code reading) |
| hardware | #1053, #1063, #1079, #1086, #1093, #1105, #1161; #1162 (gauntlet QEMU run); #1199, #1202, #1203, #1205, #1216, #1224; #1213, #1219, #1229, #1260 (gauntlet QEMU runs); #1259, #1262, #1290, #1291, #1292 |
| agent-followup | #1050, #1051, #1055, #1058, #1110, #1115, #1116, #1119, #1130, #1136, #1144, #1158 (found reading code while building goldens), #1167 |
| user | #1084, #1088, #1089, #1092 |
| golden | #1156, #1157 |

---

*Prepared from GitHub and repository data at the "now" cut.*
