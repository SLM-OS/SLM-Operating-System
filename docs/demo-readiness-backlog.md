# Demo Readiness Backlog

Curated backlog of existing and new tickets relevant to a live demo of
SLM-OS's five core capabilities: SMP, Preemptive Multitasking,
AI-based Task Scheduling, AI-based Page Eviction, and Model Inference.

**Last updated:** 15 April 2026

Excludes work already being driven by parallel agents (GPU inference on
x86-64 and Jetson, Preemptive Multitasking on Jetson+Pi 5, Pi 5 SMP
cross-CPU dispatch, Lua bindings audit, networking expansion).

---

## Must-Fix Blockers

These must be resolved before any demo work begins.

| # | Title | Priority | Impact |
|---|-------|----------|--------|
| #141 | x86-64 build broken after G3 inference kernels (libm f16 + fat LTO) | P1-high | x86-64 demo impossible until fixed |
| #68 | Wildcard pattern matching: unsafe pointer read past string boundary | — | Potential crash during demo; security issue |
| #69 | Message router: silent topic name truncation at 16 bytes | — | Silent bugs mid-demo are disastrous |
| #79 | CORE-H5: littlefs file_handle pool stale-handle reuse | P2-medium | File corruption mid-demo |

---

## High-Impact Demo Enablers

Fix these next — they directly enable visible, compelling demos.

### New Demo Infrastructure

| # | Title | Priority | What it enables |
|---|-------|----------|-----------------|
| #191 | `top` command for live system dashboard | P2-medium | Real-time view of SMP, scheduling, eviction — the primary visual tool for any live demo |
| #193 | Side-by-side scheduler policy comparison command | P2-medium | Empirical "AI scheduling beats heuristic" comparison in one shot |
| #192 | Scripted auto-demo sequencer | P3-low | Lets presenter talk while OS drives itself through all five features |
| #195 | Scheduler trace buffer and `sched trace` command | P2-medium | ASCII per-CPU timeline — makes SMP dispatch directly observable |
| #194 | Eviction pressure demo command | P3-low | Visible victim-selection narrative instead of opaque pool stats |
| #196 | Latency histograms for `bench` commands | P3-low | Demonstrates predictability — tail latency, not just averages |

### Eviction Visibility & Integration

| # | Title | Priority | What it enables |
|---|-------|----------|-----------------|
| #115 | Per-policy decision/fallback/latency counters | P3-low | Demo can show "AI made 47 decisions, 3 fallbacks, avg 190us" |
| #113 | SlmHeuristicPolicy active-inference feed from scheduler | P2-medium | Connects AI scheduler to AI eviction — proves end-to-end AI thesis |
| #111 | CACHEUS trajectory recording + `eviction trajectory` shell | P3-low | "Evicted model X, re-loaded 200ms later → bad, weights adjusted" |
| #120 | Per-pool eviction policy | P3-low | Demo: weight pool uses CACHEUS, workspace pool uses LRU |
| #119 | Online retraining loop | P3-low | "It learns from mistakes" — prove it live |
| #117 | CACHEUS vs LRU fault-rate comparison (workload replay) | P2-medium | Primary empirical evidence that AI eviction beats classical |

### Model Inference Enhancements

| # | Title | Priority | What it enables |
|---|-------|----------|-----------------|
| #37 | Multi-Model Management (LRU eviction, pinning, registry) | P3-low | Inference demo needs multiple models to make eviction visible |
| #64 | Async model preloading for component startup | P3-low | Faster demo startup — no long load pause |

### Scheduler/SMP Polish

| # | Title | Priority | What it enables |
|---|-------|----------|-----------------|
| #93 | pi_mutex: replace spin-wait with sleep queue | P2-medium | Visible busy-waits look crude; sleep queues show sophistication |
| #94 | sched/smp: replace volatile busy-wait with timer-driven delay | P3-low | `bench smp` currently has visible spin-waits |

---

## Quality-of-Life for Eviction Internals

Unlock richer eviction features and better diagnostics.

| # | Title | Priority |
|---|-------|----------|
| #123 | `mm_touch_block` syscall wrapper for kernel-side callers | P3-low |
| #122 | Extend feature vector with kernel-side signals | P3-low |
| #118 | `set_dirty` caller wire-up once runtime writes to blocks | P3-low |
| #114 | ARC direct `notify_eviction` wiring | P3-low |
| #112 | `eviction_features.rs` runtime feature-name introspection | P3-low |

---

## Deferred: Research-Heavy Items

Would be significant wins but require deeper investigation. Not
included in the current demo push.

| # | Title | Why deferred |
|---|-------|--------------|
| #176 | Investigate uses for TurboQuant in SLM-OS | Hardest of the bunch — investigation stage, unknown scope |
| #180 | Investigate Theory Radar for symbolic eviction/scheduler policies | Investigation stage, unknown scope |
| #183 | Support Gemma 4 E2B Q4_0 on Jetson | Likely depends on #176 (quantization); huge win but significant effort |

---

## Also Deferred from Demo Scope

Not included: benchmarking tickets that only inform but don't change
capability (#172, #108, #109, #110, #60), presentation deliverables
(#86–89), and tech-debt-only cleanup (#101, #100, #74, #50, #52, #62,
#63).

---

## Recommended Execution Order

1. **Unblock.** #141 (x86-64 build). #68 and #69 (silent bugs).
2. **Core observability.** #191 (top) → #195 (sched trace) → #115
   (per-policy counters) → #111 (CACHEUS trajectory).
3. **Integration.** #113 (scheduler→eviction feed) → #193 (policy
   comparison) → #194 (eviction pressure).
4. **Demo experience.** #192 (auto-demo) → #196 (histograms) → #37
   (multi-model) → #64 (async preloading).
5. **Polish & capability.** #117 (CACHEUS vs LRU) → #120 (per-pool
   policy) → #119 (online retraining) → #93/#94 (busy-wait
   replacements).
6. **Eviction internals.** #123, #122, #118, #114, #112 — order by
   dependency.
7. **Cleanup.** #79 (file_handle pool).

---

*Generated: 15 April 2026*
