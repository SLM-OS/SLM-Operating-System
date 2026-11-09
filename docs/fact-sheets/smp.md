# SMP — Fact Sheet

Cross-platform symmetric-multiprocessing bring-up and runtime behavior.

## Matrix

| Sub-capability | QEMU | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Cores online at boot | 4/4 | 4/4 | 6/6 | up to 8/8 (i7-6700) |
| CPU model | Cortex-A76 | Cortex-A76 | Cortex-A78AE (dual cluster) | Skylake+ x86-64 |
| Boot mechanism | PSCI HVC | PSCI SMC | PSCI SMC (NS EL2) | INIT-SIPI-SIPI + ACPI MADT |
| MPIDR encoding | linear | linear | dual-cluster Aff2.Aff1 (0x000,0x100,0x200,0x300,0x10200,0x10300) | APIC IDs |
| Per-CPU run queues | BSS | BSS | BSS | BSS |
| Per-CPU current-task ptr | BSS | BSS | BSS | BSS |
| Cross-CPU dispatch | ✅ full | ✅ full | ✅ full | ✅ full |
| IPI / wake mechanism | SEV broadcast | SEV broadcast | SEV broadcast | LAPIC vector 49 |
| Hardware cache coherency | ✅ automatic | ✅ automatic (A76, no enable bit) | ✅ automatic (A78AE, no enable bit) | ✅ MESI |
| Manual DC CVAC/CIVAC needed | — | device DMA only | device DMA only | — |
| Spinlock model | LDAXR/STXR | Runtime `spinlock_hw_enabled`; barrier-only pre-MMU, LDAXR/STXR/STLR after | Runtime `spinlock_hw_enabled`; same as Pi 5 | `lock cmpxchg` |
| Work stealing | ON | ON | ON | ON |
| Multi-core integration tests | 5/5 pass | 15/15 pass (modulo #216 dormancy flake) | 6-core `bench smp` 5/5 | Full suite |
| SMP-safe UART lock | IRQ-saving spinlock | IRQ-saving spinlock | IRQ-saving spinlock | IRQ-saving spinlock |
| CPU 0 idle behavior | WFI (timer wakes) | WFI (timer wakes) | WFI (HW tick default, timer wakes) / spin-yield under `JETSON_HW_TICK=OFF` | HLT |
| Secondary-CPU idle behavior | WFI | WFI under HW preempt opt-in / WFE under default coop | WFI under HW tick default / WFE under `JETSON_HW_TICK=OFF` | HLT |

## Skipped / Blocked

- **Pi 5 secondary-CPU dormancy (#216)** — occasional boot-to-boot
  pattern where one or more secondaries never enter `schedule()`,
  flaking multi-CPU integration tests. Test assertions were relaxed
  so a single awake stealer counts as success; root cause still open.

## See also

- `docs/adr/0006-cross-cpu-coherency.md` (#1193: why shared state is cacheable)
- `kernel/CLAUDE.md` §"Cross-CPU Coherency"
- `kernel/CLAUDE.md` §"Cross-CPU notification — `smp_notify_cpu()`"
- `docs/smp.md` (narrative)

*Last updated: 8 November 2026*
