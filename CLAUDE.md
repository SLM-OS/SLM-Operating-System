# Claude Code Notes for SLM-OS Project

Project-wide notes and reminders. See also:
- `kernel/CLAUDE.md` — Kernel-specific (C, assembly)
- `runtime/CLAUDE.md` — Runtime-specific (Rust)

---

## Documentation

Ignore files under `docs/archive/` unless specifically told to look at them. Archived documents are outdated and may contradict current state.

---

## Build, Deploy, and Test

Use the `/deploy-and-test` skill (`.claude/skills/deploy-and-test/SKILL.md`) when building, deploying to Pi 5 or Jetson hardware, and capturing boot/test output. This skill orchestrates the labctl MCP tools (sdwire_update, power_cycle, serial_capture, boot_test) for the full deploy cycle.

For iterative hardware debugging, use the labctl MCP tools directly:
- `sdwire_update` — flash kernel + rename/delete files on SD card
- `boot_test` — automated multi-boot reliability testing
- `serial_capture` — capture boot output with pattern matching
- `serial_send` — send commands to the shell and capture responses

---

## Git Commits

Always ask for permission before committing code. Do not automatically commit changes after completing a task.

---

## Formatting Issues

### ASCII Box Diagrams

**Issue:** When creating ASCII art boxes with text, sometimes an extra space is added on lines with text, causing misalignment with the box borders.

**Example of correct formatting:**
```
┌─────────────────────────────────────────────────────────────────────┐
│  EL3 - Secure Monitor                                    (Highest)  │
│  - Only level that can switch security states                       │
├─────────────────────────────────────────────────────────────────────┤
│  EL2 - Hypervisor                                                   │
└─────────────────────────────────────────────────────────────────────┘
```

**Watch for:** Inconsistent spacing between the `│` border and the text content. All lines within a box should have consistent left padding.

### TODO File Formatting

Use emoji markers for task status:

| Status | Marker | Example |
|--------|--------|---------|
| Completed | ✅ | `- ✅ Task completed` |
| Pending | ☐ | `- ☐ Task pending` |
| Blocked | 🔗 | `- ☐🔗 Task — requires M4` |
| Deferred | ⏸️ | `- ⏸️ Task — deferred to Phase 4` |
| Tracked in issue | 🎫 | `- ☐🎫 Task — #42` |

**Definitions:**
- **Pending** ☐ — Ready to work on now
- **Blocked** 🔗 — Waiting on dependency within this phase (combine with ☐ or ⏸️)
- **Deferred** ⏸️ — Postponed to a future phase (e.g., "deferred to Phase 5")
- **Tracked in issue** 🎫 — Covered by an open GitHub issue; append `— #N` with the issue number (combine with ☐/⏸️/🔗). For documents that describe a whole feature in one section (like `FUTURE.md`), put `**Tracking:** 🎫 #N` at the section level instead of on every bullet.

**Correct:**
```markdown
- ✅ Task completed
- ☐ Task pending
- ☐🔗 Task — requires M4 (pending, has dependency)
- ⏸️ Task — deferred to Phase 5
- ⏸️🔗 Task — deferred, had dependency when deferred
- ☐🎫 Task — #42 (pending, GitHub issue opened)
- ☐🔗🎫 Task — requires M4, tracked in #47
```

**Incorrect:**
```markdown
- [x] Task completed  ← Don't use this
- [ ] Task pending    ← Use ☐ instead
- ✓ Task completed    ← Don't use plain check symbol
- Deferred: Task      ← Use ⏸️ emoji instead
```

---

## Writing Style

### Avoid Second Person

Documentation will be submitted to an academic advisor. Avoid "you/your" language.

**Instead of:** "Your code runs here"
**Use:** "SLM-OS code runs here"

**Instead of:** "You must set the stack pointer"
**Use:** "The stack pointer must be set by boot code"

---

## Build System

### Prerequisites

- ARM GNU Toolchain (aarch64-none-elf-gcc) — for ARM64 platforms
- GCC (x86_64-linux-gnu) — for x86-64 platform
- GNU make
- CMake 3.20+
- QEMU (qemu-system-aarch64 for ARM, qemu-system-x86_64 for x86)
- grub-mkrescue — for x86-64 ISO creation (test and boot)
- clang-tidy — for `make check-braces`
- Rust toolchain with `aarch64-unknown-none` and `x86_64-unknown-none` targets — pinned by `rust-toolchain.toml` (rustup installs it automatically on first build)

### Build Commands

```bash
# Standard build targets (from project root):
make kernel          # Build kernel (default: QEMU_VIRT ARM64)
make kernel-clean    # Clean kernel build
make run             # Build and run in QEMU
make debug           # Build and run with GDB server
make test            # Build test kernel and run in QEMU
make check-braces    # clang-tidy brace rule on changed C lines, all platforms
                     #   (BRACES_BASE=<ref>; BRACES_ARGS=--all for the whole tree)

# x86-64 platform:
make kernel PLATFORM=X86_64    # Build for x86-64 (bare-metal, linked at 0x100000)
make test PLATFORM=X86_64      # Run x86-64 tests (creates GRUB ISO, uses isa-debug-exit)
make run PLATFORM=X86_64       # Run in QEMU (uses q35 machine, multiboot2 via ISO)

# x86-64 kexec paths (Linux→SLM-OS handoff — for SEC2 unlock inheritance):
make kernel-kexec PLATFORM=X86_64     # Multiboot2 kexec build (linked at 0x20000000)
make kernel-bzimage PLATFORM=X86_64   # Linux-bzImage wrapper variant
make kexec-verify PLATFORM=X86_64     # 29-check structural validator for all 3 x86 builds
make kexec-deploy PLATFORM=X86_64     # scp ELF to test-pc + fire kexec (mb2 mode)
# See docs/x86-64-gpu-inference-status.md §4.2.k for kexec investigation trail.

# Other platforms:
make kernel PLATFORM=RASPI5            # Raspberry Pi 5 (AI_SCHED ON by default)
make kernel PLATFORM=JETSON_ORIN_NANO  # Jetson Orin Nano (HW timer preempt
                                       #   ON by default; pass JETSON_HW_TICK=OFF
                                       #   on stock-BL31 dev kits — see #755;
                                       #   AI_SCHED ON by default)

# AI scheduler (adds MLP/PPO policies; default ON for Pi 5 and Jetson,
# OFF for QEMU and x86-64; AI_SCHED=OFF turns it off):
make kernel AI_SCHED=ON                        # QEMU ARM64 with AI scheduler
make test AI_SCHED=ON                          # Run tests with AI scheduler
make kernel PLATFORM=X86_64 AI_SCHED=ON        # x86-64 with AI scheduler (SSE)

# Coherency gauntlet switch (#1193, Pi 5 and Jetson only; default ON = current behavior):
make kernel PLATFORM=RASPI5 SETWAY_INVAL=OFF   # no post-MMU set/way loop on secondaries
```

The Makefile automatically selects the correct toolchain, QEMU binary, and QEMU machine settings based on `PLATFORM`. For x86-64, the test target creates a bootable GRUB ISO and uses `isa-debug-exit` for clean test termination.

**QEMU safeguards:** `make test` wraps QEMU in `systemd-run --user --scope` with `MemoryMax=3G` (prevents OOM crashes) and `CPUQuota=200%` (prevents runaway busy-spin tests from pegging all host cores), plus `timeout 120` for automatic termination. These are defined in the Makefile as `QEMU_GUARD` and `TEST_TIMEOUT`.

**Always wrap direct QEMU invocations in `systemd-run` — `-m` alone is not sufficient to cap host memory.** On x86-64 TCG hosts the translation buffer (JIT code cache) defaults to ~1 GB and can grow further under heavy translation pressure. A CPU-bound test kernel (e.g. work-stealing benchmarks with many tasks and many iterations) has been observed to push the QEMU host process past **40 GB RSS** even with `-m 1G` — enough to OOM-kill unrelated processes and hang the machine. The `-m` flag only limits **guest** RAM; it does not limit the TCG code cache, device mappings, or QEMU's own heap.

Use the same wrapper `make test` uses:

```bash
systemd-run --user --scope -q \
    -p MemoryMax=3G -p CPUQuota=200% \
    timeout 120 \
    qemu-system-aarch64 -machine virt -cpu cortex-a76 \
    -smp cores=4 -m 1G -nographic -semihosting \
    -kernel build/kernel-test/slmos.elf
```

`MemoryMax=3G` caps total process memory (the cgroup OOM-kills on overrun, producing exit code 137). `CPUQuota=200%` stops runaway busy loops from pegging every host core. `timeout 120` provides a wall-clock ceiling. **Always prefer the Makefile targets (`make test`, `make run`, `make debug`) which already apply all three guards** — hand-crafted QEMU commands are reserved for reproducing specific scenarios the Makefile can't express.

Caps to apply if you must run QEMU directly:

- `-m 1G` (ARM64 and x86-64) — guest RAM. Match `QEMU_MEMORY` in the Makefile.
- `-accel tcg,tb-size=128` — cap the TCG translation buffer at 128 MB (the second line of defense if systemd-run is unavailable for some reason).
- The `systemd-run` wrapper above — the *only* reliable hard cap on total process memory.

If `make test` fails with exit code **137** and the log contains only `Killed`, the cgroup OOM-killed QEMU. Re-run under `ps aux | grep qemu-system` if it seems stuck; any QEMU process using more than a few GB RSS should be killed immediately (`kill -TERM <pid>`; escalate to `-KILL` if needed).

### Clean Build Targets

Always use the Makefile's clean targets instead of manual `rm -rf`:

```bash
make kernel-clean            # Clean kernel build directory (build/kernel)
make kernel-test-clean       # Clean test kernel build directory (build/kernel-test)
```

When switching platforms or after significant changes, a clean rebuild ensures no stale objects:
```bash
make kernel-clean && make kernel PLATFORM=JETSON_ORIN_NANO
```

### Common Build Issues

1. **"Permission denied" during link**
   - Usually caused by stale QEMU process holding a lock on `slmos.elf`
   - **First step:** Look for running QEMU processes and kill them
   - Note: `ps -eaf | grep qemu` may fail — grep complains about "binary input" and misses processes. Use `tasklist.exe | grep -i qemu` or Windows Task Manager instead.
   - The Makefile's `check-build-dir` target tries to detect this, but may not catch all cases
   - Manual fix: Kill QEMU processes, then `make kernel-clean` and rebuild

---

## Jetson Hardware

**Board:** Jetson Orin Nano Super Developer Kit

**Status:** 🟢 Working — Boots to shell on both nano-1 and nano-2 at EL2 + VHE via UARTC

### Known Issues

**Spinlock / LSE Atomics (Solved):** On Cortex-A78AE, ARM LSE atomics (SWPALB) and exclusive operations (LDAXR/STXR) cause a Synchronous External Abort on pre-MMU, non-cacheable memory. Jetson originally defined `SPINLOCK_SKIP_LOCKING` unconditionally in `platform.h`, making every cacheable spinlock a no-op even after SMP + MMU were live — this surfaced as issue #166 (a `pmm_free_pages` free-list page fault during `bench stealing` with WORK_STEALING=ON). Jetson now shares Pi 5's runtime model: the `spinlock_hw_enabled` flag stays 0 until `vmm_init` finishes, keeping spinlocks barrier-only pre-MMU, then flips to 1 so real LDAXR/STXR run post-MMU and provide cross-CPU mutual exclusion on cacheable memory.

**GPU CBB Firewall:** GPU registers at 0x17000000 are behind the CBB firewall. Reading NV_PMC_BOOT_0 triggers an external abort. The stub GPU driver is used on Jetson instead of the NVIDIA probe driver.

**nvgpu RAS Error (Solved):** Linux's nvgpu driver leaves stale GPU DMA operations after kexec. TF-A (EL3) catches the resulting RAS Uncorrectable Error and powers off the CPU core. **Fix:** Runtime PM suspend the GPU before kexec via the `slmos-kexec` helper (`scripts/jetson-kexec-slmos.sh`, installed to `/usr/local/bin/slmos-kexec` on both Jetsons). This power-gates the GPU and stops the PMU firmware, eliminating stale DMA. See GitHub issue #9.

**Stale Interrupts After Kexec (Solved):** After kexec, stale Linux timer/peripheral interrupts can fire before SLM-OS sets up exception vectors. Without a valid VBAR, any exception crashes the CPU. **Fix:** `boot.S` masks all exceptions (`msr daifset, #0xF`) as the first instruction at `primary_cpu`, before stack setup or BSS clear.

**UART LSR Read After Kexec (Solved):** After kexec, reading UARTC LSR (Line Status Register) without a DSB barrier returns stale data due to speculative MMIO access, causing TX FIFO overruns and garbled serial output. **Fix:** `uart_tegra.c` issues `dsb sy` before each LSR read.

**EL0 Syscalls Fault After Kexec (Solved, #1079):** A kexec from VHE Linux leaves SCTLR.SPAN=0 and PSTATE.PAN=1 on the boot CPU only, so every syscall reading a user pointer took a level-3 permission fault on CPU 0, where the shell runs. Secondaries and UEFI-direct boot were never affected. The measured `0x30401805` is `cpu_soft_restart`'s `INIT_SCTLR_EL1_MMU_OFF` (no SPAN) plus M, C and I; PAN=1 is carried over from Linux. **Fix:** the Jetson block in `boot.S` sets SPAN and clears PAN, so kernel code can read user pages directly, as on Pi 5. Hardening beyond that (copy_from_user, PAN on) is #29.

**CPU Cluster 1 Slow After Kexec (Solved, #1053; UEFI case #26):** Linux's kexec offlines every CPU but CPU 0, and an offlined core's NDIV (CPU PLL multiplier) drops to the 115.2 MHz floor. A Tegra234 cluster runs at its highest core's request, so cluster 0 keeps CPU 0's frequency while cluster 1 (CPUs 4-5) stayed at 115.2 MHz, ~12x slower. **Fix:** `tegra234_cpufreq_sync_to_boot_cpu()` (`kernel/drivers/tegra_cpufreq.c`, called from `smp_init`) moves every core to CPU 0's NDIV — up after kexec, and down after UEFI-direct boot, where PSCI-booted cores request NDIV 157 (2009 MHz, above the table). The registers are at `0x0E002000 + 8 * (cluster * 4 + MPIDR.Aff1)`, 12.8 MHz per NDIV step. **Only touch those `id * 8` words: the `+4` words in between are unbacked, and reading one SErrors into EL3 ("Unhandled Exception in EL3"), which stops the board.** The target is CPU 0's frequency when kexec fires, so `slmos-kexec` pins every cpufreq policy's `scaling_min_freq` to `scaling_max_freq` first (step [8.6/9], skip with `--no-cpufreq-pin`); on jetson-nano-2 that is 1344 MHz (NDIV 105) on every boot. Without the pin it varied with the governor (seen: 729/1036/1344 MHz). The `cpu` shell command prints each core's NDIV/MHz.

### CBB Firewall — Partially Bypassed

The CBB firewall has per-peripheral permissions. By running at **EL2 with VHE** enabled, SLM-OS can access UARTC, GICv3, and timer — enough to boot to the shell. UARTA (40-pin header) and GPU (0x17000000) remain blocked.

**Working approach (April 2026):**
- kexec from Linux → SLM-OS enters at EL2
- Enable VHE (HCR_EL2.E2H=1, TGE=1) → transparent EL1 register redirection
- Use UARTC (0x0C280000) for serial console (visible via TCU on USB-C debug)
- OP-TEE carveout at 0xBE-0xC2 skipped; ~6.7 GB usable across 3 regions

**SMP:**
- 6-core boot working via PSCI CPU_ON after kexec
- Root cause of prior failure: wrong MPIDR encoding. Jetson uses dual-cluster Aff2.Aff1: 0x000, 0x100, 0x200, 0x300, 0x10200, 0x10300
- All CPU-id resolution goes through `cpu_logical_map[]` (asm via `ARM64_GET_LOGICAL_CPU`, C via `cpu_logical_id()`); regression coverage in `kernel/tests/test_mpidr_lookup.c` (PR #647)
- Boot flag visibility has a PSCI-success fallback; the caches are coherent (#1193), so its WARN is a failure signal
- VHE set up on all secondary CPUs in `smp_boot.S`
- NC memory at 0xBDE00000 (2 MB, last block of region 1 before OP-TEE) for device DMA; cross-CPU shared data is cacheable (#1193, `docs/adr/0007-cross-cpu-coherency.md`)
- Cross-CPU task dispatch working — `bench smp` dispatches to all 6 CPUs. Under default `JETSON_HW_TICK=ON` build (PR #746/#755), secondaries WFI and wake from per-CPU PPI 26 timer IRQs (SEV broadcast is redundant but harmless). Under `JETSON_HW_TICK=OFF` cooperative fallback, secondaries WFE and wake from cross-CPU SEV.
- No cache maintenance on CPU-shared state (#1193); `cache.h` DC CVAC/CIVAC serves DMA, page tables and the caches-off `secondary_mmu_*` reads before secondary boot

**Preemptive multitasking:** Default `JETSON_HW_TICK=ON` build runs
true hardware quantum preemption at 100 Hz on every CPU under the
patched tegra234 BL31 (`tools/tfa-patches/0004-*`). Override with
`make kernel PLATFORM=JETSON_ORIN_NANO JETSON_HW_TICK=OFF` for
stock-BL31 / production-fused boards. Hardware verification:
`boot_test --count 10` 10/10 on jetson-nano-2 (2026-05-09). See
`docs/fact-sheets/preemption.md`.

**UEFI direct boot (working, #26):** `make kernel PLATFORM=JETSON_ORIN_NANO` + `scripts/jetson-uefi-deploy.sh <ip>` boots `slmos.bin` from the NVMe ESP on jetson-nano-2 (one-shot BootNext; a power cycle returns to SD Linux). Full guide: `docs/deploy/jetson-uefi-direct.md`.
- UEFI loads the PE image anywhere (never at `0x80000000`); `efi_stub_entry` checks the UEFI memory map, runs ExitBootServices, cleans by VA, and `boot.S` turns the MMU off and copies the image to the link address. An unsafe destination returns `EFI_LOAD_ERROR` to the firmware instead of hanging.
- **Never use set/way cache maintenance or touch the stack after the MMU goes off on this path** — set/way does not reach the PoC; that was the old "RAS error in IOB" blocker. The RAS `ADDR` field shows PA bit 31 as bit 63.
- Drivers that need Linux-initialised hardware must check `boot_source_is_uefi()` (`kernel/include/boot_source.h`) before touching MMIO: an unpowered block raises "Unhandled Exception in EL3". BPMP `MRQ_PG`/`MRQ_CLK` state queries can't replace it — they read "off" even for XUSB that Linux left running.
- USB networking works after a UEFI-direct boot (#1084): `xusb_t234_cold.c` powers XUSB through BPMP, programs the pads from fuses and waits for CNR (falcon boots from IFR firmware, nothing to load); the driver then enumerates the hub with `ADDRESS_DEVICE` BSR=0. Camera RCE works too.
- GPU compute works after a UEFI-direct boot (#1088): `main.c` runs `ga10b_cold_run()` (`kernel/gpu/nvidia/ga10b_cold.c`, steps follow nvgpu's power-on; `nvgpu cold [step]` from the shell) and the MNIST pipeline runs on an SLM-OS-created channel. FECS/GPCCS must be bootstrapped by the LS PMU RTOS: ACR-only bootstrap halts the ctxsw ucode with mailbox6=0x98 (Linux nvgpu without the LS PMU fails the same way).
- The sched/eviction GPU consumers and the operator library also work after a UEFI-direct boot (#1092): `step_policies` builds their v7 pipelines on the cold channel (`ga10b_cold_pipelines.c`, per-kind handoffs swapped in by `ensure_bringup` with the GPFIFO cursor carried over) and attaches the operator library to the channel's VM (`oplib_cold.c`). `nvgpu cold check` compares both pipelines with the CPU reference. **Never put an unconditional UART line on the per-dispatch path** (IRQs are masked under the dispatch lock): the #844 recovery prints cost ~146 ms per v7 dispatch until they moved behind `gpu debug on`. A silent board with live power was CPU 0 deadlocked in `net_pump` on a Rust spinlock held by a preempted task (#1093, fixed): Rust locks reached from IRQs-off paths must be IRQ-saving or try-locked (runtime/CLAUDE.md). `thermal` / `thermal log <ms>` print BPMP temperatures and per-CPU liveness. SLM-OS drives the fan on both boot paths (#1105, `kernel/drivers/tegra_fan.c`): nvfancontrol's quiet curve every 2 s from a task on the last CPU; `thermal fan [auto | <0-255>]` shows or overrides it.
- UEFI leaves the DTB above the RAM identity map (`0x2679DF000`); `vmm_map_firmware_dtb()` maps it read-only. Any PORTSC read-modify-write must start from `xhci_portsc_neutral()` — carrying PED (write-1-to-disable) disables the port.
- Models reach the kernel without a block driver (ADR 0002, `kernel/arch/arm64/efi_model.c`, `kernel/src/boot_models.c`): the stub reads `\slmos\models\*.gguf` through the firmware, NVMe first (780 ms, SD 68 s), and `slm load /boot/models/<name>.gguf` borrows the reserved pages.
- UEFI leaves EMC at 204 MHz; `main.c` asks BPMP's bandwidth manager for a 3199 MHz floor (`MRQ_BWMGR_INT`) right after `bpmp_init` (#1161). **Never set EMC with MRQ_CLK `set_rate`:** the next bandwidth-manager recompute (inside BPMP about 250 ms after the first request, or any `MRQ_BWMGR_INT` request) puts it back to 204 MHz; `make check-emc-rate-path` rejects it. `clocks` shows the rates and the drop counters (`kernel/drivers/emc_hold.c`).
- Host checks: `make test-qemu-uefi` (QEMU AArch64 UEFI, in place + forced relocation, model delivery and its failure paths), `make test-jetson-uefi-layout`.

**Documentation:** `docs/archive/investigations/jetson-nvidia-support.md`, `docs/jetson-el2-bringup.md`

### Reference Documentation

- [Carrier Board Specification (PDF)](https://developer.nvidia.com/downloads/assets/embedded/secure/jetson/orin_nano/docs/jetson_orin_nano_devkit_carrier_board_specification_sp.pdf) — Definitive pinouts for J14, J12, etc.
- [Developer Kit User Guide](https://developer.nvidia.com/embedded/learn/jetson-orin-nano-devkit-user-guide/howto.html)
- [JetsonHacks GPIO Pinout](https://jetsonhacks.com/nvidia-jetson-orin-nano-gpio-header-pinout/)
- [Orin TRM](https://developer.nvidia.com/orin-series-soc-technical-reference-manual) — Requires NVIDIA developer login
  - Local copy: `H:\My Drive\Capstone\Documentation\Orin-TRM_DP10508002_v1.2p.pdf`

### Serial Console Options

| Port | Address | Type | Connection | Status |
|------|---------|------|------------|--------|
| UARTA | 0x03100000 | NS16550 | 40-pin header pins 8/10 | BLOCKED by CBB (even at EL2) |
| UARTC | 0x0C280000 | NS16550 | Via TCU → USB-C debug | ✅ TX+RX working at EL2 |
| TCU | HSP mailbox | Combined UART | USB-C debug port | Routes UARTC output after kexec |

**UARTC (Working):** UARTC at 0x0C280000 is accessible from EL2. TX writes directly to UARTC THR. RX arrives via TCU HSP mailbox at 0x03C10000 (SPE firmware routes USB-C input there). Both directions fully functional.

**UARTA (Blocked):** The 40-pin header UART at 0x03100000 remains blocked by the CBB firewall even at EL2.

**TCU:** SPE firmware continues running after kexec and routes UARTC output through the USB-C debug port.

### Button Header (J14) Quick Reference

| Pins | Function |
|------|----------|
| 5-6 | Jumper for power button mode (remove for auto power-on) |
| 7-8 | Reset (momentary short) |
| 9-10 | USB Force Recovery (hold during power-on) |
| 11-12 | Power button (momentary short) |

### Remote Lab Access

Lab hardware is managed by **labctl** (Embedded Lab Control).

**Documentation:** [github.com/johnjezl/Embedded-Lab-Control](https://github.com/johnjezl/Embedded-Lab-Control)

Check documentation for features and capabilities.
labctl is implemented as an MCP server, but also has a CLI.

**Note:** The old `lab-tools/` scripts (jetson-power.py, jetson-uart.sh, etc.) have been replaced by labctl. Use labctl for all lab operations.

### SD Card Deploy Workflow

**IMPORTANT:** Always use `labctl` for ALL SD card operations. Never manually access `/dev/sdX` devices — multiple SDWire devices exist in the lab and manual access risks writing to the wrong device.

**For SLM-OS kernel updates** (copy kernel binary to existing boot partition):

The lab has two SDWire devices. `labctl` manages which device belongs to which SBC. Use the MCP tools (`sdwire_to_host`, `sdwire_to_dut`) or CLI equivalents, and use the block device path returned by labctl to ensure the correct SD card is accessed.

**Known issue:** The block device reported by labctl may become stale after USB re-enumeration. If the reported device shows 0B size, check `dmesg` or `lsblk` for the actual device that was assigned.

---

## Post-Change Checklist (Mandatory)

After EVERY code change — no exceptions — complete all applicable steps before reporting completion:

1. Write or update regression tests that would catch the defect/behavior if it regressed
2. Update any documentation affected by the change (docs/, status files, TODO lists, inline comments)
3. Run `make test` (QEMU) and confirm all tests pass
4. If the change affects Pi 5: build with `PLATFORM=RASPI5`, deploy via `labctl sdwire_update`, and verify on hardware using `serial_capture`/`serial_send`
5. Commit with a descriptive message

Do NOT wait for the user to ask for tests or documentation. Do NOT report a change as complete until steps 1-4 are done. If a step is not applicable (e.g., change is QEMU-only), note why it was skipped.

---

## Build-Deploy-Test Workflow

Use the `/deploy-and-test` skill (`.claude/skills/deploy-and-test/SKILL.md`) for Pi 5 and Jetson hardware iterations. Do NOT manually sequence individual `make`, `sdwire_update`, `serial_capture` calls when the skill can orchestrate them. If the skill is not working or missing, inform the user immediately — do not fall back to manual steps silently.

For quick single-command hardware verification:
```
/deploy-and-test pi-5-N [build flags] --cmd "<shell cmd>"
```
This should be ONE logical operation, not four separate user-visible steps. All of it goes through the labctl MCP server; never the labctl CLI.

---

## Hardware Debugging Methodology

When debugging hardware issues, follow this order:

0. **Verify live state before trusting docs.** Board state drifts from what the docs record. Before debugging an unresponsive board or starting a hardware verification run, check labctl: health (power/ping/serial), `bindings_list` + `actuation_status` for every binding on the board (a Jetson `recovery_mode` binding left `asserted` holds the board in USB Force Recovery — silent console, no network), and serial port ownership. Checklist: `docs/lab-operations.md` §"Verify live board state first".
1. **Identify the boundary.** What is the last known-working state and the first known-broken state? (e.g., "RX works before MMU enable, fails after" — test this FIRST before exploring baud rates, pad configs, or adapter hardware)
2. **Change one variable at a time.** Do not combine multiple hypotheses in one deploy.
3. **Use binary search.** If boot works at step A and fails at step Z, test at step M — don't test A+1, A+2, A+3 sequentially.
4. **Exhaust software causes before suspecting hardware.** Adapter swaps, multimeter tests, and oscilloscope checks come AFTER software hypotheses are eliminated.
5. **Document each hypothesis, test, and result** in the relevant status doc so work isn't repeated across sessions.

---

## Reference File Cache

Third-party reference material (NVIDIA L4T nvgpu, Mesa/NVK, nouveau, HailoRT, Linux kernel, RPi firmware, ARM Trusted Firmware, etc.) lives in a local-only reference library at `~/slmos-ref/`. This is no longer a git repo — it's a flat reference tree on the developer's machine.

In-tree citations use the form `~/slmos-ref/<vendor>/<filename>:<line>`. Vendor folders: `nvidia/`, `nouveau/`, `mesa/`, `hailo/`, `tegra-l4t/`, `linux/`, `rpi/`, `circle/`, `uboot/`, `kexec/`, `tf-a/`. SLM-OS-authored investigation notes and lab traces live under `derivatives/`.

Before launching a subagent to fetch source files from GitHub, check `~/slmos-ref/` first. If the file has been fetched before, use the local copy. If fetching new files, save them to the appropriate vendor folder there (not into the public repo). `~/slmos-ref/README.md` documents the layout.

Do NOT re-fetch the same GitHub raw URLs across multiple subagents in the same session. Fetch once, read from disk afterward.

Do NOT add third-party source files to this public repo's `docs/` tree. The reference cache is intentionally outside the repo to keep the public repo's git history clean of vendored upstream material.

---

## Reliability Testing

When testing boot reliability or hardware behavior changes, use `labctl boot_test` with an appropriate count (minimum 10) rather than asking the user to manually reboot and count. Report pass/fail ratio and any failure patterns (e.g., "failed 3/10, all failures showed garbled output after line 4").

---

## labctl Is the Only Hardware Interface

NEVER bypass labctl to interact with hardware. This includes:
- NEVER manually mounting SD cards or running `mount`/`cp`/`umount`
- NEVER directly accessing `/dev/sd*` block devices
- NEVER running `nc` or `picocom` directly for serial console access
- NEVER stopping/restarting `ser2net`

If labctl lacks a needed capability, inform the user and request the feature. Do not work around it — workarounds cause device conflicts with other projects sharing the lab infrastructure.

---

## Issue Tracking

All issues are tracked in GitHub Issues via `gh` CLI. This is the single source of truth for known bugs, investigations, enhancements, and technical debt.

### Filing Issues

- Use `gh issue create` with the appropriate template (`--template bug_report.md`, `--template investigation.md`, or `--template enhancement.md`)
- Always set platform and subsystem labels on creation: `--label "platform:jetson" --label "sub:smp"`
- Always set a priority label: `--label "P1-high"`
- Assign a milestone: `--milestone "Backlog"` for committed work, `--milestone "Icebox"` for ideas with no timeline. (The `Capstone` milestone is closed and kept only as the record of the original capstone; don't assign new work to it.)
- For quick filing without the template form: `gh issue create --title "..." --body "..." --label "..." --label "..."`

### Before Starting Work

- Run `gh issue list --label "sub:<subsystem>"` to check for known issues in the area being worked on
- Check for blockers: `gh issue list --label "blocker"`

### During Implementation

- When a bug or problem is discovered during implementation, file an issue immediately rather than leaving a TODO comment
- Reference issues in commit messages: `Fix #42: correct MPIDR encoding for A78AE dual-cluster`
- If work is blocked, create or update an issue with the `blocked` label and document what it's blocked on

### Closing Issues

- Close through commits when possible: the commit message `Fix #N` or `Closes #N` auto-closes on merge
- For manual closes: `gh issue close N --comment "Fixed in <commit-hash>, verified with boot_test --count 10"`
- Include verification details — test results, boot_test counts, UART output

### Priority Escalation

- P0-critical and P1-high issues should be flagged to John immediately in the session summary
- If an issue's priority is upgraded, add a comment explaining why

### Investigation Issues

- Capture the current hypothesis and what's been ruled out
- Update the issue during probing — don't let investigation context live only in conversation history
- When an investigation resolves into a concrete bug or enhancement, file the new issue and link it, then close the investigation

### Useful Queries

```bash
# All open issues by priority
gh issue list --state open --label "P0-critical"
gh issue list --state open --label "P1-high"

# Issues for a specific platform
gh issue list --label "platform:jetson"

# Blockers
gh issue list --label "blocker" --state open

# Everything in a milestone
gh issue list --milestone "Backlog"

# Search by keyword
gh issue list --search "MPIDR"
```

---

*Last updated: 4 November 2026*
