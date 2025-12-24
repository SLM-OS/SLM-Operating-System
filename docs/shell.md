# Debug Monitor (Shell)

A minimal serial debug monitor for hardware bring-up and demos.

**Status:** Implemented

---

## Overview

A simple command loop with table-driven dispatch. The shell runs as a kernel task on CPU 0 and provides interactive access to system status and diagnostics.

The industrial IoT deployment doesn't involve humans typing commands, so this is purely a development/demo tool.

---

## Commands

```
SLM-OS> help
Available commands:
  help    - List available commands
  mem     - Show memory statistics
  tasks   - List all tasks
  cpu     - Show CPU status
  uptime  - Show system uptime
  vmm     - Show virtual memory info
  ipc     - Show IPC statistics
  model   - Show model memory pools
  dtb     - Show device tree info
  elftest - Test ELF loader
  run     - Run embedded test ELF
  clear   - Clear screen
  reboot  - Restart the system
```

### Command Descriptions

| Command | Description |
|---------|-------------|
| `help` | List available commands |
| `mem` | Show PMM statistics (total pages, free, allocated) |
| `tasks` | List all tasks with ID, state, CPU affinity, priority, and name |
| `cpu` | Show per-core status (online state, current task) |
| `uptime` | Show system uptime in seconds and milliseconds |
| `vmm` | Show virtual memory statistics (page tables, mapped regions) |
| `ipc` | Show IPC statistics (message queues, shared buffers) |
| `model` | Show model memory pool status (weight and workspace pools) |
| `dtb` | Show Device Tree info (parsed or defaults) |
| `elftest` | Run ELF loader validation tests (header parsing, architecture checks) |
| `run` | Load and execute an embedded test ELF (demonstrates ELF loader) |
| `clear` | Clear terminal screen (ANSI escape sequence) |
| `reboot` | Restart system via PSCI (QEMU: triggers exit) |

---

## Implementation

Located in `kernel/src/shell.c`.

### Key Functions

| Function | Description |
|----------|-------------|
| `shell_init()` | Initialize shell and register built-in commands |
| `shell_start()` | Spawn shell task on CPU 0 |
| `shell_process_char()` | Handle incoming UART character (called from UART IRQ) |
| `shell_register_command()` | Register external command at runtime |

### Architecture

The shell uses interrupt-driven input via the UART driver:

```
┌─────────────────────────────────────────────────────────┐
│  UART IRQ Handler                                       │
│  └── shell_process_char(c)                              │
│      ├── Echo character                                 │
│      ├── Buffer until newline                           │
│      └── On newline: parse and dispatch                 │
└─────────────────────────────────────────────────────────┘
```

### Command Dispatch

Table-driven dispatch with support for external command registration:

```c
static const struct shell_command builtin_commands[] = {
    {"help",   cmd_help,   "List available commands"},
    {"mem",    cmd_mem,    "Show memory statistics"},
    {"tasks",  cmd_tasks,  "List all tasks"},
    // ...
};

// Runtime registration for test commands, etc.
shell_register_command("test", cmd_test, "Run tests");
```

---

## Not In Scope

The following features are explicitly out of scope:

- Scripting
- Pipes
- Job control
- Command history (up arrow)
- Tab completion

Backspace and basic line editing are supported.

---

*Last updated: December 2025*
