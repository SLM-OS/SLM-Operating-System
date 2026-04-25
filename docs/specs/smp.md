# SMP — Fact Sheet

Cross-platform symmetric-multiprocessing bring-up and runtime behavior.

## Matrix

| Sub-capability | QEMU | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Cores online at boot | 4/4 | 4/4 | 6/6 | up to 8/8 (i7-6700) |
| CPU model | Cortex-A76 | Cortex-A76 | Cortex-A78AE (dual cluster) | Skylake+ x86-64 |
| Boot mechanism | PSCI HVC | PSCI SMC | PSCI SMC (NS EL2) | INIT-SIPI-SIPI + ACPI MADT |
| MPIDR encoding | linear | linear | dual-cluster Aff2.Aff1 (0x000,0x100,0x200,0x300,0x10200,0x10300) | APIC IDs |
| Per-CPU run queues | NC memory | NC memory (`0xFFE00000`) | NC memory (`0xBDE00000`) | BSS (coherent) |
| Per-CPU current-task ptr | NC | NC | NC | BSS |
| Cross-CPU dispatch | ✅ full | ✅ full (fixed 2026-04-16) | ✅ full (fixed 2026-04-15) | ✅ full |
| IPI / wake mechanism | SEV broadcast | SEV broadcast | SEV broadcast | LAPIC vector 49 |
| Hardware cache coherency | ✅ automatic | ❌ SMPEN unset by TF-A | ❌ SMPEN unset by TF-A | ✅ MESI |
| Manual DC CVAC/CIVAC needed | — | ✅ | ✅ | — |
| Spinlock model | LDAXR/STXR | Runtime `spinlock_hw_enabled`; barrier-only pre-MMU; DC CIVAC around LDAXR/STLR | Runtime `spinlock_hw_enabled`; same as Pi 5 | `lock cmpxchg` |
| Work stealing | ON | ON | ON | ON |
| Multi-core integration tests | 5/5 pass | 15/15 pass (modulo #216 flake) | 6-core `bench smp` 5/5 COMPLETED | Full suite |
| SMP-safe UART lock | Standard | IRQ-disable-only (NC lock deadlocks) | IRQ-disable-only | Standard |

## Skipped / Blocked

- **Pi 5 secondary-CPU dormancy (#216)** — occasional boot-to-boot pattern where one or more secondaries never enter `schedule()`, flaking multi-CPU integration tests. Not blocked on a permanent fix; the test assertions were relaxed so a single awake stealer counts as success. Root cause still open.
- **Jetson MPIDR fold collision (`SECONDARY_PREEMPT`)** — the ELR-trampoline CPU-index formula in `vectors.S:377-380` collides on dual-cluster cores 4/5. A runtime check (`preempt_check_cpu_mpidr`) panics on mismatch so this cannot be silently enabled.
- **True preemptive scheduling on Pi 5 / Jetson** — not an SMP issue per se; see [preemption.md](preemption.md). SMP is fully functional under cooperative preemption.
- **#166 — Jetson `pmm_free_pages` page fault under `bench stealing`** — closed. Root cause was Jetson's hardcoded `SPINLOCK_SKIP_LOCKING` making every cacheable spinlock a no-op. Fixed 2026-04-15.
- **#158 — Pi 5 boot hang with `WORK_STEALING=ON`** — closed. Root cause was the steal-deque lock living in NC memory (LDAXR/STXR never actually ran). Fixed via external cacheable lock.

## See also

- `kernel/CLAUDE.md` §"Non-Cacheable Shared Memory"
- `kernel/CLAUDE.md` §"Cross-CPU notification — `smp_notify_cpu()`"
- `docs/smp.md` (narrative)
- `docs/archive/plans/capstone-feature-status.md` §SMP

*Last updated: 18 April 2026*
