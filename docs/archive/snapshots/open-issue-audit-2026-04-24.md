# Open Issue Audit — 2026-04-24

Review of 101 open GitHub issues. Fixed candidates were verified by cross-referencing commit messages against issue numbers on `main`. Severity uses the P0/P1/P2/P3 label. Effort is estimated as **L** (hours), **M** (1–3 days), **H** (multi-day), **XH** (exploratory/multi-week or hardware RE). Demo column indicates whether the issue would meaningfully improve the interactive capstone demo (Pi 5 + Jetson + live Lua shell + Hailo inference).

---

## Likely Already Fixed (9 issues)

Each has a commit on `main` whose message references the issue number and whose diff matches the stated bug. Recommend closing.

| Issue | Title | Fix Commit |
|-------|-------|------------|
| #338 | hailo backend: run() must reuse load_model's boundary desc lists | `caf5e4a` Phase 6.7 |
| #323 | industrial_demo.lua and multiproc_demo.lua unreachable at runtime | `9d0283a` Demo defect audit |
| #322 | industrial_demo.lua script-level defects | `9d0283a` Demo defect audit |
| #321 | slm.tasks() returns empty after task ID exceeds MAX_TASKS | `9d0283a` Demo defect audit |
| #318 | demo_menu option 9 can't cleanly re-run within 30s | `9d0283a` Demo defect audit |
| #317 | Demo alerts spam '[msg] Topic not found' | `9d0283a` Demo defect audit |
| #313 | slm.cpu_id() reads wrong MPIDR bits on Pi 5 / Jetson | `9d0283a` Demo defect audit |
| #337 | Phase 6 context-switch reviewer suggestions | `71ee67c` Phase 6 close-out |
| #281 | Hailo control-channel RPC — command codes reverse engineering | `a76279a`, `b2607b3`, `57cb849` tier 1–3 |

---

## Bugs (8 open)

| # | Title | Sev | Effort | Demo |
|---|-------|-----|--------|------|
| 305 | pi-5-2: SD card slot unreliable after SDWire use — boot fails on known-good cards | **P1 (blocker)** | M | **Y** — blocks Pi 5 as a demo target |
| 315 | demo_auto.lua C-string mirror in demo_init.c is drift-prone | P2 | L | Y — demo correctness |
| 200 | Scheduler + integration tests flaky under `make test` on ARM64 QEMU | P2 | M | N |
| 216 | Pi 5: secondary CPUs intermittently dormant after boot | P2 | H | Y — SMP visible in demo |
| 237 | x86-64 test suite: 10 pre-existing failures on `make test PLATFORM=X86_64` | P2 | M | N |
| 241 | Lab infra: test-pc Ubuntu SSD drops to initramfs (missing root UUID) | P2 | L | N |
| 316 | xHCI control-transfer actual_length over-reports on short-packet Data Stage | P3 | M | N |
| 247 | Pi 5 RP1 MSIX_CFG engine doesn't fire TLPs on peripheral IRQ assertion | P3 | H | N (workaround exists) |

---

## Enhancements — Active (Capstone or unscheduled, 39 open)

Ordered by severity, then effort (low → high).

| # | Title | Sev | Effort | Demo |
|---|-------|-----|--------|------|
| 347 | Hailo: multi-context HEF dispatch (Phase 8 blocker) | **P1** | H | **Y** — unlocks real HEF models on Pi 5 |
| 309 | Phase 3A: eliminate post-kexec USB re-plug requirement on Jetson | **P1** | M | Y — smoother Jetson demo |
| 185 | SEC2 priv-lock blocks Booter Load under VFIO+FLR on x86-64 | P2 (blocker) | XH | N |
| 328 | ctxsmoke: gate diagnostic IDENTIFY behind a verbose flag | P3 | L | N |
| 332 | hailo_control: replace busy-wait in wait_for_response with yield/MSI fast-path | P2 | L | Y — cleaner inference flow |
| 339 | Hailo translator: multi-network-group HEF support (network_index) | P2 | M | Y — multi-model demo |
| 266 | Jetson: USB networking (CDC-ECM via XHCI host + dongle; XUDC fallback) | P2 | M | Y — remote Jetson demo |
| 202 | Pi 5 networking: BCM GENET driver | P2 | H | Y — on-board Ethernet for demo |
| 246 | Jetson UEFI-direct: Device-nGnRnE DRAM access raises CBB Interface Error | P2 | H | N (kexec path works) |
| 334 | Tegra xHCI: full PHY / padctl re-init at SLM-OS xhci_init | P2 | H | Y — reliable USB on Jetson |
| 354 | Hailo backend: multi-input / multi-output pad arrays | P2 | H | Y — richer model topologies |
| 353 | Hailo VDMA ring state: software cursor for steady-state inference | P2 | H | Y — sustained throughput |
| 356 | Jetson GA10B: QMD compute kernel launch (GPU inference foundation) | P2 | XH | N (too ambitious for demo) |
| 147 | GSP: integrate gsp_platform_ops into existing gpu_driver HAL | P2 | XH | N |
| 183 | Gemma 4 E2B Q4_0 inference on Jetson Orin Nano | P2 (Post-Capstone) | XH | N |
| 109 | AI Eviction: Jetson Orin Nano latency benchmark | P2 (Capstone) | M | Y — headline metric |
| 108 | AI Eviction: Pi 5 Cortex-A78 latency benchmark | P2 (Capstone) | M | Y — headline metric |
| 29 | User/Kernel Privilege Separation (EL0/EL1 + TTBR split) | P2 (Post-Capstone) | XH | N |
| 250 | MACB driver: board-unique MAC address derivation | P3 | L | N |
| 328 | ctxsmoke verbose flag (dup) | P3 | L | N |
| 251 | make hw-test-pi5 target: automated boot_test + net regression | P3 | L | N |
| 238 | x86-64: dynamic MSI-X vector allocation | P3 | M | N |
| 243 | test-pc NIC driver: Realtek RTL8168/8111 | P3 | M | N |
| 199 | Phase 3: SSH server for remote shell access | P3 | M | **Y** — dramatic remote shell demo |
| 73 | x86-64: reload GDT in 64-bit mode when kernel moves to higher-half | P3 | M | N |
| 286 | Jetson xHCI: standalone Tegra XUSB Falcon firmware load | P3 | H | N |
| 142 | Future: GSP bare-metal loader for Ampere/Orin GPU compute | P3 | XH | N |
| 351 | Integrate native HTTP client for on-device file/model fetch | (no prio) | M | Y — "fetch model live" demo beat |
| 232 | Model hot-swap: atomically replace weights during in-flight inference | (no prio) | H | **Y** — strong demo moment |
| 72 | x86-64 SIMD: SSE intrinsics disabled (LLVM crash, blocked) | blocked | H | N |
| 71 | Cache tiling assumes 64KB L1D — verify for Cortex-A55 (32KB) | (no prio) | L | N |
| 62 | Component preloading: replace manual strcmp with strncmp | (no prio) | L | N |
| 63 | Add comment explaining LRU timestamp overhead in rust_infer_classify | (no prio) | L | N |
| 67 | Message router: concurrency stress tests for priority + wildcard delivery | Capstone | M | Y — reliability story |
| 60 | ARM PMU performance counters for inference profiling | Capstone | M | **Y** — live perf overlay |
| 61 | Real AI scheduler weights from Plan A export pipeline | Capstone | M | **Y** — shows trained policy |
| 55 | Dynamic batching for inference requests | Capstone | M | Y — throughput demo |
| 58 | XGBoost inference engine for scheduler policy | Capstone | H | Y — richer AI-sched story |
| 56 | SIMD optimization: NEON intrinsics, AVX, cache-friendly tiling | Capstone | H | Y — perf story |

---

## Enhancements — Post-Capstone / Icebox (22 open)

Not demo-relevant for this capstone; listed for completeness. All P3 unless noted.

| # | Title | Milestone |
|---|-------|-----------|
| 35 | eMMC/SD persistent storage driver | Post-Capstone (P2) |
| 25 | Jetson RTL8168 NIC via BPMP IPC | Post-Capstone (P2) |
| 24 | Jetson USB Serial Console (TinyUSB + Tegra XUSB) | Post-Capstone (P2) |
| 27 | Jetson GPU Memory Integration (gpu_map, SHM_GPU_ACCESSIBLE) | Post-Capstone |
| 26 | Jetson UEFI direct boot (PE/COFF relocations) | Post-Capstone |
| 34 | Real-Time Guarantees (RMS, priority ceiling, WCET) | Post-Capstone |
| 36 | Demand Paging for Model Memory | Post-Capstone |
| 40 | Development SDK & Tooling | Post-Capstone |
| 43 | REST API for remote component management | Post-Capstone |
| 44 | Network console (telnet/SSH alternative) | Post-Capstone |
| 45 | CI: coverage tracking | Post-Capstone |
| 47 | EFI stub: boot services integration | Post-Capstone |
| 110 | AI Eviction: x86-64 latency benchmark (informational) | Post-Capstone |
| 116 | AI Eviction: multi-CPU concurrent alloc/eviction stress test | Post-Capstone |
| 28 | TensorRT/CUDA Integration on Jetson (blocked) | Icebox |
| 30 | Component Sandboxing (per-component page tables) | Icebox (blocked) |
| 31 | Secure Boot Chain (Jetson fuse-based) | Icebox |
| 32 | Encrypted Model Storage (AES-256 + Jetson SE) | Icebox |
| 33 | Full POSIX Shell (fork/exec/pipes) | Icebox (blocked) |
| 38 | Dynamic Model Compilation (JIT/quantization) | Icebox |
| 39 | Advanced Power Management (DVFS, core gating) | Icebox |
| 41 | Component Marketplace | Icebox |
| 42 | Distributed Operation (multi-node SLM-OS) | Icebox |
| 46 | CI: real hardware test farm (Pi 5 + Jetson) | Icebox |

---

## Tech Debt (6 open)

| # | Title | Sev | Effort | Demo |
|---|-------|-----|--------|------|
| 78 | CORE-H3: Shell path resolution lacks mount-escape defense | P2 | M | N |
| 74 | Replace defensive `.unwrap()` in sched/heterogeneous.rs | P2 | L | N |
| 331 | hef_parser: collapse decode_*_body duplication + merge u32/u64 varint helpers | P3 | L | N |
| 101 | steal_deque: static_assert STEAL_DEQUE_CAPACITY >= MAX_TASKS | P3 | L | N |
| 100 | steal_deque: prefer NULL over `(struct task *)0` | P3 | L | N |
| 187 | bringup.h: gate trace_mode field behind SLM_HOST_HARNESS | P3 | L | N |
| 52 | Move export function registry to central location | P3 | L | N |
| 50 | Consider hash table for export function registry | P3 | M | N |
| 51 | Make direct channel limits configurable | P3 | L | N |

---

## Investigations (4 open)

| # | Title | Sev | Effort | Demo |
|---|-------|-----|--------|------|
| 340 | Hailo translator: stream_index from edge_layer metadata (not per-direction counter) | P2 | M | Y — correctness for multi-stream |
| 222 | Jetson UEFI doc: sharpen crash-PC analysis at 0x10070 (I-cache staleness) | P2 | L | N |
| 188 | WPR2 register encoding on GA107 post-FWSEC-FRTS | P3 | XH | N |
| 180 | Theory Radar for symbolic page-eviction and scheduler policies | P3 | XH | N |
| 176 | Uses for TurboQuant in SLM-OS | (needs triage) | H | N |

---

## Test Gaps (2 open)

| # | Title | Sev | Effort | Demo |
|---|-------|-----|--------|------|
| 330 | hailo_control: test MSI fast-path short-circuits polling loop | P2 | M | Y — reliability |
| 329 | Hailo CS translator: anchor enable_lcu wire format against external reference | P2 | M | N |

---

## Documentation (3 open, excluding fixed #323/#315)

| # | Title | Sev | Effort | Demo |
|---|-------|-----|--------|------|
| 223 | CLAUDE.md: refresh 'UEFI direct boot (WIP)' summary | P3 | L | N |
| 221 | Jetson UEFI doc: clarify 'str w12, [0x0C280000]' shorthand | P3 | L | N |
| 220 | Jetson UEFI doc: rewrite first-person plural to SLM-OS voice | P3 | L | N |

Note: #310 (`arm-smmu-noshutdown` fragility on newer kernels, P3) is a Jetson tooling note rather than a code issue — tracked under Documentation-adjacent.

---

## Capstone Deliverables (4 open)

| # | Title | Sev | Effort | Demo |
|---|-------|-----|--------|------|
| 86 | Capstone: Final project report | Capstone | H | N (writing, not demo) |
| 87 | Capstone: Presentation slides | Capstone | M | N |
| 88 | Capstone: Poster (if required) | Capstone | M | N |
| 89 | Capstone: Video demo (if required) | Capstone | M | **Y** (records the demo) |

---

## Summary

- **9 likely-fixed** issues recommended for closure after spot-check.
- **2 P1-high items** remain active: #347 (multi-context HEF — unlocks real models) and #309 (kexec USB re-plug).
- **1 P1 blocker bug** that endangers the Pi 5 demo path: #305 SD card reliability.
- **Demo-critical shortlist** (highest payoff for the interactive demo):
  - #347 multi-context HEF dispatch
  - #232 in-flight model hot-swap
  - #351 HTTP fetch for live model pull
  - #199 SSH remote shell
  - #60 PMU live perf overlay
  - #61 real AI scheduler weights
  - #108 / #109 Pi 5 & Jetson latency benchmarks
  - #305 must be resolved to keep Pi 5 on the demo roster
