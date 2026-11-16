#1284 is decided, by the session's rule, as **the test's own race** rather than a defect in this PR's resume path. It is a decision by exclusion (no witness violation anywhere); the race itself was not observed. Details, limits and evidence are in the #1284 comment and in `gauntlet/switch-5`, `docs/archive/switch-gauntlet/switch-5.md` Entry 56.

In short: a kernel-side witness (`PD_WITNESS`, debug only, not on this branch) checked `preempt_disabled[]` at `schedule()`'s resume point, before the post-switch clear (must be 1) and after it (must be 0), with IRQs masked. On jetson-nano-2, UEFI-direct, the original test reproduced `bad=1` in 1 of 7000 iterations with 0 witness violations, and the fixed test had `bad=0` in 7000 iterations with 0 violations (one kexec boot per arm: 0 `bad`, 0 violations). The witness fires on the reverted #1229 line in QEMU. A blind critic passed the rows with no blocking finding.

**Pushed to this branch:** cd1557fe, test only (`kernel/tests/test_scheduler.c`): `test_preempt_flag_follows_migrated_task` reads `cpu_id()` and `preempt_disabled[c]` with local IRQs masked (FIQ too under `PI5_FIQ_TIMER`), restored right after. The assertion is unchanged, and with the #1229 line reverted the test still fails 20 of 20 in QEMU. `/ci` at cd1557fe: 11 of 11 steps pass.

Note for #1285 (stacked on this branch): #1285 rewrites the same test (three workers, masked reads), so bringing cd1557fe into #1285 conflicts in `kernel/tests/test_scheduler.c`; #1285's version already contains the same masked read.

Not merged.
