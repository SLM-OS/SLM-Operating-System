# Industrial IoT Demo

## Overview

The SLM-OS industrial IoT demo exercises the major runtime subsystems in a
single scripted sequence: component lifecycle management, publish/subscribe
message routing, threshold-based anomaly detection, and live hot-swap of a
running component. The scenario simulates an industrial sensor pipeline where
readings flow through a monitoring component that raises alerts when values
exceed a configured threshold. Mid-stream, the monitoring component is
replaced with a new instance while preserving message subscriptions,
demonstrating zero-downtime upgrades.

Five scripts are embedded at boot:

| File | Purpose |
|------|---------|
| `/mnt/files/demo.lua` | Linear Industrial IoT walkthrough. Source of truth: `scripts/demo.lua`. |
| `/mnt/files/demo_menu.lua` | Interactive menu covering all core features (SMP, scheduling, eviction, inference, components, Hailo NPU). Source of truth: `scripts/demo_menu.lua`. |
| `/mnt/files/demo_auto.lua` | 5-section scripted auto-demo with Enter-to-advance pauses. Source of truth: `scripts/demo_auto.lua`. |
| `/mnt/files/multiproc_demo.lua` | Multi-process / concurrent-task reference. Source of truth: `scripts/multiproc_demo.lua`. |
| `/mnt/files/demo_hailo.lua` | Hailo-8L NPU walkthrough — probe, load, infer via `slm.hailo.*`. Source of truth: `scripts/demo_hailo.lua`. |

All five use the same Lua API bindings (`slm.*`) and exercise the same
kernel subsystems.

### How scripts get embedded

Every `scripts/*.lua` file listed above is embedded into the kernel ELF at
link time via `.incbin` in `kernel/src/demo_scripts.S`. The repo files are
the single source of truth — there is no C-string mirror to keep in sync.
`demo_init.c` references the `_start`/`_end` symbols produced by the
assembler and writes each blob to `/mnt/files` at boot.

Embedding is gated on the `EMBED_DEMO_SCRIPTS` CMake option (default `ON`).
Pass `EMBED_DEMO_SCRIPTS=OFF` to leave the scripts out of the kernel image.
The Lua interpreter and all `slm.*` bindings remain fully functional in
either mode; scripts can still be written to the filesystem at runtime
(`write /mnt/files/foo.lua ...`).

## Prerequisites

- SLM-OS kernel built for the target platform (`make kernel` or `make kernel PLATFORM=RASPI5`)
- QEMU or Raspberry Pi 5 hardware with serial console access
- The kernel must have booted to the SLM-OS shell prompt (`slm>`)

No external files or model weights are required. The demo script is written
to the LittleFS filesystem at `/mnt/files/demo.lua` during kernel
initialization.

## Running the Demo

### Linear walkthrough

```
slm> lua /mnt/files/demo.lua
```

Runs the embedded industrial IoT scenario end-to-end in ~6–7 seconds. Each
section includes brief pauses (`sleep`) for readability on a serial console.

### Interactive menu (recommended for live demonstrations)

```
slm> lua /mnt/files/demo_menu.lua
```

Numbered menu with thirteen options — one per core feature plus an all-in-one
"full tour" that drives every subsystem in sequence. Each menu item prints a
short explanation then exercises the relevant `slm.*` bindings against the
live kernel, so a reviewer can see the actual structured data (not formatted
shell output) flowing through Lua.

| Key | Option |
|-----|--------|
| 1 | System overview — version, memory, IPC, VMM, model pools |
| 2 | SMP — per-core ticks / schedules / isolation + pinned worker demo + `bench smp` |
| 3 | Scheduling — policy list, stats, AI scheduler stats |
| 4 | Switch scheduler policy at runtime |
| 5 | Eviction — current policy + pool stats + CACHEUS expert weights |
| 6 | Switch eviction policy at runtime |
| 7 | Inference — load MNIST, infer, bench, stats, GPU status |
| 8 | List loaded models |
| 9 | Components & hot-swap (sensor_monitor + `/sensors/data`) |
| h | Hailo NPU (AI HAT+) — delegates to `demo_hailo.lua` |
| t | Full system tour |
| p | Active tasks |
| s | Drop to shell command |
| q | Quit |

### Multi-process reference demo

```
slm> lua /mnt/files/multiproc_demo.lua
```

A separate scenario focused on multi-task concurrency — spawns the
`listener`, `sensor_monitor`, and `counter` built-in components, includes
a live IPC pub/sub dashboard, and includes a pinned SMP-worker option
that keeps three Lua tasks per secondary CPU alive for ~8 seconds.
Useful when you want `top` or a second telnet session to show both the
current task and nonzero per-CPU ready counts instead of the
near-instant `bench smp` dispatch.

### Hailo NPU demo

```
slm> lua /mnt/files/demo_hailo.lua
slm> lua /mnt/files/demo_hailo.lua /mnt/files/mobilenet_v1.hef
slm> lua /mnt/files/demo_hailo.lua /mnt/files/mobilenet_v1.hef 500
```

Walks through the Hailo-8L AI HAT+ bring-up entirely from Lua:

1. **Probe** — `slm.hailo.status()` reports availability, slot usage,
   and the registered backend name (`hailo-8` when the AI HAT+ is
   detected at boot).
2. **Load** — `slm.hailo.load(path)` stages a HEF binary from the VFS
   and runs the firmware context-switch sequence
   (ACTIVATION → PRELIMINARY → DYNAMIC → ENABLED). Returns a handle.
3. **Infer** — `slm.hailo.infer(handle, input_bytes)` pushes the input
   tensor over a boundary DMA channel, waits for completion, and
   returns the raw output tensor as a Lua string. The script probes a
   handful of common input sizes (1×1, 28×28, 224×224, 224×224×3, etc.)
   until it finds one the loaded model accepts.
4. **Benchmark** — runs the inference in a loop and prints rolling
   throughput every ~10 % of iterations, then a summary with total
   FPS and latency percentiles (p50 / p95 / p99, plus min / max / avg).
   Default is 100 iterations; pass an integer as the second argument
   to override (use `0` to skip the benchmark entirely).
5. **Report** — prints an INT8 argmax of the first output chunk plus a
   16-byte hexdump so the reviewer sees actual NPU output, not just a
   length count.
6. **Unload** — `slm.hailo.unload(handle)` releases the NPU slot.
   Scripts that cycle through multiple models must unload before the
   next `load()` since the backend caps at four concurrent slots.

On builds without the Hailo backend (QEMU, Pi 5 without the AI HAT+,
other platforms) the script prints the `available=false` status and
exits cleanly — the `slm.hailo.*` bindings always exist, so the same
script runs everywhere but only exercises hardware when present.

The default HEF path is `/mnt/files/mobilenet_v1.hef`; stage a
different compiled HEF with the shell's `write` command and pass it
as the first script argument. The optional second argument sets the
benchmark iteration count (e.g., `lua /mnt/files/demo_hailo.lua
/mnt/files/my_model.hef 500`).

## Demo Walkthrough

### System Overview

The script begins by querying kernel state through Lua API bindings:

- **Version** -- `slm.version()` returns the SLM-OS version string
- **CPUs** -- `slm.cpu_count()` reports the number of online cores
- **Scheduler** -- `slm.sched_policy()` returns the active scheduling policy (e.g., `round_robin` or `ai_ppo`)
- **Memory** -- `slm.mem_stats()` returns free and total kernel heap in KB

This section confirms the kernel is operational and provides context for the
hardware platform.

### Starting Sensor Monitor

The demo calls `slm.component_run("sensor_monitor")` to launch the built-in
sensor monitor component. This component:

1. Registers itself with the component runtime
2. Subscribes to the `/sensors/data` message topic via `msg_router_subscribe`
3. Enters a receive loop, parsing each message as an integer sensor value

The component runs as a separate kernel task. Two `yield()` calls allow it to
initialize and register its subscription before the demo continues. The script
then reports the number of active components via `slm.component_count()`.

### Sensor Data Stream

The script publishes a series of simulated sensor readings to `/sensors/data`
using `slm.msg_publish()`:

**Normal readings** (23, 31, 42) -- all below the threshold of 50. The
sensor monitor logs each as "normal" and takes no further action.

**Anomalous readings** (78, 95) -- above the threshold. For each, the sensor
monitor:
- Logs an alert message (`ALERT: value=78`, etc.)
- Publishes an alert to `/alerts/threshold` via `msg_router_publish`

Each `msg_publish` call returns the number of subscribers that received the
message (expected: 1). Between readings, `yield()` and `sleep(300)` give the
sensor monitor time to process.

### Live Hot-Swap

`slm.component_hot_swap("sensor_monitor", "sensor_monitor")` replaces the
running sensor monitor instance with a fresh one. The hot-swap operation:

1. Marks the old component instance as terminating
2. Starts the new instance
3. Transfers existing message subscriptions from the old instance to the new one
4. Returns the new component index on success

The old instance's receive loop exits on its next timeout check. The new
instance inherits the `/sensors/data` subscription without any messages being
lost during the transition window.

### Verification

After hot-swap, the demo publishes one more reading (value 88, above
threshold) to confirm the new instance is receiving messages and generating
alerts. The subscriber count returned by `msg_publish` confirms delivery.

### AI Model Inference

The demo loads the built-in MNIST ONNX model (26 KB, embedded in the kernel
binary) via `slm.model_load_mnist()`. It then runs inference with
`slm.model_infer()`, producing a digit classification result. With zero input,
the MNIST model predicts class 5, matching the ONNX Runtime reference output.

### AI Scheduler

The demo displays the current scheduling policy. When built with
`AI_SCHED=ON`, the AI MLP and PPO policies are available and can be activated
via `sched policy ai_mlp` from the shell.

The demo concludes with a summary of the subsystems exercised: component
lifecycle, pub/sub messaging, anomaly detection, hot-swap, ONNX inference,
AI scheduling, and multi-core operation.

## Expected Output

Running `/mnt/files/demo.lua` prints, in order:

1. A boxed banner introducing the demo.
2. **System Overview** — version, CPU count + current CPU, scheduler
   policy, RAM usage, weight / workspace pool blocks, uptime.
3. **Sensor Monitoring Pipeline** — `sensor_monitor` start confirmation
   and active-component count.
4. **Sensor Data Stream** — five normal readings, each delivered to one
   subscriber; three anomaly readings, each triggering an `ALERT` line
   from the monitor and an `[alert-consumer] received: ALERT: value=N`
   line from the stub subscriber registered at demo start.
5. **Live Component Hot-Swap** — transition message, index of the new
   instance, one verification reading (value `88`) delivered to exactly
   one subscriber.
6. **On-Device Model Inference** — MNIST load confirmation and a
   predicted class.
7. **Final System Status** — task / component counts, memory consumed
   during the demo, total alerts seen, elapsed demo time.
8. Closing banner listing demonstrated subsystems.

The demo script completes in roughly 6–10 seconds depending on platform.
`[sensor_monitor]` lines interleave with script output because the
component runs as a separate kernel task and writes directly to UART;
this is expected.

Values that differ by platform / build config — CPU count, memory sizes,
inference latency, and exact alert ordering under hot-swap — are not
reproducible verbatim, so no literal transcript is shown here.

## Platform Notes

### QEMU (ARM64 and x86-64)

The demo runs identically on QEMU. The primary difference is the reported CPU
count: QEMU defaults to fewer virtual CPUs (typically 4) depending on the
`-smp` argument passed at launch. Memory totals also differ based on the
configured RAM size.

### Raspberry Pi 5

Tested with 5/5 boot reliability. The embedded demo completes in
approximately 6.6 seconds. Serial output is captured via the labctl serial
infrastructure over the Pi 5 debug UART.

### Jetson Orin Nano

The demo is currently blocked on the Jetson platform. After kexec from Linux,
the nvgpu driver leaves stale GPU DMA operations that trigger a RAS
Uncorrectable Error handled by TF-A (EL3), which powers off the CPU core.
This is a Linux/TF-A interaction issue unrelated to the demo itself. Once the
kexec RAS error is resolved, the demo is expected to work without
modification since the Jetson platform supports all required subsystems
(components, message routing, hot-swap).

## Known Issues

### Garbled output during hot-swap

Output from the old and new `sensor_monitor` instances may interleave
briefly during the hot-swap transition. The UART has no cross-CPU lock
on Pi 5 (see `kernel/CLAUDE.md` §"UART Lock on Pi 5 / Jetson"). The
garbling is cosmetic and does not indicate a functional error.

---

## Troubleshooting

### Demo script not found

```
lua: cannot open demo.lua
```

The embedded demo is at `/mnt/files/demo.lua`, not a relative path. Use the full path:

```
slmos> lua /mnt/files/demo.lua
```

If the file is missing, the LittleFS ramdisk may not have mounted. Check boot output for `LittleFS mounted at /mnt/files`. A clean reboot should restore it (the demo is written at every boot by `demo_init()`).

### Demo hangs after "Starting Sensor Monitor"

The sensor monitor uses a 30-second hardware counter timeout. If the demo appears stuck, wait up to 30 seconds. If it still does not progress, the timer hardware may not be running. Verify with:

```
slmos> bench irq
```

If the timer frequency shows 0, the ARM generic timer was not initialized.

### Component already running

If the `demo_menu.lua` components option (9) is invoked within 30 seconds
of a prior run, the prior `sensor_monitor` instance is still alive and
the kernel logs `Component 'sensor_monitor' is already running`. The
menu detects this via `slm.component_find` and reuses the prior
instance for the rest of the scenario; no user action is required.
For a fully clean run, either wait for the prior instance's 30-second
inactivity timeout or reboot.
