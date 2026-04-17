# Demo Readiness Backlog

Curated backlog of existing and new tickets relevant to a live demo of
SLM-OS's five core capabilities: SMP, Preemptive Multitasking,
AI-based Task Scheduling, AI-based Page Eviction, and Model Inference.

**Last updated:** 16 April 2026

Excludes work already being driven by parallel agents (GPU inference on
x86-64 and Jetson, Preemptive Multitasking on Jetson+Pi 5, Pi 5 SMP
cross-CPU dispatch, Lua bindings audit, networking expansion).

---

## Completed (Phases 1-4 + Cleanup)

The following tickets have been implemented and merged. Each has
regression tests in the kernel test suite.

### Must-Fix Blockers (all resolved)

| # | Title | Status |
|---|-------|--------|
| ✅ #141 | x86-64 build broken (libm f16 + fat LTO) | Fixed via `mathf.rs` scalar sqrtf/tanhf |
| ✅ #68 | Wildcard pattern matching unsafe pointer read | Bounded via `cstr_len_bounded` + docs |
| ✅ #69 | Message router silent topic truncation at 16 bytes | Reject at subscribe/publish boundary |
| ✅ #79 | littlefs file_handle pool stale-handle reuse | Generation counter in encoded handles |

### Demo Infrastructure (all delivered)

| # | Title | Status |
|---|-------|--------|
| ✅ #191 | `top` command for live system dashboard | Per-CPU util, tasks, memory, eviction. `uart_try_getc` on all 4 UART drivers |
| ✅ #195 | Scheduler trace buffer and `sched trace` command | 4096-event circular buffer, tabular dump + ASCII per-CPU timeline |
| ✅ #115 | Per-policy decision/fallback/latency counters | Registry-level shim, resets on policy swap |
| ✅ #111 | CACHEUS trajectory recording + `eviction trajectory` | 128-entry ring, basis-point FFI, shell subcommand |
| ✅ #113 | SlmHeuristicPolicy active-inference feed | Global atomic table, RAII guard in `rust_infer_classify` |
| ✅ #193 | Side-by-side scheduler policy comparison | `sched compare` runs context-switch bench under all policies |
| ✅ #194 | Eviction pressure demo command | `eviction demo` fills weight pool to saturation |
| ✅ #192 | Scripted auto-demo sequencer | `demo_auto.lua` — 5-section Enter-to-advance |
| ✅ #196 | Latency histograms for bench commands | 12-bucket log histogram on `bench context` with p50/p95/p99 |

---

### Phase 5: Capability & Polish (all resolved)

| # | Title | Status |
|---|-------|--------|
| ✅ #94 | timer-driven busy-wait | `timer_busy_wait_us()` inline; smp.c loops replaced |
| ✅ #120 | Per-pool eviction policy | Registry split; `eviction policy weight/workspace <name>` |
| ✅ #93 | pi_mutex sleep queue | TASK_BLOCKED waiter queue; direct hand-off on unlock |
| ✅ #117 | CACHEUS vs LRU comparison | `bench eviction` — CACHEUS 26% vs LRU 44% fault rate |
| ✅ #119 | Online retraining design | 4-phase architecture doc |

### Phase 6: Eviction Internals (all resolved)

| # | Title | Status |
|---|-------|--------|
| ✅ #112 | Feature-name introspection | FEATURE_NAMES[27] + `eviction features` shell |
| ✅ #114 | ARC notify_eviction | Trait method + allocator caller; ghost lists populate proactively |
| ✅ #123 | mm_touch_block syscall | SYS_TOUCH_BLOCK (7) in syscall table |
| ✅ #118 | set_dirty wire-up | Tests verify feature vector shift; pathway ready |
| ✅ #122 | Extended feature vector | Slots 11 + 17 wired from live data + design doc |

## Remaining

### Model Inference Enhancements

| # | Title | Priority | What it enables |
|---|-------|----------|-----------------|
| #37 | Multi-Model Management | P3-low | Multiple models to make eviction visible |
| #64 | Async model preloading | P3-low | Faster demo startup |

---

## Deferred: Research-Heavy Items

| # | Title | Why deferred |
|---|-------|--------------|
| #176 | Investigate uses for TurboQuant in SLM-OS | Investigation stage, unknown scope |
| #180 | Investigate Theory Radar for symbolic policies | Investigation stage, unknown scope |
| #183 | Support Gemma 4 E2B Q4_0 on Jetson | Likely depends on #176; significant effort |

---

*Updated: 16 April 2026*
