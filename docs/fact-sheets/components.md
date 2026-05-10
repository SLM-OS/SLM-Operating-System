# Components — Fact Sheet

Component system: lifecycle, hot-swap, isolation.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Component model | Long-lived services registered by name | Same | Same | Same |
| Lifecycle ops | `init / start / stop / destroy` | Same | Same | Same |
| Hot-swap | ✅ atomic replace with cleanup | ✅ | ✅ | ✅ |
| Component generation counter | Per-slot (prevents cleanup of reused slot) | Same | Same | Same |
| Example components | `sensor_monitor`, `demo echo`, `anomaly_detector` | Same | Same | Same |
| EL0 task creation primitives | `task_create_user` (inline EL0 entry, #697) + `task_create_user_elf` (loads embedded ELF, #734) | Same | Same | ❌ CPL=3 path unwired |
| EL0 syscalls | `SYS_EXIT`, `SYS_WRITE`, `SYS_MMAP` / `SYS_MUNMAP` (#731), `SYS_LOG` | Same | Same | ❌ |
| Per-task user address space | ✅ `TTBR0_EL1` switched per-task (#728) | ✅ | ✅ | ❌ shared CR3 today |
| Per-task ASID | ✅ TLBI on recycle, no flush on swap (#738) | ✅ | ✅ | ❌ |
| EL0 hardware verification | ✅ `usertest`, `mmaptest`, `userelf` shell verbs | ✅ verified on pi-5-2 (under both coop and HW preempt) | ✅ | ❌ |
| EL0 dynamic memory (mmap from EL0) | ✅ `SYS_MMAP` returns user VA, page-faults service via PMM_OWNED | ✅ | ✅ | ❌ |
| Shell surface | `component list/run/swap/register/status` + `usertest/mmaptest/userelf` | Same | Same | Same (run/swap work; no EL0 components) |
| Lua bindings | `slm.component_run`, `component_swap`, `component_list` | Same | Same | Same |
| Message-router integration | Components subscribe/publish to topics | Same | Same | Same |
| Runtime monitoring | State tracked in `struct component`; visible via `top` and `component list` | Same | Same | Same |
| Hot-swap demo (Lua) | `slmos> lua /mnt/files/demo.lua` | Same | Same | Same |

## What landed (April–May 2026)

- **#697 (PR-4) — EL0 smoke task on Pi 5 hardware.** `task_create_user` lands an EL0 task with its own page table; verified via `usertest` shell command.
- **#728 — per-page PMM ownership tracking + `vmm_user_unmap_page`.** Foundation for safe user-VA teardown.
- **#731 — `SYS_MMAP` / `SYS_MUNMAP` for EL0 dynamic memory.** Verified via `mmaptest` shell command (`mmap+write+read+munmap ok`).
- **#734 — EL0 ELF loader.** `task_create_user_elf` parses an embedded ELF, sets up TTBR0, jumps to entry point. Verified via `userelf` shell command.
- **#738 — per-task ASID tagging.** TLBI on slot recycle instead of full flush on every swap.

What's still missing for `slm-runner` to run in EL0: the component-runtime
shim that wraps a long-lived EL0 task as a registered component (subscribe
to topics, publish results, integrate with hot-swap). The lifting required
is now the integration glue, not the EL0 bring-up.

## Skipped / Blocked

- **`slm-runner` and other components running in EL0 on real hardware** — the EL0 *task* path works (#697/#731/#734), but the component runtime still creates components in EL1. Wiring the `component run` path to use `task_create_user_elf` is the remaining gap.
- **x86-64 CPL=3 user mode** — syscall path present in tree but never wired into task or component execution. ARM64 has the full per-task TTBR0 + ASID + EL0 path; x86-64 lags.
- **Component marketplace / registry / signing** — tracked as #41 ("Component Marketplace — signing, distribution"). Not started.
- **Persistence across reboot** — no component state survives reboot. Each boot re-registers components from in-kernel tables.
- **Component crash recovery** — hot-swap is for planned replacement, not crash recovery. A panicking component takes down the kernel today.

## See also

- `docs/components.md` (narrative)
- `docs/component-development.md` (how to write one)
- `docs/component-isolation.md` (isolation model — current + planned)
- `kernel/src/component_runtime.c`
- Issues: #41 (Marketplace)

*Last updated: 8 May 2026*
