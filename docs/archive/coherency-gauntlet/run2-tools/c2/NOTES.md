# C2 hardware notes (#1203, coherency run 2) -- state that must survive context compaction

## Grant and rules (from orchestrator)
- jetson-nano-2 granted for steps 2-3 (+4-6 as needed). IP <jetson-nano-2 IP from lab://sbcs>. Claim held by orchestrator; never claim/renew/release.
- Cells: nc 983bddc9 (cells/slmos-c2-nc.*), ncoff cbab2fc6 (cells/slmos-c2-ncoff.*), removal c1846b3b (cells/slmos-c2-removal.*; old f87d9e47 image kept as cells/slmos-c2-removal-f87d9e47-d4af31aa.*).
- Order: ncoff-kexec, ncoff-uefi, removal-uefi, removal-kexec, nc-kexec, nc-uefi.
- Per boot: 20 x `bench stealing`, then 20 x `bench stealing --sustain 2500`, then `coherency litmus` once if the command exists (save as c2/<cell>-<path>-litmus.txt). Boot log c2/<cell>-<path>-boot.txt from trigger to prompt.
- serial_send needs a confirm_token round trip each time; capture_until="slmos>".
- UEFI: only scripts/jetson-uefi-deploy.sh <jetson-nano-2 IP from lab://sbcs> <bin>; after any ESP write, from Linux: c2/bin/esp-check.sh (fsck.vfat -n, ro mount sha, not mounted, BootNext).
- Restore at end: scp read /root/slmos-efi-pre1193.efi from board into scratchpad/c2/, check sha starts 789cba62, deploy with jetson-uefi-deploy.sh (capture boot to c2/restore-boot.txt), power_cycle to SD Linux, esp-check (789cba62, no BootNext). If missing or sha differs: stop and report.
- All new files under scratchpad/c2/ (shared scratchpad).
- Fix landing on the removal branch needs orchestrator go-ahead; report the proposed fix first. Memory-ordering change beyond lints = stop condition.
- Hand-over state: SD Linux, BootCurrent 0001, no BootNext, SLMOS.efi 789cba62, l4t.efi 91db497c (c2/hw/esp-handover.txt).

## Evidence extraction
- Replies are taken verbatim from this agent's transcript with c2/bin/extract.py (list / dump <tool_use_id> <out>).
- Map by order: per cell, the boot capture (serial_capture with until slmos>|PANIC...) then confirmed sends.
- Evidence branch gauntlet-2-hw-jetson, dir docs/archive/coherency-gauntlet/run2-hw/c2/, names <cell>-<path>-{burst,sustained}-runNN.txt, <cell>-<path>-boot.txt.

## Progress
- Tools: c2/bin/assign.py <cell-path> <since> <until> c2/evidence (writes files from transcript); c2/bin/hist.py c2/evidence <cell-path> burst|sustained.
- ncoff-kexec: DONE (boot 09:03:44Z; 20 burst, 20 sustained, litmus PASS). Files in c2/evidence (42 + trigger). Window used: since 2026-11-10T09:03:00.
  burst: cpu2 exec0 10/20, cpu3 exec0 17/20; dominant f_pd (cpu3 41 of 57 att). sustained: cpu2 11.1% cpu3 11.4% vs ~19.5%; f_pd 6494/6554; cpu3 0 ok in run16 (508 att); first_ok max 1514 ms.
- ncoff-uefi: PARTIAL. Boot logs LOST twice (serial_capture queued ~8 min in labctl, 0 lines); identity via `cat /sys/version` (cbab2fc6). 6 burst runs done (cpu2/cpu3 exec0 6/6). No sustained, no litmus.
- Evidence pushed: gauntlet-2-hw-jetson @ eb6f0f4f, docs/archive/coherency-gauntlet/run2-hw/c2/ (56 files).
- PAUSED 10:26Z and reported to orchestrator (checkpoint B): defect found; proper fix changes scheduler memory ordering (stop condition).
- Board at pause: SD Linux, on, BootCurrent 0001, no BootNext, ESP SLMOS.efi = 73b7f479 (ncoff), NOT restored. /root/slmos-efi-pre1193.efi present sha 789cba62. /root/slmos-c2-ncoff.elf on board.
- Remaining: ncoff-uefi 14 burst + 20 sustained + litmus; removal-uefi, removal-kexec, nc-kexec, nc-uefi (40 each + litmus); ESP restore.

## Findings so far (ncoff-kexec)
- Run-1-style "Steal counter deltas" table is read AFTER the results print, so it counts idle attempts made while UART prints (e.g. CPU 1 table 10 vs trace 2). Run-1 "10-16 attempts" were mostly post-run.
- Burst: CPUs 2,3 exec 0 in most runs; their in-run attempts are 2 back-to-back `P` (preempt_disabled[victim] skip, v_pd_work=2: victim had work) at a single wake, then next attempt one tick (10 ms) later after the deque drained. CPUs 2 and 3 wake at the same instant; CPUs 4 and 5 wake together at another instant. Hypothesis H3: per-CPU 100 Hz ticks are phase-aligned within a cluster; idle thieves (WFI) wake only on their tick; owner CPU 1 (cluster 0) is in its own tick-driven schedule() (preempt_disabled[1]=1) at the same instant, so cluster-0 thieves skip it and sleep a full tick.
- Sustained run 1: CPUs 2,3 13% share vs ~18.5% others; f_pd 235/310 with v_pd_work 228/304; P failures at 10 ms tick multiples. Not stuck, but systematically losing tick-aligned attempts.
- Candidate fix (not landed): the preempt_disabled early-out is "belt-and-suspenders" (rq_lock is the real protection); dropping it, or retrying instead of returning to WFI, removes the skip. Not a memory-ordering change.
