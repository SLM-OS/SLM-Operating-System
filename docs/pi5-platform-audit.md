# Pi 5 Platform Audit — Functional Gaps and Unused Hardware

Platform-neutral audit of SLM-OS's current state on Raspberry Pi 5 versus the other supported platforms (QEMU_VIRT ARM64, JETSON_ORIN_NANO ARM64, X86_64), plus a catalog of Pi 5 hardware capabilities the OS does not currently exploit.

This report is strictly a current-state inventory. It does not prioritize work items, propose a roadmap, or weigh findings against the capstone deliverables.

**Date:** 2026-04-13
**Source basis:** files under `docs/`, `kernel/`, `runtime/`, `../slmos-reference-cache/`, and GitHub issues tagged `platform:pi5`, `platform:jetson`, `platform:x86-64`, `platform:qemu`.

---

## Part A — Functional Gaps on Pi 5 vs. Other Platforms

| Subsystem | Other Platforms | Pi 5 Today | Tracking | Source |
|---|---|---|---|---|
| Timer IRQ delivery | QEMU: working on all CPUs. Jetson: working on CPU 0 (same secondary-CPU limitation as Pi 5). x86-64: APIC-based, working | Hardware IRQs not delivered (TF-A/GIC block that SLM-OS cannot change from EL1/EL2). Replaced by **cooperative preemption** (`PI5_COOP_PREEMPT`): `schedule()` synthesizes `scheduler_tick()` from `CNTPCT_EL0` every 10 ms. All observability (pit_ticks, timer_handler_count, AI policy tick) advances. | #99 resolved by coop-preempt; see `docs/archive/investigations/pi5-preemption-resolution.md` | `docs/archive/investigations/pi5-preemption-resolution.md`, `docs/pi5-irq-investigation-2026-04.md` |
| Secondary-CPU preemption | QEMU: timer-driven preemption on all 4 CPUs. Jetson: blocked by the same `switch_to`-in-ISR root cause. x86-64: working | Cooperative — each CPU's `schedule()` drives its own tick under `PI5_COOP_PREEMPT`. ELR-trampoline infra (PR #98) inert under coop-preempt; retained for future hardware-IRQ restoration. | #57 (infra); #99 resolved | `docs/archive/investigations/pi5-preemption-resolution.md` |
| UART RX (console input) | QEMU: virtio-console. Jetson: UARTC via TCU. x86-64: 16550 | PL011 via RP1 PCIe; polling fallback works; MSI-X interrupt path implemented but `irq_count=0` in the field | Tied to #99 | `docs/pi5-uart-irq-investigation.md` |
| GPU driver | QEMU: stub. Jetson: NVIDIA probe driver + runtime-PM kexec path, GSP firmware load deferred. x86-64: stub (RTX 3050 target) | Stub only; VideoCore VII unused | — | `docs/gpu.md`, `kernel/gpu/gpu_stub.c` |
| Persistent storage (block device) | QEMU: 1 MB RAM disk + LittleFS. Jetson: same RAM disk | Same RAM disk (nothing else) | #35 | `kernel/drivers/ramdisk.c`, issue #35 |
| Networking | QEMU ARM64: virtio-mmio + lwIP + auto-DHCP. x86-64 QEMU: virtio-pci + lwIP + auto-DHCP. Pi 5 hardware: Cadence MACB/GEM via RP1, DHCP + ping verified on pi-5-1 at 1-5 ms RTT. Jetson: none — #25 EQOS driver pending. | Cadence MACB driver runs **polled**: IRQ wiring is installed (GIC SPI 134 via MIP0 vec 6, handler registered, MACB_IER unmasked) but RP1's MSIX_CFG engine doesn't fire TLPs on peripheral IRQ assertion (confirmed: MIP0 INTSTATL sees the MAC asserting, but no TLP reaches the GIC). Same blocker as UART RX — tracked in #247. `macbdiag` shell command surfaces the live state. | #202 (closed), #25, #247 | `kernel/drivers/macb.c`, `kernel/drivers/virtio_net.c`, `kernel/drivers/virtio_net_pci.c`, `docs/networking.md` §"Cadence MACB/GEM Driver (Pi 5)" |
| User-mode (EL0) components | QEMU: syscall infrastructure and EL0 tasks exercised in the Phase-5 M4 work. x86-64: syscall infra present, EL0 not wired | Syscall infrastructure present in tree but EL0 paths not exercised on real hardware | — | `docs/component-isolation.md`, `docs/future-work.md` |
| Boot / EL drop | QEMU: linker loads at EL1 directly. Jetson: kexec from Linux, EL2 + VHE working. x86-64: multiboot2 via GRUB | `armstub8-2712.bin` disabled due to a ~60% boot-garble rate; GIC Group 1 configuration moved into `boot.S` from EL2 (incompletely — this is a suspected cause of the #99 IRQ-delivery failure) | #99 | `docs/pi5-baremetal-status.md` §"Known limitations" |
| Release-mode build | Green on all platforms since 37c4dc9 | Green, but `-mcpu=cortex-a76` lets the compiler emit LSE atomics (SWPALB / CASALB) that BCM2712 does not execute reliably in all contexts | — | `CMakeLists.txt:34-47`, commit 37c4dc9 |
| Multi-core integration tests | QEMU: all 5 multi-core tests pass. Jetson: 6-core boot and `bench smp` cross-dispatch documented | 5 tests previously marked ignored; they complete cooperatively via WFE/SEV but cannot demonstrate mid-task preemption | #57 / #99 | `kernel/tests/test_integration.c`, `docs/archive/investigations/pi5-secondary-cpu-preemption.md` |
| IPC ack timeouts (Rust runtime) | QEMU / Jetson: `pit_ticks` advances via idle timer preemption, so `msg_router_publish` ack waits work | Historically broken for the same reason as #99 (`pit_ticks` never advances). Fixed in-band by switching the Rust router to `CNTPCT_EL0` via `timer_get_count()` | #80 (closed, merged in PR #102) | `runtime/src/msg_router.rs`, issue #80 |

### Note on Jetson
Several rows above qualify Jetson as "working on CPU 0 only" for preemption. Jetson and Pi 5 share the same `switch_to`-inside-exception-handler root cause; neither currently delivers timer-driven preemption to secondary CPUs. Jetson brings up more cores (6 vs. 4 via PSCI) but uses the same cooperative WFE/SEV dispatch model on secondaries.

---

## Part B — Pi 5 Hardware Features Not Exploited

The following are Pi 5 / BCM2712 capabilities observable in project documentation, reference files under `../slmos-reference-cache/`, or the `cortex-a76` architecture implied by `CMakeLists.txt`. Current SLM-OS usage is recorded alongside a brief description of the generic OS-level utility each would provide on this SoC.

| Feature | Evidence | Current SLM-OS usage | Generic OS utility |
|---|---|---|---|
| RP1 PCIe south-bridge | `../slmos-reference-cache/linux/linux-rpi-mfd-rp1.c`, `linux-rpi-dt-bindings-mfd-rp1.h`; BAR0 at `0x1F00000000`, BAR3 MSI-X routing working for PL011 | UART0 (PL011) driven via BAR0. Other RP1 blocks dormant | Most Pi 5 peripheral I/O sits behind RP1 — GPIO banks, I²C, SPI, USB 2.0 / 3.0, Ethernet, SD/eMMC |
| VideoCore VII GPU | Boot output references the GPU / firmware; DRAM carve-out for VideoCore | `kernel/gpu/gpu_stub.c` only | GPU compute offload, display output, video decode |
| ARM v8.2 crypto extensions (AES, SHA-1, SHA-2, PMULL) | `-mcpu=cortex-a76` enables them. `kernel/include/spinlock.h` gates LSE atomics at runtime by a similar mechanism | Unused | Block cipher, integrity digest, TLS / MAC offload |
| ARM v8.2 NEON / SIMD | Cortex-A76 mandates NEON; `runtime/src/inference/ops.rs` references SIMD tiling | `matmul_tiled` still scalar per #71 | Inference, vector math, accelerated `memcpy` / `memset` |
| Cortex-A76 PMU counters | Cortex-A76 TRM (not in tree) documents the PMU | Not accessed | Cycle-accurate profiling, hot-path discovery, scheduler feedback |
| DSU (DynamIQ Shared Unit) features | `kernel/CLAUDE.md` "Pi 5 cache coherency" discusses DSU-level behavior | Non-cacheable memory workaround for per-core L2 incoherency; DSU features otherwise unused | L3 partitioning, snoop-filter control, prefetch hints |
| PSCI (beyond CPU_ON) | `docs/pi5-baremetal-status.md` notes PSCI CPU_ON used for SMP bring-up | `CPU_ON` only | `SYSTEM_RESET`, `SYSTEM_OFF`, `CPU_OFF`, `CPU_SUSPEND` for power management and graceful shutdown |
| EL2 and VHE | Jetson runs at EL2 + VHE. Pi 5 currently boots to EL1 after the armstub was disabled | Pi 5 runs at EL1 only | Hypervisor, nested translation, single-VA hosting of kernel + user mappings |
| PCIe root port (external connector) | Pi 5 exposes a 1-lane PCIe 2.0 external connector targeting NVMe HATs | No enumeration, no driver | NVMe storage, 10 GbE NIC, accelerator boards |
| BCM2712 DMA controllers | Broadcom DMA engines inherited from earlier Pi generations | Unused | Offloaded bulk transfers, scatter-gather I/O |
| Thermal / voltage monitoring (PMIC) | Pi 5 PMIC is documented upstream | Unused | Over-temperature throttling, power budgeting, orderly shutdown |
| Hardware RNG | BCM2712 retains Pi-lineage HWRNG peripheral | Unused (no entropy source in tree) | Crypto seeding, ASLR, randomized task IDs |
| Watchdog | BCM2712 retains the Pi watchdog interface | Unused | Self-reset on hang, deadman for long-running inference |
| RP1 GPIO (beyond the ACT LED) | `kernel/drivers/uart_rp1.c` demonstrates BAR0 access | GPIO 2 (ACT LED) only | Buttons, sensors, external peripherals, debug headers |
| USB 2.0 / 3.0 | RP1 hosts a standard xHCI / EHCI controller | Unused | Keyboard / mouse / storage / networking via USB |

### Caveats for Part B

- Where a feature is not named in tree documentation (HWRNG, watchdog, DMA, PMIC), the entry describes a generic Pi 5 capability rather than a tree-verified one. These should be read as "the SoC documentation lists this" rather than "the SLM-OS tree references it".
- Status claims for Jetson and x86-64 are derived from `kernel/CLAUDE.md`, `docs/archive/investigations/jetson-nvidia-support.md`, and `docs/x86-64-*.md`. No fresh deployment verification was performed against those platforms for this audit.
- Where this audit contradicts older status documents (for example, `docs/pi5-baremetal-status.md` claiming "preemptive scheduling active"), the audit reflects measurements taken in the session that filed issue #99; see that issue for the evidence.

---

*Last updated: 2026-04-13.*
