# SLM-OS Architecture

Overview of the system as it stands on `main` in November 2026 (written
against commit `8ec0e330`). This page names the parts and how they connect,
and links to the subsystem documents for the detail. Decisions and their
reasons are in the ADRs (`docs/adr/README.md`). Every source and
document path below exists on `main` at `8ec0e330`, except
`docs/adr/README.md` and ADRs 0009 to 0017, which were added together
with this rewrite; issue, PR and commit numbers point at the change that
established the behaviour described.

---

## Overview

SLM-OS is a bare-metal operating system written in C (kernel, drivers,
shell) and `no_std` Rust (inference and policy runtime). It boots on four
platforms, runs a preemptive SMP scheduler with work stealing, and treats
the scheduling and eviction policies as swappable modules, several of them
learned models. It runs small language models from GGUF files on the CPU
and, on the Jetson Orin Nano, as one GPU command stream on an Ampere GPU
that SLM-OS brings up itself.

```
┌──────────────────────────────────────────────────────────────────────┐
│  Shell (UART, telnet, SSH)            Lua scripts      EL0 ELF tasks │
├──────────────────────────────────────────────────────────────────────┤
│  Rust runtime (runtime/, staticlib slm-runtime)                      │
│  SLM: GGUF, tokenizer, forward pass, sessions   ONNX loader + engine │
│  Eviction policies   Model memory   Message router   Components      │
├────────────────────────── C/Rust FFI ────────────────────────────────┤
│  C kernel (kernel/)                                                  │
│  PMM/VMM  Scheduler + policies  SMP  IPC  VFS/littlefs/FAT  lwIP     │
│  GPU HAL + GA10B driver   Hailo-8L driver  USB/xHCI   BPMP   Camera  │
├──────────────────────────────────────────────────────────────────────┤
│  QEMU_VIRT        RASPI5           JETSON_ORIN_NANO        X86_64    │
│  Cortex-A76       Cortex-A76       Cortex-A78AE            x86-64    │
│  EL1              EL2 + VHE        EL2 + VHE               long mode │
└──────────────────────────────────────────────────────────────────────┘
```

---

## Platforms

The platform is chosen at build time (`PLATFORM` in `Makefile` and
`CMakeLists.txt`: `QEMU_VIRT`, `RASPI5`, `JETSON_ORIN_NANO`, `X86_64`).
Each has its own linker script in `kernel/`; `cmake/` has two toolchain
files, `toolchain-aarch64-none-elf.cmake` (shared by the three ARM64
platforms) and `toolchain-x86_64-none-elf.cmake`. Per-platform defaults
that change behaviour:

| Platform | CPU (`CMakeLists.txt`) | Exception level | Preemption default | AI_SCHED default (`Makefile`) | Linker script |
|---|---|---|---|---|---|
| `QEMU_VIRT` | `cortex-a76`, 4 cores (`QEMU_CORES` in `Makefile`) | EL1 (QEMU is not started with `virtualization=on`) | timer tick | OFF | `kernel.ld` |
| `RASPI5` | `cortex-a76` | EL2 + VHE (`kernel/arch/arm64/boot.S`, Pi 5 block) | `SECONDARY_PREEMPT=ON`, `COOP_PREEMPT=OFF` (#777) | ON | `kernel-raspi5.ld` |
| `JETSON_ORIN_NANO` | `cortex-a78ae`, 6 cores in 2 clusters | EL2 + VHE (`boot.S`, Jetson block) | `JETSON_HW_TICK=ON` implies `SECONDARY_PREEMPT=ON` (#755, #1050) | ON | `kernel-jetson.ld` |
| `X86_64` | generic x86-64, `-mno-sse` in kernel C | long mode | LAPIC timer | OFF | `kernel-x86_64.ld` (and kexec variants) |

Work stealing (`ENABLE_WORK_STEALING`) is ON for `X86_64`, `RASPI5` and
`JETSON_ORIN_NANO` and OFF for `QEMU_VIRT` (`CMakeLists.txt`, #59).

The reason Pi 5 and Jetson stay at EL2 is in ADR 0009: on Jetson the CBB
firewall admits UARTC, the GIC and the timer only from EL2, and on Pi 5
interrupt delivery to NS-EL1 does not work (comment in the Pi 5 block of
`boot.S`). The platform abstraction layer is described in
`docs/platform-abstraction.md`; per-capability status by platform is in
`docs/fact-sheets/`.

---

## Boot paths

All ARM64 platforms enter `kernel/arch/arm64/boot.S`. Code before the
`primary_cpu` label runs first: UEFI detection, an early UART mark, and
the Pi 5 and Jetson EL2/VHE setup blocks. The first instruction at
`primary_cpu` masks all exceptions (`msr daifset, #0xF`, against stale
interrupts after kexec) before stack, BSS and FPU setup; then `boot.S`
calls `kernel_main` in `kernel/src/main.c`. x86-64 enters
through `kernel/arch/x86_64/trampoline32.S` (Multiboot2) or
`bzimage_entry.S` (Linux bzImage protocol) and reaches `entry64.S`.

| Platform | Path | Entry | Notes |
|---|---|---|---|
| QEMU_VIRT | `-kernel slmos.elf` | `boot.S`, DTB in `x0` | `make run`, `make test` |
| QEMU_VIRT | QEMU AArch64 UEFI | `kernel/arch/arm64/efi_stub.c` | host check `make test-qemu-uefi` |
| RASPI5 | Pi firmware loads `kernel_2712.img`, with a custom TF-A BL31 as `armstub8-2712.bin` (`deploy/pi5/config.txt`) | `boot.S` | `docs/deploy/pi5-sdcard.md` |
| JETSON_ORIN_NANO | kexec from L4T Linux | `boot.S` | `scripts/jetson-kexec-slmos.sh` (`slmos-kexec`), `docs/deploy/jetson-kexec.md` |
| JETSON_ORIN_NANO | UEFI direct from the NVMe ESP (#26, PR #1085) | `efi_stub_entry` in `efi_stub.c`, then `boot.S` | `scripts/jetson-uefi-deploy.sh`, `docs/deploy/jetson-uefi-direct.md` |
| X86_64 | GRUB Multiboot2 ISO | `trampoline32.S`, `entry64.S` | linked at `0x100000` (`kernel-x86_64.ld`) |
| X86_64 | kexec, Multiboot2 (`make kernel-kexec`) | as above | linked at `0x20000000` (`kernel-x86_64-kexec.ld`) |
| X86_64 | kexec, bzImage wrapper (`make kernel-bzimage`) | `bzimage_entry.S` | linked at `0x20000000` (`kernel-x86_64-bzimage.ld`); `make kexec-verify` checks all three builds |

**Jetson: kexec versus UEFI direct.** A kexec boot inherits hardware that
Linux brought up (the GPU channel, XUSB, clocks); a UEFI-direct boot
starts from firmware state and brings that hardware up itself. Drivers
that need Linux-initialised hardware check `boot_source_is_uefi()`
(`kernel/include/boot_source.h`) before touching MMIO. On the UEFI path
SLM-OS powers XUSB through BPMP (`kernel/drivers/usb/xhci/xusb_t234_cold.c`,
#1084), runs the GPU cold bring-up (`ga10b_cold_run()` in
`kernel/gpu/nvidia/ga10b_cold.c`, called from `main.c`, #1088, #1092), asks
BPMP for an EMC bandwidth floor (`kernel/drivers/emc_hold.c`, #1161), and
receives GGUF models read through the firmware's file protocols
(`kernel/arch/arm64/efi_model.c`, `kernel/src/boot_models.c`, ADR 0002).
The whole-forward-pass GPU program (ADR 0003) exists only on the UEFI
path; kexec boots keep the per-op GPU path. The choice of two paths is
ADR 0010.

**x86-64 kexec.** The kexec variants exist so that SLM-OS inherits GPU
state from a Linux boot: after a Linux-to-SLM-OS kexec, SEC2 is
unlocked, which the GSP bring-up needs (`docs/x86-64-gpu-inference-status.md`,
section 0).

Initialisation order in `kernel_main` (`kernel/src/main.c`): UART, VMM and
PMM, the NC DMA block (`ncmem_init`, Pi 5 and Jetson), GIC and timer, SMP
(`smp_init`), VFS, block devices and littlefs, the Rust heap and
`rust_init()`, platform devices (on Jetson: BPMP, fan, GPU), the
scheduler, IPC, the main task, the shell task (`shell_start()`, called
unconditionally), then `scheduler_start(0)`. The main task runs the test
harness when `ENABLE_BOOT_TESTS` is defined and otherwise calls
`task_exit()` (`main_task_func`). Detail:
`docs/boot-sequence.md`, `docs/jetson-boot.md`.

---

## Memory

- **Physical:** buddy allocator, `kernel/mm/pmm.c` (firmware
  `/memreserve/` ranges honoured; see `kernel/CLAUDE.md`, "Buddy
  Allocator").
- **Virtual:** `kernel/mm/vmm.c`, `kernel/arch/arm64/mmu.S`; per-task EL0
  address spaces for ELF user tasks (`kernel/src/elf_user.c`). Detail:
  `docs/mmu.md`, `docs/memory-map.md`.
- **Kernel buffers:** `kernel/mm/kbuf.c`.
- **Model memory:** Rust allocator for weights and workspaces,
  `runtime/src/mm/model_mem.rs`, with a weight cache
  (`runtime/src/mm/weight_cache.rs`) and pluggable eviction (below).
  Detail: `docs/model-memory.md`.

### Cross-CPU memory is cacheable

CPU-shared state (run queues, task table, `current_task`, steal deques,
`sched_diag_*`, boot and init flags, preemption state) lives in ordinary
Normal Write-Back Inner Shareable memory on every platform, protected by
ordinary spinlocks, with ordering by barriers and acquire/release. There
is no CPU-to-CPU cache maintenance on shared state. One boot-time set/way
loop remains: with `SETWAY_INVAL=ON` (the default, `CMakeLists.txt`,
#1193) Pi 5 and Jetson secondaries invalidate L1/L2 by set/way after
their MMU comes on.

This has been the design since PR #1225 (commit `98e17440`, 2026-11-11,
"Remove the CPU-sharing NC axis on Pi 5 and Jetson"). From April 2026
until then, Pi 5 and Jetson kept that state in a Non-Cacheable block
(`PLATFORM_HAS_NC_MEMORY`) with hand-written cache maintenance, on the
belief that firmware left `CPUECTLR_EL1.SMPEN` clear and the private L2s
incoherent. The coherency gauntlet (#1193) measured coherence across all
CPUs on both boards, including between the Jetson clusters, and found
that neither core has an SMPEN bit; ADR 0007
(`docs/adr/0007-cross-cpu-coherency.md`) records the evidence and the
decision. ADR 0007 measured the cost of the NC placement at, for example,
2,318 ns against 105 ns per context switch on Pi 5.

What remains non-cacheable: a 2 MB block per board (`kernel/mm/ncmem.c`,
`kernel/include/ncmem.h`, `PLATFORM_HAS_NC_REGION`; Pi 5 `0xFFE00000`,
Jetson `0xBDE00000`) used only for device DMA buffers (xHCI, MACB,
CDC-ECM, USB bounce buffers, camera RCE), the Jetson hand-off pages and
the Pi 5 IRQ diagnostic slots. `kernel/include/cache.h` helpers are for
DMA, page tables and the caches-off `secondary_mmu_*` reads only.
Two checks hold the line: `kernel/tests/test_nc_axis.c` (placement) and
`scripts/check-cpu-cache-maint.py` (run by `make test`; rejects cache
maintenance in scheduler, task, SMP, IPC, kprintf and spinlock code).
Rules: `kernel/CLAUDE.md`, "Cross-CPU Coherency" and "Non-Cacheable DMA
Memory". The old design is described under `docs/archive/coherency-gauntlet/`.

---

## Scheduling and SMP

- **Core:** `kernel/sched/sched.c`, `task.c`, `smp.c`, `preempt.c`,
  `task_sleep.c`. Per-CPU run queues (`struct cpu_runqueue`) with
  separate `rq_lock[]` arrays; work stealing through per-CPU deques
  (`steal_deque.c`, `steal_deque_lock[]`). Secondary CPUs start through
  PSCI `CPU_ON` (`kernel/arch/arm64/smp_boot.S`); CPU ids resolve through
  `cpu_logical_map[]` (`kernel/include/smp.h`) and `cpu_logical_id()`
  (`kernel/include/preempt.h`),
  which handles the Jetson two-cluster MPIDR layout.
- **Preemption:** timer-driven on every CPU on Pi 5 and Jetson
  (`SECONDARY_PREEMPT`; `COOP_PREEMPT` is the CNTPCT-polled fallback).
  Tasks are switched in with IRQs masked and `task_entry_trampoline`
  unmasks them before the entry function runs (#1047); before #1047
  running tasks were never preempted. Detail:
  `docs/fact-sheets/preemption.md`.
- **Secondary-CPU recovery:** `kernel/sched/cpu_supervisor.c` (#216).
- **Priority inheritance:** `kernel/ipc/pi_mutex.c`.

### Task lifecycle (#1237)

PR #1237 (commit `5b28d065`, 2026-11-12) set the rules for reclaiming a
task slot; they are documented in `docs/scheduler.md`, "Task Slot
Reclaim", and in `kernel/CLAUDE.md`:

1. **`task->on_cpu`.** Set by the CPU that switches a task in; cleared
   with a store-release by the same CPU after `switch_to` returns on the
   next task's stack (`sched_finish_switch`). `task_destroy` waits for it
   to reach 0 (2 s bound, then warns and leaks the slot), because
   `TASK_TERMINATED` is published before the switch-out (#1213).
2. **Zombie generation.** The run queue records `zombie_gen` when it parks
   an exiting task, and the reaper calls `task_destroy_gen()`, which leaves
   a slot alone once its owner has destroyed and possibly reused it.
3. **Claim publishes state.** `task_create` sets `TASK_READY` in the same
   `task_lock` section that claims the slot.
4. **Post-switch flags follow the task.** After `switch_to`, `schedule()`
   re-reads `cpu_id()` before clearing `preempt_disabled` and the related
   flags, since a stolen task resumes on another CPU (#1229). The x86
   resched IPI handler returns while `preempt_disabled[cpu]` is set
   (#1236).

One window stays open: a task requeued as READY is visible to other CPUs
before its context is saved; waiting on `next->on_cpu` under `rq_lock`
deadlocks and was reverted (#1234). The `SLOT_TRACE` build option
(`kernel/include/slot_trace.h`, `kernel/sched/slot_trace.c`) traces slot
lifecycle events. Detail: `docs/scheduler.md`, `docs/smp.md`.

---

## Locks in two languages

C code uses `spinlock_t` from `kernel/include/spinlock.h`. On ARM64
(all three platforms) locks take a barrier-only path until `vmm_init`
sets `spinlock_hw_enabled` (exclusives fault on pre-MMU memory on
Cortex-A78AE) and run LDAXR/STXR afterwards. x86-64 uses
`__atomic_exchange_n` with a `pause` spin loop. Cross-CPU sections use
`spin_lock_irqsave`. The lwIP core lock (`kernel/net/sys_arch.c`) is
IRQ-save based and relies on every lwIP caller being pinned to CPU 0.

Rust code in `runtime/` has its own locks. The rule since #1093 (a
deadlock in `net_pump` on a Rust spinlock held by a preempted task): a Rust
lock reachable from an IRQs-off path is IRQ-saving (`slm_irq_save` /
`slm_irq_restore`, as `runtime/src/msg_router.rs` does) or is taken with a
one-shot try-lock (`runtime/CLAUDE.md`, "Locks reached from IRQs-off
context"). ADR 0011 records the model.

---

## Policy layer

Scheduling and eviction policies are pluggable, and new policies are
added as options rather than replacements.

**Scheduler policies.** `struct sched_policy_ops`
(`kernel/include/sched_policy.h`) with `sched_register_policy()` and
`sched_set_policy()`. Registered policies on `main`:

| Name | Source | Build |
|---|---|---|
| `heuristic` (default) | `kernel/sched/sched_heuristic.c` | always |
| `ai_mlp` | `kernel/sched/ai/sched_ai.c`, weights `ai_weights_mlp.c` | `AI_SCHED=ON` |
| `ai_ppo` | `sched_ai.c`, weights `ai_weights_ppo.c` | `AI_SCHED=ON` |
| `ai_xgb` | `kernel/sched/ai/sched_xgb.c` | `AI_SCHED=ON` |
| `ai_hailo` | `sched_ai.c` (Hailo backend, deprecated with ADR 0017) | `AI_SCHED=ON`, not on x86-64 |

`AI_SCHED` maps to the CMake option `ENABLE_AI_SCHEDULER` and is ON by
default for Pi 5 and Jetson (`Makefile`). `AI_WEIGHTS=synthetic|real`
selects which trained weight set is compiled in (#884); both are kept as
options (#848). Weights are compiled into the image as C arrays (ADR
0016). The CPU MLP backend is `kernel/sched/ai/inference_cpu.c`. Detail:
`docs/scheduler.md`, `docs/fact-sheets/ai-scheduler.md`.

**Eviction policies.** Rust trait `EvictionPolicy`
(`runtime/src/mm/eviction/policy.rs`), constructed by name in
`runtime/src/mm/eviction/registry.rs`: `lru` (default, overridable with
`EVICTION_DEFAULT_POLICY`), `lfu`, `arc`, `slm`, `xgboost`, `mlp`,
`cacheus`, `cacheus_all5`, `first_candidate`. The Cargo feature
`ai_eviction` is on unless `DISABLE_EVICTION=ON`; `EVICTION_MODELS=ON`
selects `ai_eviction_models`, which adds the generated XGBoost and MLP
weights (`runtime/src/mm/eviction/generated/`). Detail: `docs/eviction.md`,
`docs/fact-sheets/ai-eviction.md`.

**GPU-backed policies.** On Jetson the `ai_mlp` scheduler and the `mlp`
eviction policy can run on the GA10B GPU, switched per consumer with
`gpu use sched|eviction|inference on|off` (`kernel/src/gpu_consumer.c`,
`docs/design/gpu-policy-models.md`).

---

## Rust runtime and the FFI boundary

`runtime/` is one `no_std` crate, `slm-runtime`, built as a `staticlib`
(`runtime/Cargo.toml`) for `aarch64-unknown-none` or `x86_64-unknown-none`
and linked into `slmos.elf` (`CMakeLists.txt`, `RUST_TARGET`). The
toolchain is pinned by `rust-toolchain.toml`. Modules under
`runtime/src/`:

| Module | Purpose |
|---|---|
| `slm/` | GGUF parser (`gguf.rs`), tokenizer, chat template, forward pass, KV cache, sampler, decoder, sessions, GPU program builder (`gpu_program.rs`), goldens. Cargo feature `slm`, on by default |
| `loader/` | ONNX protobuf parser, operator graph, model registry (`docs/onnx-support.md`) |
| `inference/` | ONNX execution engine, NEON/SSE ops, transformer ops, quantisation, GPU offload |
| `mm/` | model memory, weight cache, eviction policies |
| `sched/` | deadline hints, heterogeneous-core topology, XGBoost scheduler model |
| `ml/` | XGBoost tree evaluator |
| `component/` | component registry and key-value manifest parser |
| `msg_router.rs` | topic-based publish/subscribe router |
| `kernel_ffi.rs`, `log.rs` | Rust-side declarations of C functions, logging through the UART |

The boundary is plain C ABI in both directions. The C side declares Rust
entry points in `kernel/include/slm_ffi.h` and implements the kernel
services Rust calls in `kernel/src/slm_ffi.c`; the Rust side declares the
C functions in `runtime/src/kernel_ffi.rs` and exports `#[no_mangle]
extern "C"` functions. `kernel_main` gives Rust a heap
(`rust_heap_init`) and then calls `rust_init()`. Conventions for errors,
ownership and opaque handles are in `docs/ffi.md`; crate conventions in
`runtime/CLAUDE.md`.

---

## Inference

- **Small language models (GGUF).** Models are GGUF files
  (`runtime/src/slm/gguf.rs`; ADR 0015), delivered on the filesystem or,
  on a UEFI-direct boot, through firmware file protocols (ADR 0002).
  Driven from the shell (`slm` verbs, `kernel/src/slm_shell.c`,
  `kernel/src/slm_runner.c`). Correctness is held against llama.cpp by
  goldens; the #475 performance bar is ADR 0001. On Jetson after a
  UEFI-direct boot, `slm launch --gpu auto|on` runs the forward pass as
  one GPU command stream (`kernel/gpu/slm_prog.c`,
  `runtime/src/slm/gpu_program.rs`, ADR 0003, ADR 0004, ADR 0005), with
  in-place channel recovery after a step timeout (ADR 0006). Detail:
  `docs/design/slm-gpu-program.md`, `docs/design/slm-integration.md`.
- **ONNX.** `runtime/src/loader/` and `runtime/src/inference/`.
- **Inference devices.** A registry with a vtable per backend,
  `kernel/inference/inference_device.c`; backends are the CPU
  (`kernel/sched/ai/inference_cpu.c`) and the Hailo NPU
  (`kernel/inference/inference_device_hailo.c`).
- **Model engines.** `kernel/src/model_engine.c` reads `.meta` sidecars
  and dispatches launches by model kind.

### GPU

`kernel/gpu/gpu.c` is a driver registry; `main.c` registers
`gpu_nvidia_driver` on Jetson and `gpu_stub_driver` elsewhere. The Jetson
GA10B driver is under `kernel/gpu/nvidia/` (`ga10b_*`: bring-up,
channels, GMMU, PMU, FECS/GPCCS context switch firmware, QMD, cold
bring-up, MNIST and policy pipelines). An operator library (`.incbin` via
`kernel/src/oplib_embed.S`; `kernel/gpu/operator_library.c`, `oplib_*.c`)
supplies the GPU kernels. The x86-64 RTX 3050 path drives GSP-RM
(`kernel/gpu/nvidia/gsp.c`, `kernel/arch/x86_64/nvidia_gpu.c`); its
status is in `docs/x86-64-gpu-inference-status.md`. Pi 5 and QEMU have no
compute GPU. Detail: `docs/gpu.md`, `docs/gpu-compute.md`,
`docs/nvidia-gsp.md`, `docs/fact-sheets/gpu-inference.md`.

### Hailo NPU (Pi 5 AI HAT+), deprecated

The fitted part is a Hailo-8L (user, 2026-11-14), and the driver and the
Hailo docs target it: the translator emits the 8L variant
(`kernel/ai_accel/hailo/hailo_cs_actions.h:251`) and the HEF compile
wrapper defaults to `hailo8l`. One mismatch remains: the HEF fixture
fetched by `scripts/tools/fetch-hef-fixtures.sh` is a Hailo-8 build,
meant for `hailo load` on the device (ADR 0017). The integration does not
work and the path is deprecated (ADR 0017); whether to delete or freeze
it is #1254.

`kernel/ai_accel/hailo/`: PCIe bring-up on the Pi 5 (`hailo_pi5.c`,
`kernel/drivers/pcie/pcie_bcm2712.c`), firmware control RPCs
(`hailo_control.c`), HEF parsing (`hef_parser.c`, nanopb `hef.pb.c`),
context-switch translation, VDMA, inference and a boundary trace. Inference
through it stalls on the IN channel; the investigation (#682, #1001) is
closed as not planned and written up in `docs/hailo-inference-wedge.md`.
The path is ADR 0017. Detail: `docs/hailo-protocol-architecture.md`,
`docs/hailo-lifecycle.md`.

---

## Shell, scripting and user space

- **Shell.** `kernel/src/shell.c` and the `shell_*.c` files; sessions over
  UART, telnet (`telnet.c`, `tcp_shell_server.c`) and SSH share one
  command set (`shell_session.c`). `kernel/src/shell_sys.c` holds the
  system, GPU, bench and diagnostic verbs. The shell is the surface for
  bring-up, diagnostics, demos and hardware tests driven over serial; ADR
  0012. Detail: `docs/shell.md`.
- **Lua.** Lua runs inside the kernel (`kernel/lib/lua/`, bindings in
  `kernel/src/lua_slm.c`, `lua_shell.c`); demo scripts in `scripts/*.lua`
  are embedded when `EMBED_DEMO_SCRIPTS=ON`. ADR 0013; `docs/lua.md`.
- **User space.** Static ARM64 ELF tasks at EL0 (`kernel/src/elf_user.c`,
  `kernel/src/syscall.c`, `kernel/arch/arm64/user_entry.S`; sample program
  `user/hello/`). Kernel-mode ELF loading in `kernel/src/elf.c`.
- **Components.** C helpers in `kernel/src/component.c` and
  `component_runtime.c`, registry in `runtime/src/component/`. Detail:
  `docs/components.md`, `docs/component-isolation.md`.
- **IPC.** Message queues and shared buffers in `kernel/ipc/ipc.c`;
  publish/subscribe in `runtime/src/msg_router.rs`. Detail: `docs/ipc.md`.

---

## Storage and networking

- **Filesystems.** In-memory VFS (`kernel/src/vfs.c`), littlefs
  (`kernel/lib/littlefs/`, `kernel/fs/`), FAT through FatFs
  (`kernel/lib/fatfs/`), block devices (`kernel/drivers/blkdev.c`,
  `sdhci.c`, `ramdisk.c`). Detail: `docs/vfs.md`, `docs/filesystem.md`.
- **Networking.** Vendored lwIP (`kernel/lib/lwip/`, port in
  `kernel/net/lwip_slm.c`, `sys_arch.c`), SSH through vendored wolfSSH
  and wolfCrypt (`kernel/lib/wolfssh/`, `kernel/lib/wolfcrypt/`,
  `kernel/net/ssh/`, build option `NET_SSHD`), HTTP client
  (`kernel/src/net_http.c`). NICs: virtio-net (QEMU MMIO, x86-64 PCI), Cadence
  MACB through RP1 (Pi 5, `kernel/drivers/macb.c`), USB CDC-ECM over the
  Tegra xHCI (Jetson, `kernel/usb/class/cdc_ecm.c`,
  `kernel/drivers/usb/xhci/`). An RTL8168 driver for Jetson PCIe
  (`kernel/drivers/eth_rtl8169.c`) is Stage-1 scaffolding whose init does
  not set up rings (#25). ADR 0014. Detail: `docs/networking.md`,
  `docs/ssh.md`, `docs/security.md`.

---

## Jetson platform drivers

BPMP IPC (`kernel/drivers/bpmp/`), Tegra PCIe C8
(`kernel/drivers/pcie/pcie_tegra194.c`), CPU frequency sync after kexec
(`kernel/drivers/tegra_cpufreq.c`, #1053), fan control
(`kernel/drivers/tegra_fan.c`, #1105), EMC floor (`emc_hold.c`, #1161),
camera RCE and IMX219 (`kernel/drivers/camrtc/`, `kernel/drivers/camera/`),
GPIO and I2C. Board notes and solved hardware issues are in the project
`CLAUDE.md`, "Jetson Hardware"; `docs/jetson-boot.md`,
`docs/jetson-cbb-report.md`.

---

## Directory layout

```
kernel/
  arch/arm64/      boot.S, smp_boot.S, vectors, MMU, UEFI stub, EFI model delivery
  arch/x86_64/     Multiboot2 + bzImage entry, IDT, APIC, ACPI, PCI, SSE kernels
  ai_accel/hailo/  Hailo NPU driver (deprecated), HEF parser, VDMA, trace
  drivers/         UARTs, GIC, timer, SDHCI, MACB, virtio, PCIe, BPMP,
                   camera, camrtc, GPIO, I2C, usb/xhci, Tegra cpufreq/fan/EMC
  fs/              littlefs glue
  gpu/             GPU HAL, operator library, SLM GPU program
  gpu/nvidia/      GA10B driver (Jetson), GSP-RM (x86-64)
  include/         kernel headers, incl. slm_ffi.h, sched_policy.h
  inference/       inference-device registry, Hailo backend
  ipc/             message queues, shared buffers, PI mutex
  lib/             vendored: lua, lwip, wolfssh, wolfcrypt, littlefs, fatfs,
                   fdt, nanopb
  mm/              PMM, VMM, kbuf, NC DMA allocator
  net/             lwIP port, SSH host key and password store
  sched/           scheduler, tasks, SMP, preemption, work stealing
  sched/ai/        AI scheduler policies, compiled weights, CPU MLP backend
  src/             kernel_main, shell, Lua bindings, VFS, syscalls, ELF,
                   telnet/HTTP, SLM shell, telemetry
  tests/           in-kernel test suites (make test)
  usb/             USB core, CDC-ECM class driver
runtime/src/       Rust runtime (see table above)
user/hello/        sample EL0 program
scripts/           deploy, kexec, checks run by make test, Lua demos, GPU kernels
host-tools/        host-side tools: gguf-inspect, GA10B ACR, GSP harness, Hailo RE
tools/             nvgpu and TF-A patches (Jetson)
cmake/             toolchain files, generators for build info and weights
bench/475/         llama.cpp reference runs (ADR 0001)
models/test/       test model fixtures
deploy/pi5/        Pi 5 boot configuration
docs/              documentation; docs/adr/ for decisions
```

---

## Testing

`make test` builds a test kernel (`ENABLE_BOOT_TESTS`) and runs the
suites in `kernel/tests/` under QEMU, together with host-side checks under
`scripts/` (for example `check-cpu-cache-maint.py`,
`check-gpu-shell-lock.py`, `check-emc-rate-path.py`). The Rust crate has
host unit tests, run twice by `make runtime-test` (`--features slm`, then
`--features slm,ai_eviction_models`; `Makefile`). Hardware runs go through labctl. Detail: `docs/testing.md`,
`docs/fact-sheets/testing.md`.

---

## Planned direction

Recorded with the user's answers to the backfilled ADRs (2026-11-14):

- **User/kernel separation.** Per-process address spaces under the EL2
  kernel. The first kernel was deliberately small and limited, and
  splitting user and kernel space is an intended direction. ELF user
  programs are its first consumer, with shell verbs moving to user space
  (ADR 0012). Builds on the existing EL0 syscall path
  (`kernel/src/syscall.c`; trap side in `kernel/arch/arm64/vectors.S`,
  `kernel/arch/arm64/exceptions.c` and `kernel/arch/arm64/user_entry.S`)
  (user, 2026-11-14).
- **Retire kexec.** UEFI-direct becomes the only boot path on Jetson and
  x86-64, including both x86 kexec loader variants (ADR 0010, #1252).
- **Replace third-party stacks.** Move SSH and crypto from GPL wolfSSH and
  wolfCrypt to an MIT/BSD-compatible stack (ADR 0014, #1258);
  project-written replacements are acceptable later.
- **Runtime weight loading.** Load model weights at run time instead of
  compiling them into the image (ADR 0016, #1253).
- **Hailo.** The Hailo path is deprecated; delete or freeze is decided in
  #1254 (ADR 0017).

---

## History

- This file was first written in December 2025 (`176f974d`, 2025-12-23).
  Before this rewrite it described the system phase by phase (Phase 3 to
  Phase 6, status line dated April 2026) and was edited piecemeal through
  2026-11-11 (`98e17440`). That version is in git history.
- The December 2025 design document (a microkernel with two-level page
  tables, a YAML component manifest and TensorRT integration) is
  superseded by the system described here. It is not in this repository;
  it lives outside the repo, in the project owner's planning materials.
- Before November 2026 decisions were recorded in `CLAUDE.md` prose,
  commit messages and `docs/archive/`. ADRs 0001 to 0007 were written from
  2026-11-07 on; ADRs 0009 to 0017 backfill earlier decisions from that
  record. Index: `docs/adr/README.md`.
