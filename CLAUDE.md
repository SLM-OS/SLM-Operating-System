# Claude Code Notes for SLM-OS Project

Project-wide notes and reminders. See also:
- `kernel/CLAUDE.md` — Kernel-specific (C, assembly)
- `runtime/CLAUDE.md` — Runtime-specific (Rust)

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

Use green checkmark emoji (✅) for completed items, not markdown checkboxes:

**Correct:**
```markdown
- ✅ Task completed
- [ ] Task pending
- Deferred: Task postponed to later phase
```

**Incorrect:**
```markdown
- [x] Task completed  ← Don't use this
- ✓ Task completed   ← Don't use plain check symbol either
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

---

*Last updated: December 2025*
