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
| Pending | `[ ]` | `- [ ] Task pending` |
| Blocked | 🔗 | `- 🔗 Task — requires M4` |
| Deferred | ⏸️ | `- ⏸️ Task — deferred to Phase 4` |

**Definitions:**
- **Pending** `[ ]` — Ready to work on now
- **Blocked** 🔗 — Waiting on dependency within this phase (e.g., "requires M4")
- **Deferred** ⏸️ — Postponed to a future phase (e.g., "deferred to Phase 4")

**Correct:**
```markdown
- ✅ Task completed
- [ ] Task pending
- 🔗 Task — requires M4
- ⏸️ Task — deferred to Phase 4
```

**Incorrect:**
```markdown
- [x] Task completed  ← Don't use this
- ✓ Task completed   ← Don't use plain check symbol either
- Deferred: Task     ← Use ⏸️ emoji instead
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

### Reference Documentation

- [Carrier Board Specification (PDF)](https://developer.nvidia.com/downloads/assets/embedded/secure/jetson/orin_nano/docs/jetson_orin_nano_devkit_carrier_board_specification_sp.pdf) — Definitive pinouts for J14, J12, etc.
- [Developer Kit User Guide](https://developer.nvidia.com/embedded/learn/jetson-orin-nano-devkit-user-guide/howto.html)
- [JetsonHacks GPIO Pinout](https://jetsonhacks.com/nvidia-jetson-orin-nano-gpio-header-pinout/)
- [Orin TRM](https://developer.nvidia.com/orin-series-soc-technical-reference-manual) — Requires NVIDIA developer login
  - Local copy: `H:\My Drive\Capstone\Documentation\Orin-TRM_DP10508002_v1.2p.pdf`

### Serial Console Options

| Port | Address | Type | Connection | Status |
|------|---------|------|------------|--------|
| UARTA | 0x03100000 | NS16550 | 40-pin header pins 8/10 | Works, needs USB-serial adapter |
| TCU | HSP mailbox | Combined UART | USB-C debug port | Requires SPE firmware (see below) |

**TCU (USB-C Debug):** The USB-C debug console uses the Tegra Combined UART (TCU), which routes through SPE firmware via HSP mailboxes. After kexec, SPE is no longer running, so TCU doesn't work for bare-metal. See `docs/jetson-tcu.md` for full research notes.

**Recommended:** Use USB-serial adapter on 40-pin header (UARTA) for SLM-OS debugging.

### Button Header (J14) Quick Reference

| Pins | Function |
|------|----------|
| 5-6 | Jumper for power button mode (remove for auto power-on) |
| 7-8 | Reset (momentary short) |
| 9-10 | USB Force Recovery (hold during power-on) |
| 11-12 | Power button (momentary short) |

### Remote Lab Access

See `lab-tools/` for scripts:
- `jetson-power.py` — Kasa smart plug control (192.168.4.112)
- `jetson-debug.sh` — Debug USB-C serial (/dev/ttyS4)
- `jetson-uart.sh` — 40-pin header UART

SSH: `ssh -p 4243 root@gradient-nano.onthewifi.com`

---

*Last updated: December 2025*
