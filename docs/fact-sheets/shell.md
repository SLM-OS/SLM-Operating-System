# Shell / Observability — Fact Sheet

Interactive shell, command surface, observability commands, multi-session.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Shell task | Kernel task on CPU 0 | Same | Same | Same |
| Console I/O | UART PL011 | UART via RP1 (polled) | UART via UARTC + TCU HSP RX | 16550 COM1 |
| Prompt | `slmos>` | Same | Same | Same |
| Command count | ~50 | Same | Same | Same |
| Dispatch table | `builtin_commands[]` | Same | Same | Same |
| Help / help `<cmd>` | ✅ | ✅ | ✅ | ✅ |
| System info (`mem`, `tasks`, `cpu`, `uptime`, `vmm`, `ipc`) | ✅ | ✅ | ✅ | ✅ |
| Benchmarks (`bench context`, `bench irq`, `bench ipc`, `bench smp`, `bench stealing`, `bench all`) | ✅ | ✅ | ✅ | ✅ |
| Latency histograms (#196) | ✅ | ✅ | ✅ | ✅ |
| `top`-like live display (#191) | ✅ | ✅ | ✅ | ✅ |
| `sched trace` (#192) | ✅ | ✅ | ✅ | ✅ |
| Model commands (`model load/list/info/infer/bench/pools/stats/gpu/unload`) | ✅ | ✅ | ✅ | ✅ |
| Filesystem commands | ✅ | ✅ | ✅ | ✅ |
| Component commands (`component list/run/swap/register/status`) | ✅ | ✅ | ✅ | 🟡 no EL0 |
| Message router (`msg send/list/subscribe`) | ✅ | ✅ | ✅ | ✅ |
| Network commands (`net`, `ping`, `ifconfig`, `netstat`, `tcpsh`) | ✅ | ✅ | ✅ (USB CDC-ECM) | ✅ |
| Lua (`lua`, `lua -e`, `lua <file>`) | ✅ | ✅ | ✅ | ✅ |
| Scheduler (`sched`, `sched policy`, `sched stats`) | ✅ | ✅ | ✅ | ✅ |
| Diagnostic (`dtb`, `timdiag`, `peek`, `macbdiag`, `pmu`) | ✅ (`pmu` skips event values under TCG) | ✅ | ✅ | 🟡 (`timdiag` / `pmu` ARM-only — `pmu` x86 sibling tracked as #870) |
| Kernel-replace admin (`kernel status/stage/activate/promote/rollback`) | 🟡 stub (no boot media) | ✅ tryboot-driven | 🟡 stub | 🟡 stub |
| GPU/hw shell (`gpu`, `nvgpu phase-N`) | — | — | ✅ | ✅ |
| Multi-session TCP shell (port 2323) | ✅ | ✅ | ✅ (USB CDC-ECM) | ✅ |
| Telnet protocol (IAC, ECHO, SGA, NAWS, TERMINAL-TYPE, IAC IP→Ctrl+C) | ✅ | ✅ | ✅ (USB CDC-ECM) | ✅ |
| Up/down arrow command history (#434, 32 × 128 B per session) | ✅ | ✅ | ✅ | ✅ |
| `telnetd` daemon controls (`start/stop/status/sessions/kick`) | ✅ | ✅ | ✅ (USB CDC-ECM) | ✅ |
| `/etc/telnetd.conf` + `NET_TELNETD_AUTOSTART` | ✅ | ✅ | ✅ (USB CDC-ECM) | ✅ |
| `slm.telnetd_*` Lua bindings | ✅ | ✅ | ✅ (USB CDC-ECM) | ✅ |
| SSH (#199) | ⏸️ | ⏸️ | ⏸️ | ⏸️ |

## Skipped / Blocked

- **Unauthenticated telnet on hardware** — Pi 5 and Jetson lab/demo builds default `NET_TELNETD_AUTOSTART=ON`, but an explicit `-DNET_TELNETD_AUTOSTART=OFF` still wins. Lua sessions opened by telnet land on the safe binding surface (`lua_slm_newstate()`), so admin mutators (`slm.component_run`, `slm.model_load`, `slm.sched_set_policy`, `slm.task_create`, `slm.shell_exec`, `slm.hailo.*`, …) are unreachable from a remote session. That is acceptable only on trusted networks; SSH/authentication (#199) is still the real security boundary, and observability bindings remain visible.
- **SSH** (#199) — deferred until wolfSSH integration; out of current scope.
- **Jetson multi-session shell over internal Ethernet** — still blocked on #25 (RTL8168 behind PCIe C8). The shipped Jetson networking path is USB CDC-ECM over the retained XHCI root-hub handoff, which carries the multi-session TCP shell + telnet + telnetd controls. UARTC remains the local single-session console.
- **Command completion** — not implemented. Tab completion (#TBD) is out of scope for the current shell.
- **Reverse search (Ctrl-R), prefix search, history expansion (`!!` / `!N`), persistent history across reboot** — out of scope for #434; tracked separately if/when needed. Up/down arrow recall + in-place edit of the recalled line is the implemented surface.
- **Left/right arrow in-line cursor movement** — not implemented. Backspace-from-end is the only in-line edit operation; the line-edit loop in `shell_read_command` deliberately drops the parameter byte for unknown CSI sequences and lets the trailing byte fall through to the data path (an unrecognized arrow ends up echoing the bare letter).
- **`timdiag fiq` on Jetson** — disabled, known to crash EL3 handler.
- **Shell from a secondary CPU** — shell task is pinned to CPU 0. Secondary CPUs do not print (the UART lock on Pi 5/Jetson is IRQ-disable-only, deliberately not cross-CPU-safe).

## See also

- `docs/shell.md` (narrative)
- `docs/dynamic-kernel-replace-plan.md` (`kernel` admin command surface)

*Last updated: 28 April 2026*
