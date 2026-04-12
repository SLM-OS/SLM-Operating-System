# Lua Scripting

SLM-OS includes an embedded Lua 5.4 interpreter for scripting and automation.

## Overview

Lua integration provides:
- Interactive REPL (Read-Eval-Print Loop) for experimentation
- Direct code execution via command line
- Kernel API access through the `slm` module
- Standard Lua libraries: base, table, string, math

## Shell Command

```
lua              # Enter interactive REPL
lua -e "code"    # Execute Lua code directly
lua <file>       # Run script from filesystem
```

### Interactive REPL

```
slmos> lua
Lua 5.4 REPL - type 'exit' to quit
>>> print("Hello from Lua!")
Hello from Lua!
>>> 1 + 2
>>> print(1 + 2)
3
>>> exit
Exiting Lua REPL
```

REPL controls:
- `exit` - Exit the REPL
- `Ctrl+D` - Exit the REPL
- `Ctrl+C` - Cancel current line

### Direct Execution

```
slmos> lua -e "print(1+2)"
3
slmos> lua -e "for i=1,5 do print(i) end"
1
2
3
4
5
```

### Script Files

Scripts can be loaded from the mounted filesystem:

```
slmos> write /mnt/files/hello.lua print('Hello from file!')
slmos> lua /mnt/files/hello.lua
Hello from file!
```

Scripts have full access to the `slm` module:

```
slmos> write /mnt/files/status.lua slm.print('Up: ' .. slm.uptime() .. 'ms')
slmos> lua /mnt/files/status.lua
Up: 12345ms
```

Scripts are limited to 4KB. If the file cannot be read or contains errors,
`lua` prints a diagnostic and returns to the shell.

## SLM-OS Kernel Bindings

The `slm` module provides access to kernel functionality:

### System

| Function | Description |
|----------|-------------|
| `slm.print(...)` | Print to console (replaces Lua's print) |
| `slm.uptime()` | Get system uptime in milliseconds |
| `slm.mem_stats()` | Get memory statistics table |
| `slm.tasks()` | Get list of running tasks |
| `slm.sleep(ms)` | Sleep for milliseconds |
| `slm.yield()` | Yield CPU to scheduler |
| `slm.version()` | Get SLM-OS version string |
| `slm.cpu_count()` | Get number of CPUs |
| `slm.cpu_id()` | Get current CPU ID |

### Component Management

| Function | Description |
|----------|-------------|
| `slm.component_count()` | Number of registered components |
| `slm.component_list()` | Array of component tables (name, version, type, state, priority, task_id, index) |
| `slm.component_find(name)` | Find component by name, returns index or nil |
| `slm.component_run(name)` | Run a built-in component, returns index or nil |
| `slm.component_hot_swap(old, new)` | Replace component preserving subscriptions, returns index or nil |

### Message Routing

| Function | Description |
|----------|-------------|
| `slm.msg_publish(topic, data)` | Publish a message to a topic. Returns number of subscribers that received it. |

### Scheduler

| Function | Description |
|----------|-------------|
| `slm.sched_policy()` | Get current scheduler policy name (e.g., "heuristic"). |

### Model Memory and Inference

| Function | Description |
|----------|-------------|
| `slm.model_stats()` | Pool statistics: `{weights={total_blocks, free_blocks, allocated_blocks, shared_blocks, peak_usage}, workspace={...}}` |
| `slm.model_load_mnist()` | Load the built-in MNIST ONNX model (26 KB). Returns model index or -1. |
| `slm.model_find(name)` | Find a loaded model by name. Returns index or -1. |
| `slm.model_infer(index)` | Run inference on a loaded model. Returns predicted class (0-9 for MNIST). |

### Memory Statistics

```lua
>>> stats = slm.mem_stats()
>>> for k,v in pairs(stats) do print(k, v) end
total_kb    1048576
free_kb     1020432
used_kb     28144
```

### Task Information

```lua
>>> for _,t in ipairs(slm.tasks()) do
...   print(t.id, t.name, t.state, t.cpu)
... end
0       idle    ready   0
1       idle    ready   1
2       idle    ready   2
3       idle    ready   3
4       init    blocked 0
5       test    running 1
6       shell   running 2
```

### Component Management

```lua
>>> slm.component_run('counter')
0
>>> slm.component_run('echo')
1
>>> for _,c in ipairs(slm.component_list()) do
...   print(c.name, c.state, c.type)
... end
counter   running   service
echo      running   service
>>> slm.component_hot_swap('counter', 'listener')
0
```

### Model Memory Statistics

```lua
>>> stats = slm.model_stats()
>>> w = stats.weights
>>> print('Weight pool: ' .. w.free_blocks .. '/' .. w.total_blocks .. ' free')
Weight pool: 128/128 free
>>> ws = stats.workspace
>>> print('Workspace: ' .. ws.free_blocks .. '/' .. ws.total_blocks .. ' free')
Workspace: 64/64 free
```

## Implementation Details

### Build Configuration

Lua is built as a separate static library to allow floating-point operations
while the kernel itself uses `-mgeneral-regs-only`. The Lua library includes:

- Lua 5.4 core (`kernel/lib/lua/src/`)
- Libc stubs for freestanding environment (`kernel/src/lua_stubs.c`)
- SLM-OS bindings (`kernel/src/lua_slm.c`)
- Shell command (`kernel/src/lua_shell.c`)
- setjmp/longjmp for error handling (`kernel/arch/arm64/setjmp.S`)

### Lua Configuration

Lua is configured for embedded use with:
- `LUA_32BITS=1` - 32-bit integers and floats
- `LUA_USE_C89=1` - C89 compatibility mode
- `LUAI_MAXSTACK=15000` - Reduced stack size
- `LUA_MINBUFFER=32` - Smaller buffers

### Memory

Lua uses a 1MB heap allocated in `.bss` for all allocations. The heap
uses a first-fit allocator with coalescing on free.

### Libc Stubs

The freestanding environment provides minimal implementations of:
- Memory: `malloc`, `free`, `realloc`, `calloc`
- Strings: `strrchr`, `strcat`, `strstr`, `strdup`, etc.
- Math: `sin`, `cos`, `exp`, `log`, `sqrt`, `pow`, etc.
- I/O: `printf`, `fprintf` (output to UART)
- Time: `time`, `clock` (based on timer counter)

## Limitations

- **4KB script size limit**: Scripts loaded from files are limited to 4KB
- **No coroutines**: The coroutine library is not enabled
- **No debug library**: The debug library is not enabled
- **No os library**: System calls are not available
- **Limited math precision**: Math functions use Taylor series approximations

## Future Enhancements

- Expose more kernel APIs (IPC, networking, GPU)
- Script-driven test automation
- Configuration files in Lua
