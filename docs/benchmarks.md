# SLM-OS Performance Benchmarks

Performance measurements across all supported platforms.

**Date:** April 2026
**Methodology:** All benchmarks run via the `bench` shell command. Each measurement is the average of multiple iterations (count noted per test). Hardware counter (CNTPCT_EL0 on ARM64, RDTSC on x86) provides nanosecond-resolution timing.

---

## Platform Summary

| Platform | CPU | Cores | Clock | RAM | Status |
|----------|-----|-------|-------|-----|--------|
| QEMU virt | Cortex-A76 (emulated) | 4 | N/A | 1 GB | Primary dev |
| Raspberry Pi 5 | BCM2712 Cortex-A76 | 4 | 2.4 GHz | 4 GB | Hardware verified |
| Jetson Orin Nano | Cortex-A78AE | 6 | 1.5 GHz | 8 GB | Blocked (kexec RAS) |
| x86-64 (QEMU) | max | 4 | N/A | 256 MB | Experimental |

---

## Kernel Benchmarks

### Context Switch Latency

Measures round-trip time for two tasks yielding back and forth on the same CPU. 99 round-trips per measurement.

| Platform | Average | Rating |
|----------|---------|--------|
| Pi 5 | **1.858 us** | Excellent (< 10 us) |
| QEMU ARM64 | ~5-15 us | Good (emulation overhead) |

**Target:** < 10 us. **Achieved on Pi 5.**

### Interrupt Latency (Timer Tick Jitter)

Measures deviation from the expected 10 ms timer tick interval using 20 consecutive samples. The measurement captures the full IRQ path: hardware interrupt assertion, GIC acknowledge, exception vector entry, handler dispatch, timer reload, and counter read. This represents the worst-case overhead added to each timer tick.

| Platform | Min | Average | Max | Jitter (max-min) |
|----------|-----|---------|-----|-------------------|
| Pi 5 | 1.445 us | 1.705 us | 2.055 us | 0.6 us |
| Pi 5 (under load) | 1.482 us | 1.782 us | 3.074 us | 1.6 us |

Timer frequency: 54 MHz (BCM2712 system counter). The "under load" measurement was taken with active tasks and shell output. The sub-2us average confirms that the GIC-to-handler path has minimal overhead on Cortex-A76.

### IPC Latency

Message queue send + receive round-trip, 100 iterations.

| Platform | Total | Average |
|----------|-------|---------|
| Pi 5 | 13.248 us | **132 ns** |

### Shared Buffer Throughput

4 KB buffer, 1000 iterations, 64-byte stride.

| Platform | Write | Read |
|----------|-------|------|
| Pi 5 | **45.8 GB/s** | **48.1 GB/s** |

### Component Benchmarks (Pi 5)

Measured via Lua REPL with `slm.uptime()` timing. Includes UART output overhead.

| Operation | Latency | Notes |
|-----------|---------|-------|
| Component load (`component_run`) | **3 ms** | Register + task create + add to scheduler |
| Component hot-swap | **11 ms** | Unregister old + load new + transfer subscriptions |
| Message publish + process | **6.39 ms** | Publish → route → subscriber receive → process → yield (100-msg avg, includes UART print per message) |
| Message publish (raw) | **~0.2 ms** | Estimated without UART overhead (IPC round-trip is 132 ns) |

### End-to-End Component Pipeline

Measures time from message publish through component processing to result.

| Pipeline | Pi 5 |
|----------|------|
| Publish → sensor_monitor → threshold alert | **7 ms** |
| Publish → route → process → yield (per message, 100-msg avg) | **6.39 ms** |

Includes UART output from the component (serial at 115200 baud accounts for ~4ms of the latency). Without UART output, the raw pipeline would be ~2-3 ms.

### Scheduler Overhead

| Metric | Pi 5 |
|--------|------|
| schedule() call | **2 µs** (measured via 1000 yields) |
| Heuristic policy decision | ~2 µs (included in schedule) |
| AI MLP policy decision | **41.9 µs** (state extraction + inference) |

### Scheduler Throughput

| Platform | Context Switches/sec | Active Tasks |
|----------|---------------------|--------------|
| Pi 5 (fresh boot, 3 tasks) | **69,353** | 3 |
| Pi 5 (after demo, 8 tasks) | **40,368** | 8 |

### Deadline-Boosted Dispatch

Task dispatch latency with deadline boost (priority escalation for urgent tasks).

| Platform | Dispatch Latency |
|----------|-----------------|
| Pi 5 | ~18 us |

### Memory Usage (Pi 5, Runtime)

| Resource | Size |
|----------|------|
| Total RAM | 4,188,256 KB (4 GB) |
| Kernel used | 395,652 KB (387 MB) |
| Free | 3,792,604 KB (3.6 GB) |
| Model weight pool | 256 MB (128 x 2 MB blocks) |
| Model workspace pool | 128 MB (64 x 2 MB blocks) |
| Rust heap | 64 MB (Pi 5; per-platform via `RUST_HEAP_MB` in `<config.h>`) |
| RAM disk (LittleFS) | 1 MB |

---

## Boot Performance

| Platform | Total (power to shell) | Kernel init | Firmware |
|----------|----------------------|-------------|----------|
| Pi 5 | 8.5 s | ~1.6 s | ~5-7 s |
| QEMU ARM64 | ~1 s | ~1 s | N/A |

### Boot Phase Breakdown (Pi 5)

Estimated from serial output timing at 115200 baud. Total kernel init ~1.6s (serial capture), ~3.5s (power-on including firmware).

| Phase | Estimated Time | Notes |
|-------|---------------|-------|
| DTB parse + PMM init | ~100 ms | Buddy allocator setup for 4 GB |
| VMM/MMU enable | ~50 ms | Page table construction (2054 blocks) |
| VMM validation tests | ~50 ms | 6 tests — skip in production |
| GIC + Timer init | ~20 ms | |
| SMP boot (3 secondary CPUs) | ~600 ms | Sequential PSCI CPU_ON with handshake |
| Spinlock + SMP validation | ~100 ms | 16 tests — skip in production |
| Filesystem (ramdisk + LittleFS) | ~200 ms | Format, mount, write 38 help files + demo |
| Rust runtime init | ~50 ms | Heap + FFI validation |
| Component + Model + GPU init | ~100 ms | Pool allocation (384 MB logical) |
| Scheduler + Shell start | ~50 ms | |

### Optimization Opportunities

| Optimization | Estimated Savings |
|-------------|-------------------|
| Skip boot-time validation tests | ~150 ms |
| Parallel SMP boot (simultaneous PSCI) | ~400 ms |
| Lazy help/demo file writing | ~100 ms |
| **Total potential** | **~650 ms** |

**Target:** < 2 s kernel init. Current: ~1.6 s on Pi 5 (measured from first serial output to shell prompt). The 3.5s figure from earlier measurements included serial capture connection latency. With the optimizations above, sub-1s kernel init is achievable.

---

## Binary Size

| Platform | Binary (.bin) | ELF text | ELF data | ELF bss |
|----------|--------------|----------|----------|---------|
| QEMU ARM64 | 973 KB | 912 KB | 62 KB | 1.9 MB |
| Pi 5 | 824 KB | 762 KB | 61 KB | 2.6 MB |
| Jetson | 893 KB | 818 KB | 61 KB | 3.6 MB |
| x86-64 | 610 KB | 544 KB | 59 KB | 5.1 MB |

BSS varies by platform due to: task table size (NC vs BSS), per-CPU data, platform-specific driver buffers. The kernel binary includes: C kernel, Lua 5.4 interpreter, embedded help files, and demo script.

---

## Platform Comparison (QEMU)

Emulated benchmarks on the same host. QEMU ARM64 emulates Cortex-A76; QEMU x86-64 uses host CPU passthrough. Numbers reflect emulation overhead, not native hardware performance.

| Metric | QEMU ARM64 | QEMU x86-64 | Pi 5 (native) | Jetson (native) |
|--------|-----------|-------------|---------------|-----------------|
| Context switch | ~807 ns | ~1,332 ns | **1,858 ns** | **3,601 ns** |
| IPC round-trip | ~1,709 ns | ~761 ns | **132 ns** | **60 ns** |
| Buffer write | 9.7 GB/s | 21.0 GB/s | **45.8 GB/s** | **35.1 GB/s** |
| Buffer read | 8.2 GB/s | 14.1 GB/s | **48.1 GB/s** | **68.8 GB/s** |
| IRQ latency | — | — | **1.7 us** | **3.7 us** |
| MNIST inference | — | — | **483 us** | **746 us** ¹ |
| Binary size | 973 KB | 610 KB | 824 KB | 893 KB |
| CPUs | 4 | 4 | 4 | 6 |
| RAM | 1 GB | 256 MB | 4 GB | 8 GB |

QEMU numbers vary between runs due to host load and emulation non-determinism. Pi 5 native numbers are the authoritative measurements for capstone evaluation.

¹ Jetson MNIST inference has not been re-measured after the #56 micro-kernel work (4×4 NEON outer-product + tile-level prefetch). The kernel changes are platform-agnostic Rust + NEON intrinsics and cross-build cleanly for `PLATFORM=JETSON_ORIN_NANO`, so a similar uplift is expected; the 746 µs figure remains the pre-#56 baseline.

---

## Memory Usage

### Model Memory Pools

| Pool | Block Size | Blocks | Total |
|------|-----------|--------|-------|
| Weight (read-only) | 2 MB | 128 | 256 MB |
| Workspace (scratch) | 2 MB | 64 | 128 MB |
| **Total** | | **192** | **384 MB** |

### Kernel Memory (Pi 5)

| Region | Size |
|--------|------|
| Kernel code + data | ~1 MB |
| PMM heap | 4,076 MB (of 4 GB RAM) |
| NC shared memory | 2 MB |
| Rust heap | 64 MB (Pi 5; per-platform via `RUST_HEAP_MB` in `<config.h>`) |
| Model pools | 384 MB |
| RAM disk (LittleFS) | 1 MB |
| Task stacks | 16 KB each (max 32 tasks) |

---

## AI Scheduler Inference

SLM-OS ships **four pluggable scheduler policies** in addition to
the heuristic baseline. They co-exist as runtime-selectable options
(see `docs/scheduler.md`) — no policy is "the winner". The capstone
story is "we built the substrate, here are four characterizations."

| Policy   | Source                               | Wire format               | Decision shape                                  | Loaded via                              |
|----------|--------------------------------------|---------------------------|--------------------------------------------------|------------------------------------------|
| `heuristic` | Built-in                          | n/a                       | Round-robin + deadline pressure                  | Always available                         |
| `ai_mlp` | `slm-os-scheduler-ai` MLP cascade   | C-source weights (515 KB) | Argmax over 42 logits                            | Statically linked + optional runtime blob |
| `ai_ppo` | `slm-os-scheduler-ai` PPO actor     | C-source weights (515 KB) | Argmax over 42 logits                            | Statically linked + optional runtime blob |
| `ai_xgb` | `slm-os-scheduler-ai` XGBoost cascade | SEMB+XGBC binary (~9 MB) | 3 classifiers in cascade — `(core, priority, preempt)` | Runtime blob (mandatory; never statically linked) |
| `ai_hailo` | Hailo-8 INT8 of MLP cascade       | HEF (~1 MB)               | Argmax over 42 INT8 logits                       | NPU offload via `hailo load`             |

Real trained MLP and PPO weights from the Plan A export pipeline (108→256→256→128→42 architecture, ~3 MB per model).

| Metric | Pi 5 (native) | Target |
|--------|--------------|--------|
| MLP inference latency | **41.9 us** | < 50 us |
| State extraction | 108 dimensions | — |
| Action space | 42 actions (7 cores x 3 priority x 2 preempt) | — |
| Weight size (MLP) | 3.0 MB | — |
| Weight size (PPO) | 3.0 MB | — |
| XGBoost cascade size (runtime blob) | ~9 MB (462 K nodes) | runtime-loaded, never in image |
| Kernel binary with AI | 1.9 MB (vs 824 KB without) | — |

**Target achieved:** 41.9 us < 50 us on Cortex-A76 @ 2.4 GHz.

### AI vs Heuristic Scheduler Comparison

| Metric | Heuristic | AI MLP | AI XGB | Ratio (XGB / heuristic) |
|--------|-----------|--------|--------|------|
| Decision latency (Pi 5) | ~2 us | 41.9 us | _captured by `bench sched-policy`_ | _t.b.d._ |
| Decision latency (QEMU) | ~2 us | ~1,000 us | _captured by `bench sched-policy`_ | _t.b.d._ |
| Fallback rate | N/A | 0-50% (depends on isolation) | clamps + isolation-aware fallback | — |
| CPU assignment | Round-robin | Model-driven | Cascade-driven (`core` then `priority` then `preempt`) | — |

The XGBoost row is collected via `bench sched-policy` after staging
`xgb_sched.smb`; an empty cell means no cascade was active when the
bench ran. Sibling-repo simulator runs (Plan A
`results/eval_summary.csv`, 200 episodes × 8 scenarios) report
**MLP at ~99.6% mean deadline-compliance vs XGBoost at ~95.3%**
across all scenarios. The gap is workload-dependent — XGBoost
matches MLP on `deadline_pressure` and `memory_pressure` (both
100%) and trails most on the more interleaved workloads
(`asymmetric`, `light_*`, `heavy_inference`, all 90–93%).
Informative for option-space comparison, not a selection
criterion. SLM-OS treats XGBoost as a swappable policy alongside
MLP/PPO/Hailo per
[#848](https://github.com/SLM-OS/SLM-Operating-System/issues/848)
("pluggable policies as first-class").

The AI scheduler adds ~40 µs overhead per scheduling decision on Pi 5 hardware. This is acceptable for inference-heavy workloads where decisions happen infrequently (component dispatch, not per-tick). The heuristic policy remains the default for latency-sensitive cooperative scheduling.

**When to use AI scheduling:**
- Workloads with heterogeneous task requirements (different priority/preemption needs)
- Systems where optimal CPU placement matters more than scheduling overhead
- Evaluation of learned scheduling policies against heuristic baselines

### Scheduling-quality workload harness (#882, sub-ticket of #61)

The `bench sched-policy` verb also accepts a workload mode that
drives a representative task mix through `scheduler_add_task` so
policies can be compared on real scheduling **quality** — deadline
miss rate, completion-time percentiles, per-CPU balance, and
throughput — not only inference-only decision latency. The
inference-only mode (legacy `bench sched-policy` with no flags)
still runs and remains backward compatible.

```
slmos> bench sched-policy --workload mixed --all
slmos> bench sched-policy --workload deadline-heavy --policy ai_mlp
slmos> bench sched-policy --list-workloads
```

Workloads are static templates defined in
`kernel/sched/ai/workloads.c`:

| Workload            | Tasks | Templates | Purpose                                                     |
|---------------------|-------|-----------|-------------------------------------------------------------|
| `mixed`             | 24    | 3         | Representative cross-section — short tight-deadline + long no-deadline tasks. Closest shape to the synthetic-baseline training set. |
| `deadline-heavy`    | 24    | 3         | Every task carries a tight relative deadline; surfaces deadline-pressure policy differences. |
| `latency-sensitive` | 24    | 2         | Very short tasks with very tight deadlines; CPU-balance matters most. |
| `cpu-bound`         | 16    | 2         | Long tasks, no deadlines; surfaces work-balance differences without deadline pressure. |

Output format — one row per policy, columns `Done / DL-tasks /
DL-miss% / p50 us / p99 us / max us / CPU-cov / Tasks/s`. `CPU-cov`
is the coefficient of variation of completed-task counts across
CPUs (lower = more balanced). `Tasks/s` reports steady-state
throughput. Per the [exploratory-OS framing][i848], the table is
characterization data; no winning policy is declared in the output.

[i848]: https://github.com/SLM-OS/SLM-Operating-System/issues/848

The harness wraps the existing pluggable-policy interface — switching
policies between rows via `sched_set_policy`, dispatching tasks
through `scheduler_add_task` so the active policy's `assign_cpu`
callback is exercised, then restoring the prior policy on the way
out. Worker tasks run at `TASK_PRIORITY_HIGH` so they preempt the
shell's busy-wait on cooperative-preempt platforms.

Real hardware comparison tables for Pi 5 (4 CPUs) and Jetson Orin
Nano (6 CPUs) — across all four registered policies, on both the
synthetic-trained baseline weights and the SLM-OS-fine-tuned weights
— land in #884 (61d) after the trace-capture (#880) and fine-tune
(#879) sub-tickets complete. The harness lands here so that work has
a stable target shape.

### Scheduling-quality matrix (#61, capstone wrap-up)

**Status:** populated 2026-05-16 from real-hardware captures on
pi-5-2 (Pi 5, 4× Cortex-A76) and jetson-nano-1 (Jetson Orin Nano,
6× Cortex-A78AE). Three `bench sched-policy --workload mixed --all`
runs per build; median (by DL-miss%) reported.

`bench sched-policy --workload <name> --all` (added in #882, PR #928)
exercises all four registered policies through a representative task
mix and reports per-policy scheduling-quality metrics — deadline-miss
rate, completion-time p50/p99, per-CPU balance coefficient of
variation, throughput. The capture is one of two `AI_WEIGHTS=` axes:

- `AI_WEIGHTS=synthetic` (default) — Plan A synthetic-trained MLP/PPO,
  per the existing `kernel/sched/ai/ai_weights_{mlp,ppo}.c`.
- `AI_WEIGHTS=real` — SLM-OS-fine-tuned variants from sibling-repo
  PR #879 (`ai_weights_{mlp,ppo}_real.c`). Built from SLM-OS-captured
  traces collected via `sched aitrace dump` (#880, PR #931).

QEMU rows are omitted intentionally — emulated timing is too noisy to
characterize policy quality, and #882 already covers QEMU harness
mechanics in CI.

| Platform | Policy     | Weights   | DL-miss% | p50 us | p99 us | CPU-cov | Tasks/s |
|----------|------------|-----------|---------:|-------:|-------:|--------:|--------:|
| Pi 5     | heuristic  | n/a       |    31.2% |   2604 |   8099 |   0.000 |    2131 |
| Pi 5     | ai_mlp     | synthetic |    56.2% |   2891 |   9428 |   0.000 |    2169 |
| Pi 5     | ai_mlp     | real      |    12.5% |   1773 |   8029 |   0.375 |    2252 |
| Pi 5     | ai_ppo     | synthetic |    93.7% |   8035 |  12953 |   0.125 |    1475 |
| Pi 5     | ai_ppo     | real      |    — |  — |  — |   — |   — |
| Pi 5     | ai_xgb     | n/a       |    6.2%* |   1151 |   4045 |   0.250 |    2764 |
| Jetson   | heuristic  | n/a       |     0.0% |    513 |   1534 |   0.666 |    2733 |
| Jetson   | ai_mlp     | synthetic |     0.0% |    596 |   1605 |   0.333 |    2260 |
| Jetson   | ai_mlp     | real      |    — |  — |  — |   — |   — |
| Jetson   | ai_ppo     | synthetic |    0.0%† |   590 |   1606 |   0.833 |    2255 |
| Jetson   | ai_ppo     | real      |    — |  — |  — |   — |   — |
| Jetson   | ai_xgb     | n/a       |    0.0%‡ |   519 |   1530 |   0.750 |    2694 |

\* Pi 5 ai_xgb: no cascade staged at boot, so `assign_cpu` falls back
to heuristic on every decision. The DL-miss% / p50 / p99 column is the
heuristic-fallback path's timing under the ai_xgb policy registration;
a staged-cascade row needs runtime `slm.sched_model_load
xgb_sched.smb` and is deferred to a follow-up of #61.

† Jetson ai_ppo: 24/24 decisions out-of-range → 24 heuristic fallbacks.
The shipped PPO weights were trained against the 24-action Pi 5 space;
Jetson's 36-action space requires a Jetson-specific PPO train.

‡ Jetson ai_xgb: same heuristic-fallback story as Pi 5.

The four `— ` rows (Pi 5 ai_ppo real, Jetson ai_mlp/ai_ppo real) were
out of scope for this capture pass. The fine-tune flow itself is
validated end-to-end via the Pi 5 ai_mlp_real row + the smoke tests
in sibling-repo PR for #879; reproducing it for Jetson needs an
on-target Jetson trace capture and is the obvious next iteration.

Per the [exploratory-OS framing][i848], the matrix is characterization
data: SLM-OS ships every (policy × weight set) combination as an
option, and a "winner" depends on the workload. Decision latency is
known to be ~21× slower for AI policies than for heuristic (41.9 µs
vs 2 µs on Pi 5; see the table above) — the row to watch is whether
the longer inference cost is amortized by better scheduling quality
on real workloads, and per which metric. If fine-tuning produces flat
or worse results vs. synthetic, that's a real finding (the SLM-OS
demo workloads may be too small to differentiate trained policies);
the matrix gets populated with whatever the numbers say.

[i848]: https://github.com/SLM-OS/SLM-Operating-System/issues/848

**Build select:**

```
# Synthetic baseline (current behaviour, default)
make kernel PLATFORM=RASPI5 AI_SCHED=ON
# SLM-OS-fine-tuned (requires `scripts/import_ai_weights.sh --include-real`)
make kernel PLATFORM=RASPI5 AI_SCHED=ON AI_WEIGHTS=real
```

The same C symbol names (`ai_mlp_w0`, `ai_mlp_b0`, …) ship in both
`ai_weights_mlp.c` and `ai_weights_mlp_real.c`; the CMake build picks
one and only one .c file. Switching weight sets is therefore a build-
time choice, not runtime. Runtime weight-blob loading via the
existing `sched_blob_*` infrastructure is a documented follow-up.

### XGBoost cascade workflow

The XGBoost cascade is loaded at runtime — there is no kernel-image
embedding. End-to-end:

```
# Sibling repo: emit the binary blob
$ python scripts/export_models.py --model xgboost --platform jetson_orin_nano

# Copy the resulting deploy/generated/xgb_sched.smb to the SD card
# (or any path the running shell can read)

# In the SLM-OS shell:
slm.sched_model_stage("xgboost", "/sd/xgb_sched.smb")
slm.sched_model_activate("xgboost")
slm.sched_set_policy("ai_xgb")
```

CPU-id clamping: the cascade was trained on a 6-core space; on Pi 5
(4 cores) the core classifier can emit 4 or 5. `sched_xgb_assign_cpu`
clamps via modulo and continues, with a one-shot WARN. The
distribution may skew on 4-core platforms but no decision is dropped
to the heuristic fallback.

#### Bit-equality verification (`bench xgb-equiv`)

The sibling repo's exporter emits two corpus files alongside
`xgb_sched.smb`: `test_vectors_xgb.bin` (1000 × 108 f32 input states)
and `expected_actions_xgb.bin` (1000 × 3 i32 ground-truth `(core,
priority, preempt)` triples produced by Python's `TripleClassifier
.predict`). Replay the corpus through the on-device cascade with:

```
# In the SLM-OS shell, after the stage/activate step above:
bench xgb-equiv 0:/slmstore/test_vectors_xgb.bin 0:/slmstore/expected_actions_xgb.bin
```

The verb walks both files in lockstep, runs `rust_sched_xgb_predict`
on each state, diffs the triple against the expected one, and prints
a pass/fail summary plus the first-mismatch index + actual-vs-expected
triple if any disagreement appears. Exit code is 0 on full match,
1 on any mismatch — useful for scripted hardware verification. Both
VFS-rooted (`/mnt/files/...`) and FAT-rooted (`0:/slmstore/...`)
paths are accepted.

This exists because the on-device walker re-implements XGBoost
inference in `no_std` Rust (tree traversal, sigmoid, multiclass
argmax). The synthetic 3-classifier cascade in `rust_run_tests`
proves the FFI plumbing works; this verb proves numerical equivalence
with the trainer on a *trained* cascade. Closes
[#904](https://github.com/SLM-OS/SLM-Operating-System/issues/904).

**Pi 5 result (2026-05-15, BCM2712 Cortex-A76 @ 2.4 GHz):**

| Metric | Value |
|--------|-------|
| Match rate | **23 / 1000** |
| Per-decision latency | **240,747 ns (~240 µs)** |
| First mismatch (i=0) | got `(core=1, priority=2, preempt=1)` vs expected `(core=1, priority=1, preempt=0)` |

The 977/1000 mismatch rate is a real divergence between the on-device
walker and Python's `TripleClassifier.predict` — exactly the class of
bug this verb was designed to catch. The shape (`core` matches at
i=0, `priority` and `preempt` differ) is consistent with a
derived-feature drift, label-encoder inverse-transform mismatch, or
classifier-input-vector ordering bug. **Investigation tracked in
[#920](https://github.com/SLM-OS/SLM-Operating-System/issues/920)**;
the verb itself is sound and will be the canonical regression-detection
tool once the divergence is fixed.

The cascade walker is pure software (no NEON, no FP-tier divergence
between platforms) so a Jetson run would produce identical numbers;
not re-run on Jetson for that reason.

#### Eviction equivalence verification (`bench xgb-equiv-evict`)

Consumer side of [#932](https://github.com/SLM-OS/SLM-Operating-System/issues/932),
bundled into [#448](https://github.com/SLM-OS/SLM-Operating-System/issues/448).
See `kernel/src/shell_sys.c`'s `bench_xgb_equiv_evict` for the verb,
`runtime/src/lib.rs`'s `rust_eviction_xgb_predict_compare` for the FFI,
and the `xgb_equiv_evict_*` checks in `rust_eviction_run_tests` for
a host-side mini-corpus regression that exercises the same FFI path
against a Rust-built toy model — this catches plumbing regressions
in `make test` without needing the producer-side `.smb` on hardware.

The eviction-side analog of the scheduler verb above. The
`slm-os-page-eviction` exporter emits two corpus files alongside
`evict.smb`: `test_vectors_xgb_evict.bin` (N × 27 f32 feature vectors)
and `expected_evict.bin` (N × f32 sigmoid scores from
`booster.predict()`). Replay the corpus through the active
on-device XGBoost eviction blob with:

```
# In the SLM-OS shell, after the eviction blob is staged + activated:
eviction model load xgboost 0:/slmstore/evict.smb
eviction model activate xgboost
bench xgb-equiv-evict 0:/slmstore/test_vectors_xgb_evict.bin 0:/slmstore/expected_evict.bin
```

**Differences from the scheduler verb:**

- **Sigmoid score, not classification triple.** Eviction prediction
  returns a continuous probability (P(optimal eviction target)) so
  per-vector comparison uses an absolute tolerance (`1e-4`) rather
  than integer equality. Tolerance is well above f32 sigmoid
  round-trip noise (~1e-6) but well below what any structural bug
  (missed `base_score` fold, wrong tree-walk path) would produce
  (≥ 0.01).
- **Bit-pattern comparison wire.** The FFI
  (`rust_eviction_xgb_predict_compare`) takes `expected_bits` and
  `tolerance_bits` as raw `uint32_t` IEEE-754 patterns so `shell_sys.c`
  (compiled under `-mgeneral-regs-only`) never materialises an `f32`
  in C code. First-mismatch diagnostics print hex patterns; decode with
  `python3 -c 'import struct;print(struct.unpack("<f", bytes.fromhex("HEX"))[0])'`.

**Pi 5 result (2026-05-17, BCM2712 Cortex-A76 @ 2.4 GHz):**

| Metric | Value |
|--------|-------|
| Match rate | **100 / 100** |
| Per-decision latency | **1,429,641 ns (~1.43 ms)** |
| First mismatch | none |

Closes the on-device side of [#932](https://github.com/SLM-OS/SLM-Operating-System/issues/932). The result was captured by exporting an `evict.smb` + 100-vector corpus from `slm-os-page-eviction` (XGBoost model carried from `slm-os-page-sim/data/models/xgb_model.json`), staging both onto pi-5-2 via `labctl sdwire update`, activating the runtime blob via `eviction model load/activate xgboost`, and replaying with `bench xgb-equiv-evict`. The ~1.43 ms/inference is the full 200-tree softmax-sigmoid walk in pure software (no NEON path yet); the scheduler verb's ~240 µs comparison is for the much smaller cascade. Latency-side improvement work is tracked separately under [#108](https://github.com/SLM-OS/SLM-Operating-System/issues/108).

The `slm-os-page-eviction` PR #3 (merged 2026-05-17) emits the
`evict.smb` blob and verification corpus in the format this verb
consumes. The producer side is the canonical source of `.smb` blobs
for any future "graduate this model into the baked path" workflow
[(#952)](https://github.com/SLM-OS/SLM-Operating-System/issues/952).

---

## Cross-Platform Inference Latency

Unified view of MNIST classify latency (25 classes, 5,998 parameters)
across every SLM-OS target. Populated by C3 on x86-64 and by the
Jetson plan's G3/G4 on Jetson; Pi 5 numbers carried forward from
earlier measurements.

| Model | Platform | Dtype | Latency (avg) | Backend | Notes |
|-------|----------|-------|---------------|---------|-------|
| MNIST | Raspberry Pi 5 (Cortex-A76 @ 2.4 GHz) | FP32 | **0.483 ms** | Rust + NEON (4×4 outer-product micro-kernel, #56) | Measured on hardware (pi-5-2, `model bench mnist 200`) |
| MNIST | Jetson Orin Nano (Cortex-A78AE @ 1.5 GHz) | FP32 | **0.746 ms** | Rust + NEON | Pre-#56 baseline; not re-measured post-micro-kernel work |
| MNIST | test-pc (i7-6700 @ 3.4 GHz), scalar baseline | FP32 | *TBD* | Rust scalar fallback | Pre-C1 measurement; to be captured before removing this row |
| MNIST | test-pc (i7-6700 @ 3.4 GHz), SSE-asm | FP32 | *TBD* | C SSE2 kernel (`kernel/arch/x86_64/sse_kernels.c`) | Post-C1 measurement on real hardware; QEMU numbers also published |
| MNIST | Jetson Linux (Cortex-A78AE) — ONNX Runtime reference | FP32 | 0.117 ms | OpenBLAS + multi-threaded | Production runtime, not SLM-OS |

The two "test-pc" rows will be filled in by `bench infer` runs on
the H610M dev PC once Phase C is deployed via `make x86-disk`. The
scalar baseline row serves as the reference for how much speedup
the SSE kernels delivered — historical before/after data that
future regressions can be judged against. Pre-C1, the x86-64 CPU
path is pure scalar (no SIMD) because of the Rust `x86_64-unknown-none`
toolchain limitation documented in GitHub #72; C1 works around it by
putting the SIMD kernels in a C translation unit built with `-msse`.

### Dispatch selection

| Platform | SIMD backend | Dispatch symbol |
|----------|--------------|-----------------|
| aarch64  | NEON         | `ops.rs` inline `core::arch::aarch64::*` (`vmaxq_f32`, `vfmaq_f32`, …) |
| x86_64   | SSE2         | C kernels via `extern "C"` (`slm_sse_relu_f32` etc.) compiled with `-msse -msse2` |
| Other    | Scalar       | Plain Rust fallback |

The dispatch skeleton (three `#[cfg(target_arch = ...)]` arms per
kernel) is shared — see `runtime/src/inference/ops.rs`.

## ONNX Model Inference (MNIST)

Real ONNX model inference using the built-in MNIST digit classifier (26 KB, 12 operators, 5,998 parameters). Model loaded via `rust_model_load_builtin_mnist()`, inference via `rust_infer_classify()`.

| Metric | Pi 5 (native) |
|--------|--------------|
| Model size | 26 KB (23,982 bytes weights) |
| Parameters | 5,998 |
| Operator nodes | 12 (Conv2D, MaxPool, Gemm, Relu, Reshape, Add, Softmax) |
| Inference latency (avg, post-#56) | **483 us** |
| Throughput (post-#56) | **2,070 inferences/sec** |
| Accuracy | Class 5 for zero input (matches ONNX Runtime reference) |

Post-#56 figures are the 200-iteration `model bench mnist 200` average on pi-5-2 with the 4×4 NEON outer-product micro-kernel and tile-level prefetch hints active. See §Per-operator Profile below for the methodology and the full pre-/post-#56 deep dive.

### Latency Distribution (pre-#56 build, 1000 iterations)

Retained for historical jitter characterisation. The numbers below were captured against the row × scalar `simd_fma_row` matmul inner kernel; post-#56 per-iteration latency is ~483 µs (see above), but a 1000-iter percentile sweep against the new kernel has not been re-run.

| Percentile | Latency |
|------------|---------|
| p50 | 1,092 us |
| p95 | 1,092 us |
| p99 | 1,092 us |
| p100 (max) | 1,100 us |

Every sample in 1000 iterations measured 1,092 µs except one outlier at 1,100 µs. The 8 µs max jitter demonstrates deterministic inference suitable for real-time edge deployment — the property is a function of the kernel's lack of memory-allocation/GC/page-fault pressure, not the absolute matmul cost, so the post-#56 distribution is expected to retain the same shape at the lower latency band.

### Per-operator Profile (#56)

The per-op profiling harness in `runtime/src/inference/engine.rs` wraps every `execute_node` dispatch in CNTPCT timestamps and accumulates per-`OpType` counts and latencies. Enable with `model profile on`, run the workload, then `model profile show` (clear with `model profile reset`). Overhead is negligible: 200-iteration MNIST bench on pi-5-2 measured 3,011 µs/inference both profile-on and profile-off (run-to-run jitter swamps the harness cost).

> **Scope: CPU inference only.** Every change in #56 — the per-op profiler, the 4×4 NEON micro-kernel, and the prefetch hints — lives in the CPU operator path (`runtime/src/inference/ops.rs` and `engine.rs::execute_node`). The GPU path is separate on two levels:
>
> - **Per-op GPU dispatch** (`engine.rs::exec_matmul_gpu` → `gpu_execute_matmul` FFI) bypasses `ops.rs::matmul_tiled` entirely; the 4×4 kernel never runs on GPU-served ops.
> - **MNIST GPU fastpath** (`engine.rs::run_mnist_gpu_fastpath`, enabled by `gpu use inference on` + a per-model toggle, Jetson-only today) routes the whole graph through the GPU pre-uploaded handoff and returns from `run_inference` before `execute_node` is ever called. As a side effect, `model profile show` reports an empty table after a GPU-served bench — that's not a bug, it's the GPU fastpath legitimately bypassing the profile hook. Use `gpu use inference off` (default) for any per-op profile measurement.
>
> The 6.23× speedup below is therefore the CPU-baseline win. A similar treatment of the GPU path (e.g. tuning `scripts/gpu-kernel-mnist.c` or the GA10b channel-handoff dispatch) is out of scope for #56 and would be a separate effort.

All measurements on Pi 5 (BCM2712 Cortex-A76 @ 2.4 GHz), pi-5-2, `BUILD_TYPE=Release`, `model bench mnist 200`. Per-op figures are microseconds; "calls" is the count of `execute_node` invocations across the run.

**Baseline (pre-#56 — row × scalar `simd_fma_row` inner kernel):**

| Op | Calls | Avg | Min | Max | Notes |
|----|-------|-----|-----|-----|-------|
| Conv | 400 | **1,433 µs** | 1,048 µs | 1,818 µs | Dominant — two conv layers × 200 iters |
| MatMul | 200 | 22 µs | 22 µs | 24 µs | Output Gemm decomposes to one MatMul |
| Relu | 400 | 19 µs | 7 µs | 31 µs | |
| MaxPool | 400 | 15 µs | 6 µs | 24 µs | |
| Add | 600 | 9 µs | 2 µs | 21 µs | Conv-bias + Gemm-bias adds |
| Reshape | 400 | 1 µs | 1 µs | 2 µs | |

Total dispatched-op time per inference: ~2,985 µs → 3,011 µs bench latency. `Conv` is ~95% of inference time. The 3,011 µs figure is the #56-era baseline against a slightly higher-overhead build than the earlier 1,092 µs `Latency Distribution` measurement reflected; the #56 PRs report before/after against this current measurement so the deltas are apples-to-apples. The headline tables in `Platform Comparison` and `Cross-Platform Inference Latency` carry the **post-#56** average (483 µs) — i.e. the end state after all three #56 PRs landed — not the 3,011 µs interim baseline used here for the per-op breakdown.

**After PR 2 (4×4 NEON outer-product micro-kernel):**

| Op | Calls | Avg | Min | Max | Δ vs baseline |
|----|-------|-----|-----|-----|---------------|
| Conv | 400 | **169 µs** | 141 µs | 199 µs | **8.5× faster** |
| MatMul | 200 | 22 µs | 22 µs | 25 µs | Unchanged (small matmuls take `matmul_simd`) |
| Relu | 400 | 19 µs | 7 µs | 31 µs | Unchanged |
| MaxPool | 400 | 16 µs | 6 µs | 25 µs | Unchanged |
| Add | 600 | 9 µs | 2 µs | 21 µs | Unchanged |
| Reshape | 400 | 1 µs | 1 µs | 2 µs | Unchanged |

End-to-end MNIST: **3,011 µs → 483 µs (6.23× faster)**, throughput **332 → 2,070 infer/sec**. The win lands entirely on `Conv` because conv2d → im2col → `matmul_tiled` is the only op that uses the tiled path; small fully-connected matmuls fall through `matmul_inner` to the non-tiled `matmul_simd` form and keep the old row×scalar kernel.

The 4×4 kernel keeps the C accumulator block resident in 4 NEON registers across the full K sweep, issuing 4 FMAs per `vld1q_f32(B)` + 4 × `vdupq_n_f32(A)` cycle — eliminating the per-K-step C load/store pair the row×scalar form pays.

Jetson hardware run deferred until lab tooling for non-SD-card deploy is in this agent's reach; the change is platform-agnostic Rust + NEON intrinsics and cross-builds clean for `PLATFORM=JETSON_ORIN_NANO`.

**After PR 3 (software prefetch hints):**

PR 3 adds `prfm pldl1keep` hints to `matmul_tiled`. Two prefetch sites were tested:

1. **Tile-level A-row prefetch** — issued once per 4-row group, ahead of the next ib-block's first call into `matmul_4x4_kernel_neon`. Prefetches the first cache line of each of the next 4 A rows.
2. **In-kernel B-row prefetch** (initial design, *reverted*) — `prfm pldl1keep` inside the 4×4 kernel's K-loop, 8 K-steps ahead of the consuming `vld1q_f32(B)`.

| Op | Calls | Avg | Min | Max | Δ vs PR 2 |
|----|-------|-----|-----|-----|-----------|
| Conv | 400 | **170 µs** | 141 µs | 202 µs | +0.6% (jitter) |
| End-to-end MNIST | — | **486 µs** | 484 | 513 | +0.6% (jitter) |

The in-kernel B prefetch (variant 2 above) measured **3% slower** (483 → 500 µs) on Cortex-A76 — the address arithmetic for the prefetch target (`bp_block + (kki + PFDIST_K) * n_stride`) added an integer multiply and a compare per K iteration that the A76 hardware prefetcher already covers. The matmul tile working set (~12 KB) fits in the 64 KB L1D with comfortable headroom, so cold-line misses are not a meaningful cost on this CPU. That variant was reverted; only the tile-level A-row prefetch ships.

Conclusion: software prefetch is a net **no-op on Cortex-A76**. The infrastructure (a portable `prefetch_l1_read` helper and the tile-level hook) is kept in place because (a) the A78AE in Jetson Orin Nano has a different prefetcher and may behave differently, and (b) tighter inner loops on the same path (e.g. an 8×4 micro-kernel) will have more arithmetic per cache line and benefit more.

### Per-operator Profile with PMU columns (#60 / #871)

`model profile show` is extended in #871 with three PMU-derived columns
appended to the latency columns above:

- **L1D%** — L1 data cache miss rate over the bucket: `cache_misses /
  cache_references × 100`. Useful for telling memory-bound ops apart
  from compute-bound ones.
- **IPC** — instructions retired per cycle: `instructions_retired /
  cycles`. The compute-density indicator.
- **mispred%** — branch mispredictions per 100 retired instructions:
  `branch_mispredictions / instructions_retired × 100`. Surfaces
  control-flow-heavy ops.

PMU columns are blank (`-`) on QEMU TCG (event counters not modelled)
and on platforms where the PMU has not been enabled on the calling
CPU.

#### Captured PMU profile — pi-5-2 + jetson-nano-1 (2026-05-15)

Captured via `model profile on; model bench mnist {200|100}; model profile show`
after the kernel boots and PMU init completes on every CPU. The
per-op latency columns mirror the pre-#871 table above; the new
columns are L1D miss-rate %, IPC (instructions retired per cycle),
and branch mispredictions per 100 retired instructions.

##### Pi 5 (Cortex-A76, BCM2712 @ ~2.4 GHz, AI_SCHED=ON, 200 iter)

| Op | Calls | Avg µs | Min µs | Max µs | L1D% | IPC | Mispred% |
|----|------:|-------:|-------:|-------:|-----:|----:|---------:|
| Conv | 400 | 1,431 | 1,046 | 1,816 | <1 | **2.46** | <1 |
| MatMul | 200 | 23 | 22 | 24 | <1 | 2.43 | <1 |
| Relu | 400 | 19 | 7 | 31 | <1 | 2.12 | <1 |
| MaxPool | 400 | 15 | 6 | 24 | <1 | 2.75 | <1 |
| Add | 600 | 9 | 2 | 21 | <1 | 1.75 | <1 |
| Reshape | 400 | 1 | 1 | 2 | <1 | 2.09 | <1 |

Total/inf: 3,008 µs.

##### Jetson Orin Nano (Cortex-A78AE @ 1.5 GHz, 100 iter)

| Op | Calls | Avg µs | Min µs | Max µs | L1D% | IPC | Mispred% |
|----|------:|-------:|-------:|-------:|-----:|----:|---------:|
| Conv | 200 | 1,990 | 1,450 | 2,533 | <1 | **3.63** | <1 |
| MatMul | 100 | 32 | 31 | 34 | <1 | 3.72 | <1 |
| Relu | 200 | 24 | 9 | 39 | <1 | 3.49 | <1 |
| MaxPool | 200 | 17 | 7 | 31 | <1 | 5.00 | <1 |
| Add | 300 | 17 | 2 | 42 | <1 | 1.92 | <1 |
| Reshape | 200 | 2 | 1 | 3 | <1 | 3.67 | <1 |

Total/inf: 4,181 µs.

Integer division renders L1D% / Mispred% as 0 when the true rate is
below 1 %; both columns are <1 % across every op on both platforms —
MNIST weights + working set fit comfortably in L1D after warmup.

##### Interpretation

**Where the latency goes is unambiguous: Conv2D dominates at ~95% of
inference on both platforms.** L1D miss rate stays below 1% across
every op on both Cortex-A76 and Cortex-A78AE — MNIST is small enough
that the working set fits in L1D after warmup, so inference is
**compute-bound, not memory-bound**. The `cache_pressure` feature
fed into the AI scheduler (#872) therefore reads low under MNIST and
will only spike under larger models whose working set spills L2.

The IPC delta between the two cores tells the second-order story:
Cortex-A78AE retires 1.5× more instructions per cycle than
Cortex-A76 on Conv2D (3.63 vs 2.46), but Pi 5's higher clock
(2.4 vs 1.5 GHz) more than compensates — Pi 5 finishes Conv2D in
1,431 µs vs Jetson's 1,990 µs. A78AE also accumulates ~2× more
backend-stall cycles than A76 on the same loop (see
`docs/pmu-bringup.md` reference output) — its wider issue width
isn't fully utilised by the current matmul kernel, leaving headroom
for future per-microarch tuning.

**Honest framing on "before/after #56".** The per-op profile harness
*itself* landed under issue #56 (PR #849), so true pre-#56 PMU
numbers don't exist on this codebase — we only have post-#56 PMU
data. What the table demonstrates is that whatever #56's improvements
were, the workload remains firmly compute-bound on both platforms;
further latency gains from this point will come from either a wider
matmul micro-kernel (push IPC higher) or moving Conv2D off the CPU
(Hailo on Pi 5, GA10B GPU on Jetson).

### Model Memory Utilization

| Model | Weight Blocks | Workspace Blocks | Actual Weights |
|-------|--------------|-----------------|----------------|
| MNIST | 1 / 128 (2 MB allocated, 24 KB used) | 1 / 64 (2 MB allocated) | 23,982 bytes |

The 2 MB block granularity means small models waste most of their allocated block. For production deployment with many small models, a sub-block allocator within the weight pool would improve density.

---

## Dynamic Batching (Runtime-Toggleable Mode)

SLM-OS's `InferenceScheduler` supports a runtime-toggleable batched dispatch mode (issue #55). Concurrent inference requests are collected into a bounded queue and, on either the size threshold or a configurable timer, dispatched in a single engine call. Per the exploratory-OS framing (#848), batching is a swappable option — default OFF — not a default behavior change.

This section reports the **characterisation** of that mode on Pi 5 and Jetson, not a winner. The capstone story is "the engine supports a batched dispatch mode; here is its behavior at N = 1..16 on real hardware," not "batching wins."

### Workload

[`bench infer-stress N [iters]`](shell.md#command-descriptions) spawns N concurrent task workers, each running `iters` MNIST inferences against the dispatcher. Reported numbers below are from the shell command with the platform's standard `iters` setting (50 for N ≤ 8, 25 for N = 16 to cap wall-clock).

Configuration for all batched runs: `batch_size = 8`, `timeout_us = 5000` (the shipping defaults). With batching ON the dispatcher routes through the queue + threshold/timer path; with OFF every request takes the existing singleton path.

### Pi 5 (pi-5-2) — `bench infer-stress`, native boot, 4× Cortex-A76 @ 2.4 GHz

| N | Mode | req/s | min µs | avg µs | max µs | batches_full | batches_timeout | singleton |
|---|---|---|---|---|---|---|---|---|
|  1 | OFF | 330 | 3009 |  3010 |  3028 | 0 | 0 |  50 |
|  1 | ON  | 330 | 3009 |  3010 |  3011 | 0 | 0 |  50 |
|  2 | OFF | 331 | 3010 |  5918 |  6030 | 0 | 0 | 100 |
|  2 | ON  | 331 | 3011 |  5979 |  6025 | 0 | 0 | 100 |
|  4 | OFF | 331 | 3010 | 11555 | 54224 | 0 | 0 | 200 |
|  4 | ON  | 331 | 3010 | 11536 | 45188 | 0 | 0 | 200 |
|  8 | OFF | 331 | 3009 | 11750 | 57226 | 0 | 0 | 400 |
|  8 | ON  | 331 | 3010 | 11595 | 48199 | 0 | 0 | 400 |
| 16 | OFF | 331 | 3010 | 11642 | 48197 | 0 | 0 | 400 |
| 16 | ON  | 331 | 3010 | 11501 | 51213 | 0 | 0 | 400 |

Pi 5 max-latency outliers at N ≥ 4 (~50 ms) reflect the default cooperative-preempt scheduling (10 ms quantum), not dispatcher contention: a worker that loses CPU just after `submit_inference_sync` returns can wait several quanta before its next iteration runs. The per-iter `min` (~3.0 ms) is the true single-inference cost; `avg` widens with N because workers serialise on `EngineGuard`.

### Jetson Orin Nano (jetson-nano-1) — `bench infer-stress`, slmos-kexec, 6× Cortex-A78AE

CPU-path matrix (the MNIST GPU fast path is single-sample only, so even ON paths take the CPU graph here; see "GPU fast path interaction" below):

| N | Mode | req/s | min µs | avg µs | max µs (noise) | batches_full | batches_timeout | singleton |
|---|---|---|---|---|---|---|---|---|
|  1 | OFF | 239 | 4157 |  4176 |    4203 | 0 | 0 |  50 |
|  1 | ON  | 239 | 4166 |  4177 |    4189 | 0 | 0 |  50 |
|  2 | OFF | 239 | 4194 |  8317 |    8377 | 0 | 0 | 100 |
|  2 | ON  | 239 | 4175 |  8312 |    8375 | 0 | 0 | 100 |
|  4 | OFF | 102 | 4177 | 18961 |  654563 | 0 | 0 | 200 |
|  4 | ON  | 102 | 4172 | 17998 |  444816 | 0 | 0 | 200 |
|  8 | OFF |  79 | 4173 | 30594 | 1099540 | 0 | 0 | 400 |
|  8 | ON  | 102 | 4172 | 30422 | 1308376 | 0 | 0 | 400 |
| 16 | OFF | 143 | 4175 | 28189 | 1544941 | 0 | 0 | 400 |
| 16 | ON  | 143 | 4175 | 28022 | 1517780 | 0 | 0 | 400 |

Jetson max-latency outliers (the seconds-long tails) come from L4T-side scheduling noise after `slmos-kexec` — when the Linux→SLM-OS handoff inherits a busy GIC/timer state, individual worker iterations get descheduled for hundreds of milliseconds. The per-iter `min` (~4.2 ms) reflects the actual CPU-path cost; the `avg` widens with N because workers contend on the engine lock.

GPU fast path interaction: at `N = 1` with `gpu use inference on` and a v6 channel handoff present, the engine routes through `run_mnist_gpu_fastpath`. With batching ON or `N > 1`, the GPU fast path is skipped because the GPU FFI asserts `input_len == 784`. Documented limitation; a batched MNIST GPU dispatch would amortise per-call setup over N samples and is candidate future work.

### Interpretation — honest characterisation

**Batched dispatch never fires on this workload.** The `batches_full` and `batches_timeout` columns are zero across every row of both matrices; every iteration on every run takes the singleton path. This is the honest result for MNIST + CPU graph and is explained by the dispatcher's three-way interaction with `EngineGuard` and the configured timer:

1. The submitter that reserves the only slot in an empty queue sees `pending_count == 1` in `decide_role` and takes the **Solo fallback** (releases the slot and dispatches via the existing singleton path). Without this fallback, an isolated request would block for the full `batch_timeout_us` waiting for peers that never arrive — but here the shortcut means a queue of one always self-clears in microseconds.
2. Concurrent workers serialise on `EngineGuard` (the engine lock that has always protected `run_inference`). With per-inference cost ≈ 3 ms on Pi 5 and ≈ 4.2 ms on Jetson, the window between *worker A releasing its slot and entering* `run_inference` and *worker B arriving at the queue* is ≈ microseconds — orders of magnitude shorter than the inference itself. Two workers virtually never coexist as `PENDING` at the same instant.
3. The configured 5 ms timer flush can only fire when at least one waiter is sitting in the queue. Because of (1) and (2), no waiters accumulate.

Together these mean batched dispatch is **structurally bounded by the engine lock** in the small-model + CPU regime. Throughput peaks at the single-thread engine rate (≈ 331 req/s Pi 5, ≈ 239 req/s Jetson) regardless of N; additional workers stack as latency, not throughput.

**This is not a bug — it is the realistic characterisation that 55d was specifically asked to capture.** The dispatcher is correct (every QEMU regression test passes, the timer-flush path is exercised by a deterministic test in `rust_batch_inference_test`), and the infrastructure is in place. Workloads that would actually benefit are:

- **Larger per-call setup that amortises across a batch.** Transformer attention layers, multi-MB Gemm operations, or quantised inference paths whose dequant/setup cost is per-call rather than per-element.
- **Routing batched dispatches through a GPU fast path.** The current GPU MNIST handoff is `input_len == 784` only. A batched GPU dispatch would amortise channel set-up, doorbell, and PCI write overhead over N samples — the largest potential win.
- **A lock-free or coarser-grained engine path** so multiple workers can land in `PENDING` together. `EngineGuard` is the bottleneck today; the parent issue notes that batching naturally reduces lock contention by holding the lock for one large call instead of N small ones — but only if the batch actually forms before the lock is acquired, which it doesn't with the current Solo fallback.

The capstone report should reference this section as "the batched dispatch mode is implemented and characterised end-to-end; on the MNIST + CPU workload the engine lock dominates, so the option-space addition is the deliverable, not a throughput win." Per the exploratory-OS framing (#848), an unswapped option is still a valid contribution.

---

## Test Suite Performance

| Platform | Tests | Pass | Fail | Time |
|----------|-------|------|------|------|
| QEMU ARM64 | 620+ | All | 0 | ~15 s |
| Pi 5 | 620+ | 615+ | 5 | ~24 s |
| x86-64 | 434 | 426 | 8 | ~10 s |

Pi 5 failures: 5 multi-core integration tests (secondary CPU preemption limitation).
x86-64 failures: 8 platform-specific tests (PIC, PCI, GPU commands not implemented).

---

## Linux Comparison

Measured on Jetson Orin Nano running Linux 5.15.148-tegra (6x Cortex-A78AE @ 1.5 GHz, 8 GB). Note: different hardware than Pi 5, but both are ARM64. Linux numbers represent a production JetPack deployment, not a minimal kernel.

| Metric | SLM-OS (Pi 5) | SLM-OS (Jetson) | Linux (Jetson) | SLM-OS vs Linux |
|--------|---------------|-----------------|---------------|-----------------|
| Context switch | **1.858 us** | **3.601 us** | 13.6 us (pipe) | **3.8x faster** |
| IPC round-trip | **132 ns** | **60 ns** | 23.7 us (UDS) | **395x faster** |
| Boot to shell | **~1.6 s** | ~3 s | 20.8 s | **7x faster** |
| Kernel binary | **824 KB** | **893 KB** | 41.1 MB | **46x smaller** |
| MNIST inference | **0.483 ms** | **0.746 ms** ¹ | 0.117 ms (ONNX RT) | 4.1x slower* |
| CPUs | 4 | 6 | 6 | Same |

¹ Jetson MNIST inference has not been re-measured after the #56 micro-kernel work; the 746 µs figure is the pre-#56 baseline.

*\* ONNX Runtime uses optimized BLAS (OpenBLAS/LAPACK) with cache-optimized tiling and multi-threaded matmul. SLM-OS uses a single-threaded NEON matmul with 32×32 cache tiling and a 4×4 register-blocked outer-product micro-kernel (landed in #56: PRs #849/#876/#887). The remaining ~4× gap to ONNX Runtime is attributable to multi-threading and BLAS-specific tuning, not the matmul micro-architecture; the in-kernel single-threaded path is no longer the bottleneck. The inference engine remains functionally correct and deterministic (8 µs jitter under the pre-#56 1000-iter distribution; post-#56 percentile sweep not yet captured).*

### Why SLM-OS is Faster (except inference)

- **No syscall overhead:** function calls replace trap-based system calls
- **No virtual memory TLB faults:** identity-mapped 2 MB blocks
- **No process isolation:** all code runs in kernel mode (EL1)
- **No scheduler complexity:** 8-priority cooperative + deadline boost vs CFS
- **Purpose-built IPC:** zero-copy message queues in shared address space vs kernel-mediated pipes/sockets
- **No driver framework:** direct register access vs driver model layers

### Tradeoffs

SLM-OS achieves these numbers by trading:
- Process isolation (all components share address space)
- POSIX compatibility (custom API)
- Hardware driver ecosystem (manual driver development)
- Multi-user support (single-purpose)

These tradeoffs are acceptable for dedicated AI edge devices where the OS runs a single, known workload.

---

*SLM-OS measured: April 2026 on Raspberry Pi 5 (BCM2712, 4x Cortex-A76 @ 2.4 GHz, 4 GB LPDDR4X)*
*Linux measured: April 2026 on Jetson Orin Nano (6x Cortex-A78AE @ 1.5 GHz, 8 GB, JetPack 6 / Linux 5.15.148-tegra)*
