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
| Network commands (`net`, `ping`, `ifconfig`, `netstat`, `tcpsh`) | ✅ | ✅ | ❌ no NIC | ✅ |
| Lua (`lua`, `lua -e`, `lua <file>`) | ✅ | ✅ | ✅ | ✅ |
| Scheduler (`sched`, `sched policy`, `sched stats`) | ✅ | ✅ | ✅ | ✅ |
| Diagnostic (`dtb`, `timdiag`, `peek`, `macbdiag`) | ✅ | ✅ | ✅ | 🟡 (`timdiag` ARM-only) |
| GPU/hw shell (`gpu`, `nvgpu phase-N`) | — | — | ✅ | ✅ |
| Multi-session TCP shell (port 2323) | ✅ | ✅ | ❌ no NIC | ✅ |
| Telnet protocol | ⏸️ | ⏸️ | ⏸️ | ⏸️ |
| SSH (#199) | ⏸️ | ⏸️ | ⏸️ | ⏸️ |

## Skipped / Blocked

- **`telnetd` daemon control surface (§3 of multi-session plan)** — planned (`telnetd start/stop/status/sessions/kick`, `/etc/telnetd.conf`). Not yet implemented.
- **SSH** (#199) — deferred until wolfSSH integration; out of current scope.
- **Jetson multi-session shell** — blocked on Jetson networking (#25 / #266). Single-session UARTC console works fine.
- **Command completion / history / arrow-key editing** — not implemented. Raw-line mode only. Would require telnet IAC negotiation for server-side echo.
- **`timdiag fiq` on Jetson** — disabled, known to crash EL3 handler.
- **Shell from a secondary CPU** — shell task is pinned to CPU 0. Secondary CPUs do not print (the UART lock on Pi 5/Jetson is IRQ-disable-only, deliberately not cross-CPU-safe).

## See also

- `docs/shell.md` (narrative)
- `docs/multi-session-shell-plan.md` (TCP shell + telnet + telnetd)
- `docs/demo-readiness-backlog.md` (observability backlog — closed items)
- Issues: #191-#196 (observability tickets), #199 (SSH)

*Last updated: 18 April 2026*
