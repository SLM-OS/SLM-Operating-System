# Claude Code Notes for SLM-OS Project

Project-wide notes and reminders. See also:
- `kernel/CLAUDE.md` — Kernel-specific (C, assembly)
- `runtime/CLAUDE.md` — Runtime-specific (Rust)

---

## Build, Deploy, and Test

Use the `/deploy-and-test` skill when building, deploying to Pi 5 hardware, and capturing boot/test output. This skill orchestrates the labctl MCP tools (sdwire_update, power_cycle, serial_capture, boot_test) for the full deploy cycle.

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

**Definitions:**
- **Pending** ☐ — Ready to work on now
- **Blocked** 🔗 — Waiting on dependency within this phase (combine with ☐ or ⏸️)
- **Deferred** ⏸️ — Postponed to a future phase (e.g., "deferred to Phase 5")

**Correct:**
```markdown
- ✅ Task completed
- ☐ Task pending
- ☐🔗 Task — requires M4 (pending, has dependency)
- ⏸️ Task — deferred to Phase 5
- ⏸️🔗 Task — deferred, had dependency when deferred
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
- Rust toolchain with `aarch64-unknown-none` and `x86_64-unknown-none` targets

### Build Commands

```bash
# Standard build targets (from project root):
make kernel          # Build kernel (default: QEMU_VIRT ARM64)
make kernel-clean    # Clean kernel build
make run             # Build and run in QEMU
make debug           # Build and run with GDB server
make test            # Build test kernel and run in QEMU

# x86-64 platform:
make kernel PLATFORM=X86_64    # Build for x86-64
make test PLATFORM=X86_64      # Run x86-64 tests (creates GRUB ISO, uses isa-debug-exit)
make run PLATFORM=X86_64       # Run in QEMU (uses q35 machine, multiboot2 via ISO)

# Other platforms:
make kernel PLATFORM=RASPI5            # Raspberry Pi 5
make kernel PLATFORM=JETSON_ORIN_NANO  # Jetson Orin Nano

# AI scheduler (optional, adds MLP/PPO policies):
# Must configure manually since Makefile doesn't forward cmake options
cmake -B build/kernel -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-none-elf.cmake \
  -DPLATFORM=QEMU_VIRT -DENABLE_AI_SCHEDULER=ON && cmake --build build/kernel

# AI scheduler on x86-64 (uses SSE instead of NEON):
cmake -B build/kernel -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64-none-elf.cmake \
  -DPLATFORM=X86_64 -DENABLE_AI_SCHEDULER=ON && cmake --build build/kernel
```

The Makefile automatically selects the correct toolchain, QEMU binary, and QEMU machine settings based on `PLATFORM`. For x86-64, the test target creates a bootable GRUB ISO and uses `isa-debug-exit` for clean test termination.

**QEMU safeguards:** `make test` wraps QEMU in `systemd-run --user --scope` with `MemoryMax=3G` (prevents OOM crashes) and `CPUQuota=200%` (prevents runaway busy-spin tests from pegging all host cores), plus `timeout 120` for automatic termination. These are defined in the Makefile as `QEMU_GUARD` and `TEST_TIMEOUT`.

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

**Status:** 🟡 Partially Working — EL2 + VHE + UARTC bypasses CBB for serial and core subsystems

### Known Issues

**Spinlock / LSE Atomics:** On Cortex-A78AE, ARM LSE atomics (SWPALB) and exclusive operations (LDAXR/STXR) cause Synchronous External Abort before MMU enable (non-cacheable memory). `SPINLOCK_SKIP_LOCKING` is defined in `platform.h` to use barrier-only spinlocks. This is safe for early single-CPU boot; SMP data uses NC memory.

**GPU CBB Firewall:** GPU registers at 0x17000000 are behind the CBB firewall. Reading NV_PMC_BOOT_0 triggers an external abort. The stub GPU driver is used on Jetson instead of the NVIDIA probe driver.

**nvgpu RAS Error:** Linux's nvgpu driver leaves stale GPU DMA operations after kexec. TF-A (EL3) catches the resulting RAS Uncorrectable Error and powers off the CPU core. This is a Linux/TF-A interaction issue, not an SLM-OS bug. Workaround: needs investigation into stopping nvgpu cleanly before kexec, or resetting the GPU fabric from EL3.

### CBB Firewall — Partially Bypassed

The CBB firewall has per-peripheral permissions. By running at **EL2 with VHE** enabled, SLM-OS can access UARTC, GICv3, and timer — enough to boot to the shell. UARTA (40-pin header) and GPU (0x17000000) remain blocked.

**Working approach (April 2026):**
- kexec from Linux → SLM-OS enters at EL2
- Enable VHE (HCR_EL2.E2H=1, TGE=1) → transparent EL1 register redirection
- Use UARTC (0x0C280000) for serial console (visible via TCU on USB-C debug)
- OP-TEE carveout at 0xBE-0xC2 skipped; ~6.7 GB usable across 3 regions

**SMP (April 2026):**
- 6-core boot working via PSCI CPU_ON after kexec
- Root cause of prior failure: wrong MPIDR encoding. Jetson uses dual-cluster Aff2.Aff1: 0x000, 0x100, 0x200, 0x300, 0x10200, 0x10300
- Boot flag visibility uses PSCI success fallback (same cache incoherency as Pi 5)
- VHE set up on all secondary CPUs in `smp_boot.S`
- NC memory at 0xBDE00000 (2 MB, last block of region 1 before OP-TEE) for cross-CPU shared data
- Cross-CPU task dispatch working — `bench smp` dispatches to all 6 CPUs via cooperative WFE/SEV
- DC CVAC/CIVAC cache maintenance active (cache.h), page tables flushed to DRAM before secondary boot

**UEFI direct boot (WIP, not required for SMP):**
- EFI stub (`efi_stub.c`) with VHE-compatible MMU disable + self-relocating trampoline in `boot.S`
- PE/COFF loads when UEFI uses preferred address (ImageBase=0x80000000)
- Blocked when UEFI can't use preferred address (no `.reloc` section for PE relocation)

**Documentation:** `docs/jetson-nvidia-support.md`, `docs/jetson-el2-bringup.md`

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

Use the build-and-run skill for Pi 5 hardware iterations. Do NOT manually sequence individual `make`, `sdwire_update`, `serial_capture` calls when the skill can orchestrate them. If the skill is not working or missing, inform the user immediately — do not fall back to manual steps silently.

For quick single-command hardware verification:
```
make kernel PLATFORM=RASPI5 && labctl sdwire_update + serial_capture + serial_send
```
This should be ONE logical operation, not four separate user-visible steps.

---

## Hardware Debugging Methodology

When debugging hardware issues, follow this order:

1. **Identify the boundary.** What is the last known-working state and the first known-broken state? (e.g., "RX works before MMU enable, fails after" — test this FIRST before exploring baud rates, pad configs, or adapter hardware)
2. **Change one variable at a time.** Do not combine multiple hypotheses in one deploy.
3. **Use binary search.** If boot works at step A and fails at step Z, test at step M — don't test A+1, A+2, A+3 sequentially.
4. **Exhaust software causes before suspecting hardware.** Adapter swaps, multimeter tests, and oscilloscope checks come AFTER software hypotheses are eliminated.
5. **Document each hypothesis, test, and result** in the relevant status doc so work isn't repeated across sessions.

---

## Reference File Cache

Before launching a subagent to fetch source files from GitHub (Circle, Linux kernel, NVIDIA open-gpu-kernel-modules, RP1 datasheet), check `docs/reference/` first. If the file has been fetched before, use the local copy. If fetching new files, save them to `docs/reference/` for future sessions.

Do NOT re-fetch the same GitHub raw URLs across multiple subagents in the same session. Fetch once, read from disk afterward.

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

*Last updated: 1 April 2026*
