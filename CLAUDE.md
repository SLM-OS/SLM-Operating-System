# Claude Code Notes for SLM-OS Project

Project-wide notes and reminders. See also:
- `kernel/CLAUDE.md` — Kernel-specific (C, assembly)
- `runtime/CLAUDE.md` — Runtime-specific (Rust)

---

## Git Commits

Always ask for permission before committing code. Do not automatically commit changes after completing a task.

---

## File Editing

**Always use relative paths** when reading or editing files. Absolute paths (e.g., `H:/My Drive/...`) can cause "file has been unexpectedly modified" errors due to CLion indexing or Google Drive sync interference. Relative paths work more reliably.

**Correct:**
```
Read file_path="docs/shell.md"
Edit file_path="kernel/src/shell.c"
```

**Avoid:**
```
Read file_path="H:/My Drive/Capstone/CS-496-SLM-Operating-System/docs/shell.md"
```

**When experiencing repeated "file has been unexpectedly modified" errors:**
- Work with the user to diagnose the root cause first
- Do NOT resort to workarounds like `sed` or Bash heredocs
- Possible causes: CLion indexing, file watchers, IDE auto-save
- Try restarting CLion or pausing file sync services

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

## Project Environment

- **User's terminal**: Cygwin (paths like `/cygdrive/c/...`)
- **Claude Code's shell**: Git Bash/MINGW64 (paths like `/c/...`)
- This mismatch means Cygwin-style paths in the user's PATH don't work for Claude Code
- **Always use Windows-style paths** (`C:/Program Files/...`) in Makefiles and commands — they work in both environments
- Windows CMake must be used instead of Cygwin CMake (path translation issues)
- Project is on Google Drive (`H:\My Drive\`) which can cause file locking issues during builds

### Running Cygwin from Claude Code

**Problem:** When Claude Code runs `C:/cygwin64/bin/bash.exe`, it inherits Git Bash's mount table. This causes `/usr/bin` to point to Git Bash's binaries instead of Cygwin's, making Cygwin-installed programs (like `picocom`) unavailable.

**Solution:** Use `env -i` to clear the inherited environment before running Cygwin bash:

```bash
# Correct: Clean environment with proper Cygwin mounts
C:/cygwin64/bin/env.exe -i HOME=/tmp PATH=/usr/bin:/bin C:/cygwin64/bin/bash.exe --login -c "which picocom"
# Output: /usr/bin/picocom

# Incorrect: Inherits Git Bash mounts
C:/cygwin64/bin/bash.exe --login -c "which picocom"
# Output: picocom not found (because /usr/bin points to Git Bash)
```

**Verification:** Check which `/usr/bin` is mounted:
```bash
C:/cygwin64/bin/env.exe -i PATH=/usr/bin:/bin C:/cygwin64/bin/bash.exe -c "mount | grep usr"
# Should show: C:/cygwin64/bin on /usr/bin
# Not: C:/Program Files/Git/usr/bin on /usr/bin
```

---

## Build System

### Prerequisites

- ARM GNU Toolchain for Windows (aarch64-none-elf-gcc)
- Windows CMake (not Cygwin CMake)
- Cygwin make (C:/cygwin64/bin/make.exe)
- QEMU for Windows (qemu-system-aarch64)

### Build Commands

```bash
# Standard build targets (from project root):
"C:/cygwin64/bin/make.exe" kernel          # Build kernel
"C:/cygwin64/bin/make.exe" kernel-clean    # Clean kernel build
"C:/cygwin64/bin/make.exe" run             # Build and run in QEMU
"C:/cygwin64/bin/make.exe" debug           # Build and run with GDB server

# Alternative using -C flag:
"C:/cygwin64/bin/make.exe" -C "H:/My Drive/Capstone/CS-496-SLM-Operating-System" kernel
```

### Common Build Issues

1. **"Permission denied" during link**
   - Usually caused by stale QEMU process holding a lock on `slmos.elf`
   - **First step:** Look for running QEMU processes and kill them
   - Note: `ps -eaf | grep qemu` may fail — grep complains about "binary input" and misses processes. Use `tasklist.exe | grep -i qemu` or Windows Task Manager instead.
   - The Makefile's `check-build-dir` target tries to detect this, but may not catch all cases
   - Manual fix: Kill QEMU processes, then `rm -rf build/kernel` and rebuild
   - Can also happen with Google Drive sync - pause sync or wait.

2. **"make: command not found"**
   - Use full path: `"C:/cygwin64/bin/make.exe"`

3. **Path translation issues**
   - Windows tools need Windows paths (H:/My Drive/...)
   - Cygwin tools need Cygwin paths (/cygdrive/h/My Drive/...)
   - The Makefile handles this, but direct cmake calls may fail.

4. **Build directory on Google Drive**
   - See `docs/building.md` — file locking during sync can cause errors.

5. **CLion file locking during build**
   - Symptoms: "Permission denied" when linking `slmos.elf`, or CMake cache errors
   - Affected files: `slmos.elf`, `CMakeConfigureLog.yaml`, `CompilerIdC.exe`
   - **Root cause:** Microsoft's Incremental Linker (`link.exe`) holds file locks that persist even after CLion closes. Requires full system reboot to release.
   - **Current workaround:** Build to local temp directory: `C:/temp/slmos-build`
   - **CLion settings that may help** (Settings > Build > CMake):
     - Disable "Reload CMake project on editing CMakeLists.txt"
     - Disable "Auto-reload CMake on external changes" (Advanced Settings)
     - Disable "Sync project after changes in the build scripts"
     - Disable "Sync external changes when switching to the IDE window"
     - Disable "Sync external changes periodically when the IDE is inactive"
   - **Note:** Issue may be exacerbated by project being on Google Drive

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

**Quick reference:**
- Power control: `labctl power on/off/cycle pi-5-1`
- Serial console: `labctl connect pi-5-1-console`
- SD card deploy: See "SD Card Deploy Workflow" below
- Lab status: `labctl status`

**Restarting the Pi 5:**
1. **Preferred:** `labctl power cycle pi-5-1`
2. **From shell:** `reboot` command via SLM-OS shell (when RX input is working)

**Note:** The old `lab-tools/` scripts (jetson-power.py, jetson-uart.sh, etc.) have been replaced by labctl. Use labctl for all lab operations.

### SD Card Deploy Workflow

**IMPORTANT:** Always use `labctl` for ALL SD card operations. Never manually access `/dev/sdX` devices — multiple SDWire devices exist in the lab and manual access risks writing to the wrong device.

**If the MCP server fails:** Do NOT revert to using the labctl CLI directly or try to mount/modify the SD card manually. Instead, inform the user of the MCP failure so they can investigate and fix the labctl issue. Working around labctl defeats its purpose of managing multiple SDWire devices safely.

**For SLM-OS kernel updates** (copy kernel binary to existing boot partition):

The lab has two SDWire devices. `labctl` manages which device belongs to which SBC. Use the MCP tools (`sdwire_to_host`, `sdwire_to_dut`) or CLI equivalents, and use the block device path returned by labctl to ensure the correct SD card is accessed.

```bash
# 1. Power off the Pi
labctl power off pi-5-1

# 2. Switch SD card to host — note the block device path returned
labctl sdwire host pi-5-1
# Returns e.g.: "block device: /dev/sdd"

# 3. Mount the boot partition (use the device labctl returned + "1")
sudo mount /dev/sdd1 /mnt

# 4. Copy kernel binary
sudo cp build/kernel/slmos.bin /mnt/kernel_2712.img
sync

# 5. Unmount
sudo umount /mnt

# 6. Switch back to DUT and power on
labctl sdwire dut pi-5-1
labctl power on pi-5-1
```

**For full SD card images** (raw `dd` write):
```bash
labctl sdwire flash pi-5-1 path/to/image.img
```

**Known issue:** The block device reported by labctl may become stale after USB re-enumeration. If the reported device shows 0B size, check `dmesg` or `lsblk` for the actual device that was assigned.

---

*Last updated: 1 April 2026*
