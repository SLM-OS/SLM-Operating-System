# Debug Monitor (Shell)

A minimal serial debug monitor for hardware bring-up and demos. This is **optional** — skip if behind schedule.

**Status:** Not implemented

---

## Overview

Not a real shell — just a command loop with `strcmp()` dispatch. Target: ~200-300 lines of C.

The industrial IoT deployment doesn't involve humans typing commands, so this is purely a development/demo tool.

---

## Commands

```
SLM-OS> help
  mem      - memory statistics
  tasks    - list tasks
  cpu      - per-core status
  gpio <n> - toggle GPIO pin
  reboot   - restart system

SLM-OS> tasks
PID  STATE    CPU  NAME
  1  RUNNING    0  shell
  2  READY      1  idle
  3  BLOCKED    2  model_loader
```

### Command Descriptions

| Command | Description |
|---------|-------------|
| `help` | List available commands |
| `mem` | Show PMM statistics (free pages, allocated, etc.) |
| `tasks` | List all tasks with PID, state, CPU, and name |
| `cpu` | Show per-core status (online, current task, idle time) |
| `gpio <n>` | Toggle GPIO pin N (real hardware only) |
| `reboot` | Restart system via PSCI |

---

## Implementation

### Key Functions

| Function | Description |
|----------|-------------|
| `shell_init()` | Spawn shell task on CPU 0 |
| `shell_getline()` | Read line from UART (blocking) |
| `shell_dispatch()` | Match command string and execute handler |

### Architecture

```
┌─────────────────────────────────────────────────────────┐
│  shell_task()                                           │
│  ┌─────────────────────────────────────────────────┐    │
│  │  while (1) {                                    │    │
│  │      print_prompt();                            │    │
│  │      shell_getline(buffer, sizeof(buffer));    │    │
│  │      shell_dispatch(buffer);                   │    │
│  │  }                                              │    │
│  └─────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
```

### Command Dispatch

Simple `strcmp()` chain — no parsing library needed:

```c
if (strcmp(cmd, "help") == 0) {
    cmd_help();
} else if (strcmp(cmd, "mem") == 0) {
    cmd_mem();
} else if (strcmp(cmd, "tasks") == 0) {
    cmd_tasks();
} else if (strncmp(cmd, "gpio ", 5) == 0) {
    int pin = atoi(cmd + 5);
    cmd_gpio(pin);
} else {
    uart_printf("Unknown command: %s\n", cmd);
}
```

---

## Not In Scope

The following features are explicitly out of scope:

- Scripting
- Pipes
- Job control
- Command history
- Tab completion
- Line editing (backspace is OK)

---

## Alternative if Skipped

If the debug monitor is not implemented, use:

- Compile-time test selection (`#ifdef TEST_xxx`)
- `uart_printf()` debugging
- Hard-coded demo sequences in `main()`

---

*Created: December 2025*
