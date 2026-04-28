# Admin & Telemetry Suite — Demo Runbook

**Status:** 2026-04-26
**Spec:** [docs/specs/admin-telemetry-suite.md](specs/admin-telemetry-suite.md)
**Milestones:** M1 #413 · M2 #415 · M3 #416 · M4 #417 · M5 #418 · M6 #419

End-to-end walkthrough of the admin & telemetry suite. Walks through
each TUI page, then runs the canonical "upload + launch + watch"
demo from a clean boot.

This document substitutes for screenshots — every page's expected
output is shown literally so an operator running the demo on real
hardware can match against it visually. ASCII drift between this
runbook and what the kernel actually emits is itself a regression
to file.

## Prerequisites

* SLM-OS built with `make kernel PLATFORM=JETSON_ORIN_NANO AI_SCHED=ON`
  and deployed via `sudo slmos-kexec /root/slmos.elf` to `jetson-nano-2`.
  Networking, DHCP-at-boot, telnetd autostart, and GA10B firmware
  embedding are all default-ON for `JETSON_ORIN_NANO`. The only
  prerequisite outside the build itself is having the firmware blobs
  staged at `$HOME/jetson-ga10b-firmware/`. Run
  `scripts/tools/fetch-ga10b-firmware.sh` once after a fresh clone —
  it scp's the 17-file GA10B nvgpu firmware set (~1 MB total) from
  `/lib/firmware/nvidia/ga10b/` on a running Jetson into the build
  host's `$HOME` (the script defaults match the CMake auto-detect
  path). The firmware is NVIDIA-proprietary and not in the repo.
  Equivalent QEMU build works for everything except the GPU consumer
  toggle's success path (no Jetson GA10B in QEMU).
* Telnet reachable: `nc 192.168.4.5 2323`. The shell prompt appears
  immediately.
* `/mnt/files` is mounted (LittleFS, preferably from the persistent
  boot-FAT-backed `0:/slmstore/files.lfs` image with RAM fallback).
  `demo_init` populates `/mnt/files/admin.lua` at boot — the `admin`
  shell command depends on this.

## 1. Tour: `admin` TUI

`admin` enters the seven-page Lua TUI from spec §11. It uses the
safe-table `slm.*` bindings only — every mutation lives behind
`lua-admin` so a casual telnet observer can't accidentally swap
the scheduler policy. Refresh is 1 Hz; key handling is non-blocking
via `slm.try_getc()`.

```
slmos> admin
```

The screen clears, the header bar shows `1 Overview  2 Tasks  3 Sched
4 Eviction  5 Models  6 Telemetry  7 REPL`, and page 1 (Overview)
renders.

### 1.1 Overview (key 1)

Single-screen system snapshot. Surfaces the consumers' default
state plus rolling telemetry totals.

```
+------------------------------------------------------------------+
| SLM-OS Admin  uptime 12s  f9                                     |
| 1 Overview  2 Tasks  3 Sched  4 Eviction  5 Models  6 Telemetry  |
+------------------------------------------------------------------+

System overview
  cpu_count        6
  memory           4128 KB used / 6803456 KB total
  sched policy     ai_mlp
  eviction policy  cacheus
  GPU ready        ON
  GPU consumers    sched=off  eviction=off  inference=off
  telemetry        7 events published (eviction=4, inference=3)

+------------------------------------------------------------------+
| q quit  r refresh  1-7 page                                      |
```

`GPU consumers` reflects the M2 toggle state; `telemetry` is the
M4 cumulative publish counter.

### 1.2 Tasks (key 2)

Live task list. Reads `slm.tasks()` per refresh.

```
Tasks
  id   name                 state      cpu
  1    main                 ready      0
  2    shell                running    0
  3    telnetd              ready      0
  4    sched_ai_loader      ready      1
  ...
```

A `task kill <id>` from `lua-admin` removes a row on the next refresh
without restarting the TUI.

### 1.3 Sched (key 3)

AI scheduler decision rate, percentile latencies, totals. Returns
nil + a friendly message when the active policy is `heuristic`
(no per-policy stats).

```
Scheduler
  policy           ai_mlp
  decisions/s      842
  fallbacks/s      0
  p50 / p90 / p99   12 us /  18 us /  47 us
  total decisions  10287   total fallbacks 4
  total ns         128456000
```

Wait 1 s (one refresh tick) under load and the rate moves;
percentiles stabilise as the histogram fills.

### 1.4 Eviction (key 4)

Same shape, scoped to `select_victim`. Counts come from the M3
Rust→C FFI hook in `runtime/src/mm/eviction/registry.rs`.

```
Eviction
  decisions/s      0
  fallbacks/s      0
  p50 / p90 / p99    -    /    -    /    -
  total decisions  0   total fallbacks 0
```

A bare run with no allocator pressure shows zeros. To exercise:
`bench eviction` from another shell, then re-open the page.

### 1.5 Models (key 5)

Engine registry + global inference rate. M5 ships READY engines for
`raw` and `mnist`; HAILO and GGML are NOSYS until those runtimes land.

```
Model engines
  name     state     summary
  raw      READY     load-and-report; never instantiated
  mnist    READY     built-in MNIST graph (rust_model_load_builtin_mnist)
  hailo    NOSYS     Hailo-8 HEF runtime (M5 stub — needs PR for HEF launch)
  ggml     NOSYS     generic ggml runtime (M5 stub — separate spec)

Inference
  calls/s          3
  errors/s         0
  p50 / p99       189 us / 412 us
  total calls      27   errors 0
```

### 1.6 Telemetry (key 6)

Cumulative feed counters per emitter topic + a one-line subscriber
hint. M4 ships `tel.evi` and `tel.inf`.

```
Telemetry feed
  tel.evi      published 4
  tel.inf      published 3
  total        7

  Subscribe pattern from another shell:
    lua -e 'slm.telemetry_subscribe("tel.*", function(t,d) print(t,d) end)'
```

### 1.7 REPL (key 7)

A launching pad rather than an embedded REPL.

```
REPL
  Press 'q' to leave the TUI and run `lua` from the shell prompt.
  Inline expressions are easier to test from a shell session than
  inside this TUI's input loop, so the M6 REPL page is intentionally
  a launching pad rather than an embedded REPL.
```

## 2. Runbook: upload → launch → watch

End-to-end demo. Uses two telnet sessions: session A drives the
upload + launch; session B watches the live telemetry feed.

### 2.1 Session B — start a subscriber

```
slmos> lua -e 'local h = slm.telemetry_subscribe("tel.*", function(t,d) print(t,d) end); print("subscribed handle="..h); while true do slm.sleep(1000) end'
subscribed handle=1
```

The subscriber blocks. As `tel.*` events arrive, they print
`<topic>  <key=value payload>`.

### 2.2 Session A — push a model, write its sidecar

For M5 the upload + sidecar are two separate operations (spec §14.9 —
the unified `model upload` flow lands when M6's host helper drives it).

```
slmos> # Push the blob via the existing xput protocol. For demos a
slmos> # 0-byte placeholder is fine because the `mnist` engine ignores
slmos> # the on-disk blob and uses its built-in graph.
slmos> xput begin /mnt/files/models/mnist.blob 1
XPUT ok begin path=/mnt/files/models/mnist.blob size=1
slmos> xput chunk 0 00
slmos> xput finish
slmos> write /mnt/files/models/mnist.meta "kind=mnist
size=1
"
```

`xput finish` writes the FNV-1a checksum into the file header; the
0-byte placeholder is fine because the `mnist` engine ignores the
on-disk content and uses the built-in graph.

### 2.3 Session A — verify the sidecar parses

```
slmos> model meta mnist
Meta for 'mnist':
  name           mnist
  kind           mnist
  size           1 bytes
```

Same parser path the M5 unit tests pin at the bucket-edge cases.

### 2.4 Session A — launch and observe

```
slmos> model launch mnist
model launch mnist: ok (task_id=0)
```

In session B, the `tel.inf` subscription should print one line per
inference call (driven by the engine's self-test on load):

```
tel.inf  dt=187234 ok=1
```

If the engine instead returns `MODEL_LAUNCH_ERR_NOSYS` for hailo /
ggml, that's expected per spec §10.2 — those engines are stubs in M5.

### 2.5 Watch the rate climb

Open `admin` in a third session and switch to page 5 (Models). The
`Inference total calls` count increments on each subsequent
`model launch` or shell-driven `model infer` call. The
`calls/s` number reflects the EWMA from M3.

## 3. Operating notes

### 3.1 Telemetry subscriptions block on stale ACKs

`msg_router_publish` waits up to `ACK_TIMEOUT_SECS` (5 s) for every
subscriber to ACK before returning. A wedged or disconnected
subscriber that holds a stale subscription stalls every record path
for 5 s — eviction is the most painful one because it's
allocator-driven. Mitigation, in order of preference:

* `slm.telemetry_unsubscribe(handle)` from the script that subscribed.
* Disconnect the telnet session: `slm.msg_router_unsubscribe_all` is
  called from the per-Lua-state teardown helper at shell exit.
* Reboot.

### 3.2 GPU consumer toggles

Three toggles, layered:

```
gpu use inference on|off          # master — controls all GPU inference dispatch
model use-gpu <name|idx> on|off   # per-model override (default ON at load)
gpu use sched on|off              # WIRED — ai_mlp policy on GA10B (PR-3)
gpu use eviction on|off           # scaffold — no eviction policy declares GPU yet
gpu use status                    # tabular view of all three flags
```

**inference** is wired through the MNIST GA10B fastpath
(`engine::mnist_gpu_fastpath_eligible`); flipping it ON on Jetson
with the v6 MNIST channel handoff present routes the model through
the GPU. **sched** is wired through `slm_gpu_run_sched_inference`
(PR-3 of `docs/specs/gpu-policy-models.md`): when ON, the active
`ai_mlp` policy's `ai_mlp_forward_logits` runs on GA10B for every
`assign_cpu` decision instead of CPU NEON, falling back to CPU
on dispatch error. Requires the sched-MLP v6 handoff staged via
`scripts/gpu-kernel-sched-mlp.c --preserve-for-kexec` pre-kexec.
**eviction** still accepts on/off as operator intent and emits a
`note: scaffold only …` warning — no eviction policy declares a
GPU forward pass yet (PR-5/PR-6 follow-up).

When the GPU isn't available at all (`slm_gpu_available() == 0`,
e.g. QEMU or non-Jetson builds), every `enable=true` short-circuits
to `EOPNOTSUPP` with reason `"GPU not available on this build"`.

### 3.3 What's not in this demo

* Per-task GPU dispatch — global toggle only (spec §14.1).
* Per-model `slm.inference_rate()` breakdown — single global
  series (spec §14.3 follow-up).
* Long-form telemetry topics + JSON sample payloads (spec §14.6).
* Shell-blocking `telemetry subscribe <pattern>` — Lua-side
  subscription is the workflow (spec §14.7).
* 1 Hz aggregate / heartbeat topics — the M1/M3 latency hist + rate
  already give that view (spec §14.8).
* `model upload <name>` as a single shell command — operators stage
  via `xput` + `write` (spec §14.9).
* Ref-checked `model unload <name>` — the existing
  `model unload <name|idx>` already refuses pinned models (spec §14.10).

## 4. Verifying the runbook

Re-run this document from a clean boot any time the demo is presented.
If any literal output below has drifted from what the kernel emits,
file a regression — the runbook is intentionally pinned to specific
strings so drift is visible without comparing screenshots.
