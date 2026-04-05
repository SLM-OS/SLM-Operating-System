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

- ARM GNU Toolchain for Windows (aarch64-none-elf-gcc)
- GNU make
- QEMU for Windows (qemu-system-aarch64)

### Build Commands

```bash
# Standard build targets (from project root):
make kernel          # Build kernel
make kernel-clean    # Clean kernel build
make run             # Build and run in QEMU
make debug           # Build and run with GDB server

### Common Build Issues

1. **"Permission denied" during link**
   - Usually caused by stale QEMU process holding a lock on `slmos.elf`
   - **First step:** Look for running QEMU processes and kill them
   - Note: `ps -eaf | grep qemu` may fail — grep complains about "binary input" and misses processes. Use `tasklist.exe | grep -i qemu` or Windows Task Manager instead.
   - The Makefile's `check-build-dir` target tries to detect this, but may not catch all cases
   - Manual fix: Kill QEMU processes, then `rm -rf build/kernel` and rebuild

---

## Jetson Hardware

**Board:** Jetson Orin Nano Super Developer Kit

**Status:** ⛔ BLOCKED — CBB firewall prevents bare-metal peripheral access

### Critical Blocker

SLM-OS bare-metal execution on Jetson is blocked by the Tegra234 Control Backbone (CBB) firewall, which prevents unsigned/unauthenticated code from accessing peripherals. This is a hardware-enforced security feature.

**Key findings:**
- kexec is NOT supported (NVIDIA confirmed)
- Direct UEFI boot has same CBB restrictions
- All boot methods blocked by CBB firewall

**Comprehensive documentation:** `docs/jetson-nvidia-support.md`

### Reference Documentation

- [Carrier Board Specification (PDF)](https://developer.nvidia.com/downloads/assets/embedded/secure/jetson/orin_nano/docs/jetson_orin_nano_devkit_carrier_board_specification_sp.pdf) — Definitive pinouts for J14, J12, etc.
- [Developer Kit User Guide](https://developer.nvidia.com/embedded/learn/jetson-orin-nano-devkit-user-guide/howto.html)
- [JetsonHacks GPIO Pinout](https://jetsonhacks.com/nvidia-jetson-orin-nano-gpio-header-pinout/)
- [Orin TRM](https://developer.nvidia.com/orin-series-soc-technical-reference-manual) — Requires NVIDIA developer login
  - Local copy: `H:\My Drive\Capstone\Documentation\Orin-TRM_DP10508002_v1.2p.pdf`

### Serial Console Options

| Port | Address | Type | Connection | Status |
|------|---------|------|------------|--------|
| UARTA | 0x03100000 | NS16550 | 40-pin header pins 8/10 | Works with Linux; BLOCKED for SLM-OS (CBB firewall) |
| TCU | HSP mailbox | Combined UART | USB-C debug port | Requires SPE firmware (Linux only) |

**TCU (USB-C Debug):** The USB-C debug console uses the Tegra Combined UART (TCU), which routes through SPE firmware via HSP mailboxes. After kexec, SPE is no longer running, so TCU doesn't work for bare-metal. See `docs/jetson-tcu.md` for full research notes.

**UARTA (40-pin Header):** Serial hardware works for Linux. For SLM-OS, the CBB firewall blocks access to 0x03100000. See `docs/jetson-nvidia-support.md` for full analysis.

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
