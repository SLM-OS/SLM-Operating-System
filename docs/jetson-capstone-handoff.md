# Jetson Capstone — Session Handoff

Snapshot for a new agent picking up this work after the S4 default-flip
landed (PR #167, merge commit `3d416e8`, 2026-04-15).

**Read this first; it has the context that the long-form plan docs
assume.** When this doc and the long-form docs disagree, the long-form
docs are authoritative — file an update to this one.

---

## 1. Where the project is right now

The minimum capstone path (G1 → G6) is **complete**. Track S is through
**S4**. Track P is parked at **P1** (cooperative preemption).

| Track | Status | Notes |
|---|---|---|
| Prereqs #1-#4 | ✅ DONE | Shared infrastructure for the cross-platform plans |
| P1 (cooperative preemption) | ✅ DONE | `COOP_PREEMPT` synthesizes ticks at yield points; hardware-verified on Jetson |
| P2-P6 (true preemption) | 🚫 deferred | Hard-blocked on TF-A GIC Group-config (EL3-owned). Investigation complete (2026-04-15): all 8 NS-accessible paths eliminated. See `docs/jetson-preemption-investigation.md`. `timdiag` shell command added for future diagnostics. Not in capstone scope |
| G1-G6 | ✅ DONE | NEON MatMul / Conv / LN/RMSN/GELU / FP16+INT8 / cross-platform bench / thesis framing |
| S1 | ✅ DONE | NC-memory steal deque placement |
| S2 | ✅ DONE | Steal counters (closed #105) |
| S3 | ✅ DONE | `bench stealing` + Phase C QEMU+Pi 5 numbers |
| S4 | ✅ DONE | Default flip ON for x86-64 + Pi 5 (PR #167), plus Jetson ON follow-up (2026-04-15) after #166 fix. QEMU stays OFF by design (test-harness flakes) |
| S5 (load-balancing) | ✅ DONE | Proactive `least_loaded_cpu` override in `scheduler_add_task`; see `docs/jetson-capstone-execution-plan.md` §S5 |

**Capstone defense readiness:** the engineering work is delivered. What
remains is non-engineering polish (demo, thesis writeup, integration
sweep) plus a small bug backlog (below).

---

## 2. Recent merge history (most recent first)

Pull `git log --oneline main` for the live view. As of handoff time:

| Commit | Subject | What landed |
|---|---|---|
| `3d416e8` | Merge PR #167 | S4: work-stealing default ON for x86-64 + Pi 5 |
| `ec82de2` | S4 docs + test stabilization | Shape-A retry test, doc audit, QEMU scoped to OFF |
| `f8d66bc` | S4 default flip | `CMakeLists.txt` per-platform option block |
| `1495398` | Fix #158 (and prep S4) | External `steal_deque_lock[MAX_CPUS]` (cacheable) |
| `c978a76` | Merge PR #145 | Fix #139: per-slot generation counter (ABA) |
| `7c5d7b2` | Merge PR #144 | G2-G6 + S2-S3 |

If a fresh agent loads `main` and runs `make test` and `make test
WORK_STEALING=ON`, both should be green on QEMU ARM64 (with the
caveats in §6).

---

## 3. Open items, in priority order

### 3a. Bugs from this work

| # | Title | Priority | Notes |
|---|---|---|---|
| #166 | Jetson page fault during `bench stealing` with WORK_STEALING=ON | ✅ closed | Root cause: Jetson hard-defined `SPINLOCK_SKIP_LOCKING` in `platform.h`, making every cacheable spinlock (PMM, task-table, rq_lock, steal_deque_lock) a no-op under SMP. Concurrent `pmm_free_pages` calls corrupted the buddy free list, faulting inside `free_list_add`. Fix (2026-04-15): switched Jetson to Pi 5's runtime `spinlock_hw_enabled` model — barrier-only pre-MMU, real LDAXR/STXR post-MMU. Verified 3 consecutive clean `bench stealing 16` runs (20.9 ms, 2/4/4/4/1/1 distribution across all 6 CPUs). Also bundled the `nvidia_vbios_platform_load` link-error fix (the Jetson build hazard called out in §6). |
| #141 | x86-64 build broken after G3 inference kernels (libm f16 + fat LTO) | ✅ closed | Resolved in PR #173 (merge commit `f389e84`) by replacing `libm::sqrtf` / `libm::tanhf` with scalar `runtime::mathf` approximations. |
| #174 | `steal_deque.h` doc clarity | P3-low | Docs-only, ~10 min |
| #175 | Push-failure counter for `steal_deque` | P3-low | ~20 LOC, observability-only |
| #158 | Pi 5 boot hang | ✅ closed | Fixed by external lock in PR #167 |
| #139 | Work-stealing ABA race | ✅ closed | Fixed by per-slot generation counter in PR #145 |
| #142 | GSP bare-metal loader (future work) | partially addressed | ARM64 platform shim + nvgpu-native bringup scaffold + FECS method gateway. Phases 1–5 plumbed with 41 host tests (26 bringup + 15 platform shim). **Phase 5 hardware-verified on jetson-nano-2**: FECS responds to method submission from SLM-OS (DISCOVER_IMAGE_SIZE = 513,280 bytes). Priv-lockdown (#190) resolved via Path 3. Channel/pushbuffer (phases 6–7) needed for compute dispatch. |
| #190 | Jetson GSP Falcon priv-lockdown blocks ACR HS load | ✅ resolved | Root cause: kexec helper's runtime-PM suspend power-gated the GPU; Falcon BROM reasserts bit 13 on re-enable. Fix: `--no-gpu-suspend` flag skips the suspend; `ga10b_bringup_inherit()` detects Linux's post-boot FECS/GPCCS PASS state and skips phases 1–4. Hardware-verified 2026-04-17. |

### 3b. Capstone deliverables outside the engineering plan

These are explicit in the plan's week-by-week calendar (weeks 11-12):

- Demo prep + rehearsal.
- Integration testing sweep.
- Final report writeup using `docs/capstone-thesis-framing.md` as the
  spine.

### 3c. Recommended ordering

If picking what to do next, the honest order is:

1. **Thesis writeup + demo prep** — time-boxed project management,
   no engineering blockers.
2. **#174 / #175 / S5** — genuinely optional polish.
3. **GPU bringup continuation** — scoped below. Each of the three
   unlock paths is research-heavy and has real risk of not landing in
   the capstone window; all are valuable for the thesis appendix
   regardless of outcome.

### 3d. GPU Bringup (branch: `worktree-jetson-gpu-inference`)

Extensive work landed on a side branch targeting Ampere compute. The
platform shim, firmware pipeline, bringup skeleton, diagnostic shell
commands, and Phase 1 ACR HS load are all in place and tested. The
block is a hardware-level priv-lockdown on the GSP Falcon.

**What landed (13 commits ahead of main):**
- `kernel/arch/arm64/nvidia_gsp_platform.c` — full `gsp_platform_ops`
  implementation (all 11 vtable functions) for Jetson.
- `kernel/arch/arm64/nvidia_ga10b_firmware.S` + CMake wiring — `.incbin`
  embedding for all 17 ga10b firmware blobs, gated behind
  `-DGA10B_FIRMWARE_DIR=<path>`.
- `scripts/tools/fetch-ga10b-firmware.sh` — pulls firmware from a
  Jetson's `/lib/firmware/nvidia/ga10b/` to the build host.
- `scripts/jetson-kexec-slmos.sh` — extended to force-enable GPU
  clocks via BPMP debugfs before kexec, so GPU MMIO stays live
  through the Linux → SLM-OS handoff.
- `kernel/gpu/nvidia/ga10b_bringup.{c,h}` — nvgpu-native bringup
  scaffold with Phases 1–4 implemented:
  - **Phase 1 (ACR HS load)**: GA10B-specific engine reset
    (assert→delay→deassert), PIO IMEM/DMEM upload, BCR_CTRL=0x11,
    STARTCPU, halt polling, BR_RETCODE decoding.
  - **Phase 2 (FECS) / Phase 3 (GPCCS)**: STARTCPU on each GR Falcon
    followed by `ctxsw_mailbox[0]` polling for `PASS=1` / `FAIL=2` /
    `CSUM=0x21` sentinels. ACR pre-loads both IMEM/DMEM, so the host
    only has to kick them.
  - **Phase 4 (PMU)**: documented no-op for GA10B default
    (`support_ls_pmu=false`); shell command still exposed for when
    PMU is wired on later.
- `kernel/mm/vmm.c` — extended GPU BAR0 mapping from 1 × 2 MB block
  to 8 × 2 MB blocks so Falcon engine apertures past 0x17200000 are
  reachable.
- Shell commands: `gpu` (info + raw MMIO read), `nvgpu` (firmware
  inventory, per-phase driver: `prepare | acr | fecs | gpccs | pmu | run`).
- `host-tools/gsp-harness/test_ga10b_bringup.c` — 18 tests covering
  firmware accessor, state machine, ACR sequence, and the new
  FECS/GPCCS/PMU phase plumbing against a mock-vtable.
- `host-tools/gsp-harness/test_nvidia_gsp_platform.c` — 15 tests for
  the Jetson platform shim's portable surfaces (vtable install,
  firmware_get dispatch, DMA alignment math, BAR1 early-out).
- `scripts/tests/jetson-gpu-lockdown-probe.sh` — reproducible probe
  that reads `HWCFG2` from live Linux via `/dev/mem`, kexecs SLM-OS,
  re-reads via the shell, and reports the bit-13 delta. Makes the
  #190 measurement reproducible across sessions.
- `docs/archive/investigations/jetson-nvgpu-bringup-research.md`, `docs/archive/investigations/jetson-nvgpu-acr-analysis.md`,
  58 cached L4T nvgpu reference files.

**Hardware verification (jetson-nano-2):**
- GPU MMIO live at EL2+VHE: BOOT_0=`0xB7B000A1`, BOOT_42=`0x17BA1000`
- NVIDIA driver detects GA10B with 1024 CUDA + 32 tensor cores
- All 17 firmware blobs reachable via `nvgpu info`
- **#190 RESOLVED (Path 3, April 17):** `--no-gpu-suspend` kexec
  preserves Linux's Falcon state. `nvgpu inherit` detects HWCFG2
  bit 13 = 0 + FECS/GPCCS PASS → skips phases 1–4.
- **FECS method gateway verified:** `nvgpu test` submits
  DISCOVER_IMAGE_SIZE → FECS returns 513,280 bytes (context size).
  First bare-metal GPU method submission from SLM-OS.
- **CBB firewall mapped (April 17):** PFIFO, CHRAM, NV_USERMODE
  are permanently blocked from EL2. Channel setup requires
  Linux-side pre-creation ("inherit channel" path).

**Merge guidance:** the branch delivers:
- Complete arm64 platform shim (11/11 vtable fns, 15 host tests)
- Phases 1–5 of nvgpu bringup (26 host tests)
- #190 priv-lockdown root-caused and resolved
- FECS method gateway — hardware-verified GPU controllability
- CBB firewall accessibility map — architectural knowledge for
  future channel work
- `peek` shell command for DRAM inspection
- 72+ cached L4T nvgpu reference files

---

## 4. Document map (what to read for what)

The plan family is layered. Read them in this order on a fresh start:

| File | What it tells a new reader |
|---|---|
| **This file** | Where things are right now and what's open |
| `docs/jetson-capstone-execution-plan.md` | The authoritative plan. Phase definitions, prerequisites, calendar, exit criteria. **Look here first** for "what does S5 mean, exactly?" or "what are P3's prerequisites?" |
| `docs/archive/plans/jetson-capstone-gap-analysis.md` | Historical: the gap between what existed pre-capstone and what was needed. Snapshot, not current-state |
| `docs/capstone-thesis-framing.md` | The narrative for the capstone report. Read this before talking about the project — it has the "delivered vs. proposed" framing |
| `docs/cross-platform-inference-bench.md` | The G5 deliverable. Live measurement table |
| `docs/work-stealing-bench.md` | The S3+S4 deliverable. Pi 5 hardware numbers, S4 flip rationale |
| `docs/scheduler.md` §Work stealing | The current locking model + per-platform default table |
| `kernel/CLAUDE.md` | Kernel-specific gotchas. NC memory, spinlocks, MPIDR encoding |
| `runtime/CLAUDE.md` | Rust runtime gotchas. `no_std`, FFI, NEON dispatch |
| `CLAUDE.md` (project root) | Build commands, lab access, formatting conventions |

The cross-platform sister plans for context (read only as needed):

- `docs/archive/plans/x86-64-capstone-gap-closure-plan.md` — x86-64's parallel work
- `docs/pi5-preemption-plan.md` — Pi 5's parallel work

---

## 5. Hardware lab + tooling notes

The lab is managed by `labctl` (MCP server). Two SBCs matter for this
work:

- **pi-5-1** (192.168.4.97): SDWire-flashed deploys via
  `mcp__labctl__sdwire_update`. Boot is fast (~5 s to shell). Use this
  for any work-stealing hardware iteration.
- **jetson-nano-2** (192.168.4.93): kexec-from-Linux deploys via
  `slmos-kexec` (script at `/usr/local/bin/slmos-kexec`). Linux first,
  then SCP, then `sudo -n /usr/local/bin/slmos-kexec /tmp/slmos.elf`.
  After kexec, Linux is gone — power-cycle + wait for Linux to come
  back before the next deploy.

**Critical setup:** passwordless sudo for `slmos-kexec` is configured
on jetson-nano-2 via `/etc/sudoers.d/slmos-kexec`. This was added
during the S4 capture session — don't break it. A fresh agent can
verify with `ssh 192.168.4.93 sudo -n -l /usr/local/bin/slmos-kexec`
which should print the binary path with no password prompt.

**Avoid pi-5-2** (status: unknown, power: on) — it likely belongs to
another project.

**Don't use `cd` between MCP serial commands** — `mcp__labctl__serial_send`
and `mcp__labctl__serial_capture` are stateless and operate on the SBC
name directly.

---

## 6. Known gotchas / footguns

These bit me in the S4 session and will bite the next agent if they
don't know them up front.

### Build / toolchain

- **rustc toolchain note (#141 historical).** Before PR #173 (commit
  `f389e84`), `libm::sqrtf` / `libm::tanhf` tripped a soft-float
  legalization bug on `x86_64-unknown-none` under fat LTO. The G3
  kernels now use `runtime::mathf` scalar approximations and the
  problem is gone.
- **`make kernel-clean` and `cargo clean` separately**. CMake caches
  `ENABLE_WORK_STEALING` per build directory; if a previous run set it
  ON or OFF, a re-configure with a different default won't change the
  cache. After flipping defaults, blow away `build/kernel` and
  `build/kernel-test`.
- **Jetson `nvidia_vbios_platform_load` link error (Fixed).** A stub
  at `kernel/arch/arm64/nvidia_gsp_platform_stub.c` returns -1 so the
  GSP bringup code compiled for Jetson links cleanly. The stub is
  never invoked at boot (Jetson uses the CBB-blocked GPU as a stub
  driver). Bundled with the #166 fix.

### Scheduler / SMP

- **`SPINLOCK_SKIP_LOCKING` retired on Jetson (2026-04-15).** Jetson
  now shares Pi 5's runtime `spinlock_hw_enabled` model. Pre-MMU it
  stays 0 (barrier-only); `vmm_init` flips it to 1 and post-MMU
  spinlocks use real LDAXR/STXR. NC-memory-embedded spinlocks still
  cannot use exclusive monitors (LDAXR/STXR on NC memory does not
  work) — the cacheable `rq_lock[MAX_CPUS]` / `steal_deque_lock[MAX_CPUS]`
  pattern is still required for anything guarding NC data. See #166.
- **Atomic operations on NC memory may fault.** Per ARM ARM and
  documented in `kernel/CLAUDE.md`. The S3 bench driver hits this on
  Pi 5 if it uses `__atomic_fetch_add` against an NC-memory counter.
  Use per-slot single-writer u32 stores instead — already documented
  in the bench driver source.
- **Jetson has dual-cluster MPIDR encoding** (Aff2.Aff1: 0x000, 0x100,
  0x200, 0x300, 0x10200, 0x10300). Any code that does
  `(mpidr & 0xFF) | ((mpidr >> 8) & 0xFF)` to extract a CPU index will
  collide on CPUs 4/5. Use the logical-CPU map in `nc_cpu_logical_map`.

### Tests

- **The QEMU integration suite has pre-existing flakes**. `main` itself
  passes ~50% under `make test` over multiple runs. Test failures from
  `test_benchmark_queue_operations`, `test_rapid_task_exit_no_panic`,
  `test_multicore_basic` are usually pre-existing, not from your
  change. Verify by stashing and testing on `main`.
- **Priority-ordering tests must be pinned to CPU 0** when
  WORK_STEALING is on. `test_priority_ordering_multiple_levels` and
  friends were updated in PR #167; if you add a new
  priority-ordering test, do the same.
- **`test_work_stealing_distributes_load` is structured as
  retry-then-assert** (Shape A). It runs the scenario 8 times and
  passes if ≥ 2 attempts had at least one task land on a CPU other
  than the owner (CPU 1). The previous "≥ 2 distinct CPUs ran tasks"
  criterion was retired on Pi 5 in April 2026 because a single awake
  stealer can legitimately absorb all 5 tasks before others wake
  — see kernel/CLAUDE.md "`test_work_stealing_distributes_load`
  success criterion" and issue #216 for the dormancy pattern that
  surfaced it.

### Hardware iteration

- **kexec from a non-TTY ssh** needs `sudo -n` to bypass the password
  prompt. The sudoers file is set up on jetson-nano-2; verify before
  each first-time use.
- **Long bench output overruns `mcp__labctl__serial_send`'s response
  size.** When this happens the MCP server saves the raw text to a
  file under `~/.claude/projects/.../tool-results/` and returns the
  path. `grep` it directly; don't try to read 200 kB into context.
- **Background kexec ssh process** doesn't return — the SSH session
  dies when Linux dies. Kick it off with `ssh ... &` and then start
  the serial capture; don't wait on the ssh PID.

---

## 7. Verification recipes

If the next agent wants to confirm the project is in the state this
doc claims:

```bash
# Default QEMU build green (will fail flakily ~20%; that's pre-existing)
make test

# WORK_STEALING=ON green on QEMU
make test WORK_STEALING=ON

# Pi 5 hardware: bench stealing 16 should print 12-13 ms total,
# 4/4/4/4 distribution
make kernel PLATFORM=RASPI5
# (then deploy via mcp__labctl__sdwire_update + serial_send)

# Jetson hardware: bench stealing 16 in OFF mode should be clean
# (~20 ms). ON mode is also clean (20.9 ms, 2/4/4/4/1/1 distribution)
# since #166 was fixed 2026-04-15.
make kernel PLATFORM=JETSON_ORIN_NANO WORK_STEALING=ON
```

Live numbers are in `docs/cross-platform-inference-bench.md` and
`docs/work-stealing-bench.md`.

---

## 8. What "done" looks like for the capstone

The defense criteria the project is targeting:

1. **A small language model OS that boots on multiple platforms.**
   ✅ — QEMU ARM64, Raspberry Pi 5, Jetson Orin Nano, x86-64
   (#141 closed via PR #173).
2. **NEON-accelerated CPU inference with FP32 / FP16 / INT8 paths.**
   ✅ — G1-G4 land the kernels; `bench matmul` and `bench quant`
   demonstrate.
3. **Cross-platform performance numbers showing portability.**
   ✅ — `docs/cross-platform-inference-bench.md`.
4. **An honest narrative about GPU support.**
   ✅ — `docs/capstone-thesis-framing.md` §"GPU Support and the GSP
   Blocker" explains why CPU is the delivered path.
5. **A scheduler that demonstrates SMP + work-stealing measurably.**
   ✅ — Pi 5 hardware shows 3.12× speedup; default ON for hardware
   platforms.

The non-engineering remainder is the capstone report draft + demo,
plus optional bug cleanup.

---

*Created: 2026-04-15. Update this file whenever a session ends with a
non-trivial change to the open-items list or the verification
recipes.*
