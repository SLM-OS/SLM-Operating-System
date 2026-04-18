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
| EL0 isolation (user mode) | 🟡 infrastructure present, Phase-5 M4 exercised | 🟡 not exercised on HW | 🟡 not exercised on HW | ❌ syscalls present, EL0/CPL=3 not wired |
| Per-component address space | ❌ (single AS today) | ❌ | ❌ | ❌ |
| Shell surface | `component list/run/swap/register/status` | Same | Same | Same (run/swap work; no EL0 components) |
| Lua bindings | `slm.component_run`, `component_swap`, `component_list` | Same | Same | Same |
| Message-router integration | Components subscribe/publish to topics | Same | Same | Same |
| Runtime monitoring | State tracked in `struct component`; visible via `top` and `component list` | Same | Same | Same |
| Hot-swap demo (Lua) | `slmos> lua /mnt/files/demo.lua` | Same | Same | Same |

## Skipped / Blocked

- **Full EL0 (user-mode) isolation on real hardware** — `kernel/src/component_runtime.c` has syscall infrastructure and EL0 paths, exercised only on QEMU under Phase-5 M4. Not verified on Pi 5 or Jetson hardware.
- **x86-64 CPL=3 user mode** — syscall path present in tree but never wired into component execution. EL0-equivalent isolation remains future work.
- **Per-component TTBR0_EL1 / CR3 switching** — no per-component page tables today. All components share the kernel address space.
- **Component marketplace / registry / signing** — tracked as #41 ("Component Marketplace — signing, distribution"). Not started.
- **Persistence across reboot** — no component state survives reboot. Each boot re-registers components from in-kernel tables.
- **Component crash recovery** — hot-swap is for planned replacement, not crash recovery. A panicking component takes down the kernel today.

## See also

- `docs/components.md` (narrative)
- `docs/component-development.md` (how to write one)
- `docs/component-isolation.md` (isolation model — current + planned)
- `kernel/src/component_runtime.c`
- Issues: #41 (Marketplace)

*Last updated: 18 April 2026*
