# SLM-OS Fact Sheets

Tight per-capability cross-platform reference. Each row in the matrix
below links to a drill-down sheet covering that capability's major
sub-features across the four supported platforms, including anything
that was attempted but blocked.

**Not** a status report — these are stable facts about the codebase.
For delivery narrative, see `docs/archive/plans/capstone-feature-status.md`.

**Platforms:**

| Abbrev | Platform |
|---|---|
| QEMU | `QEMU_VIRT` (ARM64 Cortex-A76, 4 cores, GICv2/v3, PL011 UART) |
| Pi 5 | `RASPI5` — Raspberry Pi 5 (BCM2712, 4× Cortex-A76, PL011 via RP1 + MACB) |
| Jetson | `JETSON_ORIN_NANO` — Orin Nano Dev Kit (Tegra234, 6× Cortex-A78AE dual-cluster) |
| x86-64 | `X86_64` — QEMU q35 + bare-metal dev PC (i7-6700, RTX 3050) |

**Legend:**

| Symbol | Meaning |
|---|---|
| ✅ | Present, working, tested |
| 🟡 | Partial — works with caveats documented in the drill-down |
| ❌ | Blocked — tried and hit a blocker; see drill-down's "Skipped / Blocked" |
| ⏸️ | Deferred — not attempted, planned for future work |
| — | Not applicable on this platform |

---

## Top-level capability matrix

| Capability | QEMU | Pi 5 | Jetson | x86-64 | Drill-down |
|---|---|---|---|---|---|
| Boot to shell | ✅ | ✅ | ✅ | ✅ | [boot.md](boot.md) |
| SMP | ✅ 4 cores | ✅ 4 cores | ✅ 6 cores | ✅ 8 cores | [smp.md](smp.md) |
| Memory management | ✅ | ✅ | ✅ | ✅ | [memory.md](memory.md) |
| Preemptive multitasking | ✅ HW | 🟡 cooperative | 🟡 cooperative | ✅ HW | [preemption.md](preemption.md) |
| Networking | ✅ DHCP + ping | ✅ DHCP + ping | ✅ DHCP + ping (USB CDC-ECM) | ✅ DHCP + ping | [networking.md](networking.md) |
| GPU / accelerator inference | — | ✅ Hailo-8 NPU | ✅ | ✅ | [gpu-inference.md](gpu-inference.md) |
| Storage / filesystem | ✅ RAM disk + LittleFS | ✅ | ✅ | ✅ | [storage.md](storage.md) |
| Shell / observability | ✅ | ✅ | ✅ | ✅ | [shell.md](shell.md) |
| AI task scheduler | ✅ NEON | ✅ NEON | ✅ NEON | ✅ SSE | [ai-scheduler.md](ai-scheduler.md) |
| AI page eviction | ✅ | ✅ | ✅ | ✅ | [ai-eviction.md](ai-eviction.md) |
| Runtime model blob formats | ✅ | ✅ | ✅ | ✅ | [runtime-blob-formats.md](runtime-blob-formats.md) |
| Device-local file contract | ✅ | ✅ | ✅ | ✅ | [device-file-contract.md](device-file-contract.md) |
| IPC (message router) | ✅ | ✅ | ✅ | ✅ | [ipc.md](ipc.md) |
| Components (hot-swap) | ✅ | ✅ | ✅ | 🟡 no EL0 yet | [components.md](components.md) |
| Lua scripting | ✅ 5.4.7 | ✅ | ✅ | ✅ | [lua.md](lua.md) |
| Multi-session shell (TCP + telnet + telnetd) | ✅ | ✅ | ✅ (USB CDC-ECM) | ✅ | [shell.md](shell.md) |
| Testing / CI | ✅ full | 🟡 HW-in-loop | 🟡 HW-in-loop | ✅ QEMU + HW | [testing.md](testing.md) |

---

## How to use these sheets

- **Answer "does X work on platform Y?"** — scan the relevant drill-down's matrix.
- **Answer "why doesn't X work on platform Y?"** — read the drill-down's "Skipped / Blocked" section.
- **Update a sheet when delivering code** — every merged PR that adds/removes a capability updates the affected sheet in the same commit.
- **Add a new sheet** when introducing a new top-level capability. Link it from this README.

*Last updated: 27 April 2026*
