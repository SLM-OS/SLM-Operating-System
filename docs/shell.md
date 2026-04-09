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
  help      - List available commands
  mem       - Show memory statistics
  tasks     - List all tasks
  cpu       - Show CPU status
  uptime    - Show system uptime
  vmm       - Show virtual memory info
  ipc       - Show IPC statistics
  model     - Model management (load/unload/info/list/gpu/pools/stats/bench)
  dtb       - Show device tree info
  elftest   - Test ELF loader
  run       - Run a program (run <name>)
  kill      - Terminate a task by ID
  ls        - List directory (ls [path])
  cd        - Change directory (cd [path])
  pwd       - Print working directory
  cat       - Show file contents (cat <path> [offset] [length])
  write     - Write to file (write <path> <content>)
  mkdir     - Create directory (mkdir <path>)
  rm        - Remove file/dir (rm <path>)
  mv        - Move/rename (mv <src> <dst>)
  df        - Filesystem stats (df [path])
  truncate  - Truncate file (truncate <path> <size>)
  append    - Append to file (append <path> <content>)
  cp        - Copy file (cp <src> <dst>)
  touch     - Create empty file (touch <path>)
  stat      - Show file info (stat <path>)
  tree      - Recursive directory listing (tree [path])
  wc        - Count lines/words/bytes (wc <path>)
  hexdump   - Hex dump file (hexdump <path> [offset] [len])
  grep      - Search in file (grep <pattern> <path>)
  find      - Find files (find <path> <pattern>)
  component - Component system (list/run/swap/register/status)
  msg       - Message router (send/list/subscribe)
  net       - Network control (init/status)
  ping      - Send ICMP echo request
  ifconfig  - Network interface config
  netstat   - Network statistics
  sched     - Scheduler policy management and stats
  lua       - Lua scripting (REPL, -e "code", or script file)
  clear     - Clear screen
  reboot    - Restart the system
```

### Command Descriptions

| Command | Description |
|---------|-------------|
| `help [cmd]` | List commands, or show detailed help for a specific command |
| `mem` | Show PMM statistics (total pages, free, allocated) |
| `tasks` | List all tasks with ID, state, CPU affinity, priority, and name |
| `cpu` | Show per-core status (online state, current task) |
| `uptime` | Show system uptime in seconds and milliseconds |
| `vmm` | Show virtual memory statistics (page tables, mapped regions) |
| `ipc` | Show IPC statistics (message queues, shared buffers) |
| `model` | Show model pool stats and loaded model list |
| `model load <path>` | Load an ONNX model from VFS (e.g., `/mnt/files/mnist.onnx`) |
| `model list` | List loaded models with parameter count, weight size, and node count |
| `model info <name\|idx>` | Show detailed information for a loaded model |
| `model unload <name\|idx>` | Unload a model by name or registry index and free its memory |
| `model infer <name\|idx>` | Run inference on model with zero input, print output probabilities and predicted class |
| `model gpu` | Show GPU status, capabilities, and inference backend |
| `model pools` | Show weight and workspace memory pool statistics |
| `model stats` | Show inference performance statistics (latency, throughput, errors) |
| `model bench <name> [N]` | Benchmark model inference latency (default 10 iterations) |
| `dtb` | Show Device Tree info (parsed or defaults) |
| `elftest` | Run ELF loader validation tests (header parsing, architecture checks) |
| `run <name>` | Run a program by name from the ELF table |
| `kill <pid>` | Terminate a task by process ID |
| `ls [path]` | List directory contents (defaults to cwd) |
| `cd [path]` | Change working directory (defaults to `/`) |
| `pwd` | Print current working directory |
| `cat <path> [off] [len]` | Display file contents with optional offset/length |
| `write <path> <content>` | Write content to file (creates or overwrites) |
| `mkdir <path>` | Create directory in mounted filesystem |
| `rm <path>` | Remove file or empty directory |
| `mv <src> <dst>` | Move/rename file or directory |
| `df [path]` | Show filesystem usage statistics |
| `truncate <path> <size>` | Truncate file to specified size |
| `append <path> <content>` | Append content to file (for logging) |
| `cp <src> <dst>` | Copy file (cross-mount supported) |
| `touch <path>` | Create empty file if it doesn't exist |
| `stat <path>` | Show file/directory information (type, size) |
| `tree [path] [depth]` | Recursive directory listing (default depth: 5) |
| `wc <path>` | Count lines, words, and bytes in file |
| `hexdump <path> [off] [len]` | Hex dump of file contents (default: 256 bytes) |
| `grep <pattern> <path>` | Search for substring in file (shows line numbers) |
| `find <path> <pattern>` | Find files by name pattern (wildcards: `*`, `?`) |
| `component` | Component system management (see below) |
| `msg` | Message router commands (see below) |
| `net init\|status` | Initialize network or show status |
| `ping <ip> [count]` | Send ICMP echo requests (default: 4) |
| `ifconfig` | Show network configuration |
| `ifconfig dhcp` | Enable DHCP |
| `ifconfig <ip> <mask> <gw>` | Set static IP configuration |
| `netstat` | Show network TX/RX statistics |
| `sched` | Show current scheduler policy name |
| `sched policy` | List all registered scheduling policies |
| `sched policy <name>` | Switch to a named scheduling policy |
| `sched stats` | Show scheduler statistics and per-CPU utilization |
| `lua` | Enter Lua REPL |
| `lua -e "code"` | Execute Lua code directly |
| `lua <file>` | Run Lua script from filesystem |
| `clear` | Clear terminal screen (ANSI escape sequence) |
| `reboot` | Restart system via PSCI (QEMU: triggers exit) |

### Getting Help

The `help` command has two modes:

**List all commands:**
```
SLM-OS> help
Available commands:

  help       List available commands
  mem        Show memory statistics
  tasks      List all tasks
  ...

Use 'help <cmd>' for detailed help on a command.
```

**Detailed help for a command:**
```
SLM-OS> help cp
cp - Copy files

Usage:
  cp <source> <destination>

Copies file contents from source to destination. Works across
mount points. Destination is overwritten if it exists.

Examples:
  cp hello.txt backup.txt           Copy in current dir
  cp /mnt/files/a.txt ./b.txt       Copy with paths
```

Help text is stored in `/mnt/files/help/*.txt` files, written at boot. This file-driven approach keeps help text out of the compiled kernel binary and makes it easy to modify.

### Working Directory

The shell maintains a current working directory (cwd) that starts at `/`. Use `cd` and `pwd` to navigate:

```
SLM-OS> pwd
/

SLM-OS> cd /mnt/files
SLM-OS> pwd
/mnt/files

SLM-OS> cd ..
SLM-OS> pwd
/mnt

SLM-OS> cd
SLM-OS> pwd
/
```

All filesystem commands support relative paths:

```
SLM-OS> cd /mnt/files
SLM-OS> ls
hello.txt    [f]    20
readme.txt   [f]    74

SLM-OS> cat hello.txt
Hello from LittleFS!

SLM-OS> mkdir logs
Created directory /mnt/files/logs

SLM-OS> ls ./logs
(empty directory)

SLM-OS> cd logs
SLM-OS> pwd
/mnt/files/logs

SLM-OS> write app.log Started
Wrote 7 bytes to /mnt/files/logs/app.log

SLM-OS> cat ../hello.txt
Hello from LittleFS!
```

Path resolution handles:
- Relative paths (prepends cwd)
- `.` (current directory)
- `..` (parent directory)
- Multiple slashes (`//` → `/`)
- Trailing slashes (stripped)

### Filesystem Commands

The `ls` and `cat` commands work with both virtual files and mounted filesystems:

```
SLM-OS> ls /
sys/
proc/
components/
mnt/

SLM-OS> ls /sys
memory
cpus
ipc
model
uptime
version

SLM-OS> cat /sys/memory
total_kb: 1048576
free_kb: 1047040
allocated_pages: 384
```

Mount point access (LittleFS on RAM disk):

```
SLM-OS> ls /mnt/files
hello.txt    [f]    20
readme.txt   [f]    74

SLM-OS> cat /mnt/files/hello.txt
Hello from LittleFS!
```

The output format for mounted directories shows:
- Filename
- Type indicator: `[f]` for file, `[d]` for directory
- Size in bytes (for files)

See `docs/filesystem.md` for details on the LittleFS integration.

### Write Commands

The following commands modify files in mounted filesystems:

```
SLM-OS> write /mnt/files/test.txt Hello World
Wrote 11 bytes to /mnt/files/test.txt

SLM-OS> cat /mnt/files/test.txt
Hello World

SLM-OS> append /mnt/files/log.txt First entry
Appended 12 bytes to /mnt/files/log.txt

SLM-OS> append /mnt/files/log.txt Second entry
Appended 13 bytes to /mnt/files/log.txt

SLM-OS> cat /mnt/files/log.txt
First entry
Second entry

SLM-OS> mkdir /mnt/files/subdir
Created directory /mnt/files/subdir

SLM-OS> mv /mnt/files/test.txt /mnt/files/renamed.txt
Moved /mnt/files/test.txt -> /mnt/files/renamed.txt

SLM-OS> rm /mnt/files/renamed.txt
Removed /mnt/files/renamed.txt

SLM-OS> df
Filesystem      Blocks     Used     Free   Use%
/mnt/files         256        4      252     1%
                 1024K     16K    1008K
```

### Streaming Reads

For large files, use offset and length parameters with `cat`:

```
SLM-OS> cat /mnt/files/large.bin 0 64
[offset=0, read=64 bytes]
(first 64 bytes of file)

SLM-OS> cat /mnt/files/large.bin 64 64
[offset=64, read=64 bytes]
(next 64 bytes of file)
```

### File Truncation

Clear or resize files with `truncate`:

```
SLM-OS> truncate /mnt/files/log.txt 0
Truncated /mnt/files/log.txt to 0 bytes

SLM-OS> cat /mnt/files/log.txt
(empty)
```

### File Utility Commands

Additional commands for file operations:

**Copy files:**
```
SLM-OS> cp /mnt/files/hello.txt /mnt/files/backup.txt
Copied 20 bytes: /mnt/files/hello.txt -> /mnt/files/backup.txt
```

**Create empty file:**
```
SLM-OS> touch /mnt/files/newfile.txt
Touched /mnt/files/newfile.txt
```

**Show file information:**
```
SLM-OS> stat /mnt/files/hello.txt
  File: /mnt/files/hello.txt
  Type: regular file
  Size: 20 bytes
```

**Recursive directory tree:**
```
SLM-OS> tree /mnt/files
/mnt/files
  hello.txt  (20 bytes)
  readme.txt  (74 bytes)
  logs/
    app.log  (25 bytes)
```

**Count lines, words, bytes:**
```
SLM-OS> wc /mnt/files/readme.txt
        2       12       74  /mnt/files/readme.txt
```

**Hex dump file contents:**
```
SLM-OS> hexdump /mnt/files/hello.txt 0 32
00000000  48 65 6c 6c 6f 20 66 72  6f 6d 20 4c 69 74 74 6c  |Hello from Littl|
00000010  65 46 53 21                                       |eFS!|
00000014
```

**Search for pattern in file:**
```
SLM-OS> grep Hello /mnt/files/hello.txt
1: Hello from LittleFS!
(1 matches)
```

**Find files by pattern:**
```
SLM-OS> find /mnt/files *.txt
/mnt/files/hello.txt
/mnt/files/readme.txt
(2 files found)

SLM-OS> find /mnt/files log*
/mnt/files/logs/
/mnt/files/logs/app.log
(2 files found)
```

Pattern wildcards:
- `*` matches any sequence of characters
- `?` matches any single character

### Scheduler Command

The `sched` command provides scheduler policy management and statistics:

```
sched                                       - Show current policy name
sched policy                                - List all registered policies
sched policy <name>                         - Switch to named policy
sched stats                                 - Show scheduler statistics
```

**Policy switching example:**
```
SLM-OS> sched
Scheduler policy: heuristic
SLM-OS> sched policy
Available policies (3):
  heuristic (active)
  ai_mlp
  ai_ppo
SLM-OS> sched policy ai_mlp
Switched to policy: ai_mlp
SLM-OS> sched stats
Scheduler Statistics:
  Policy:           ai_mlp
  Tasks:            2
  Ready:            0
  Context switches: 1523
  Timer ticks:      8042
Per-CPU Utilization:
  CPU 0: 12% (965 / 8042 ticks)
  CPU 1: 3% (241 / 8042 ticks)
  CPU 2: 2% (161 / 8042 ticks)
  CPU 3: 1% (80 / 8042 ticks)
```

**Note:** `ai_mlp` and `ai_ppo` policies are only available when the kernel is built with `-DENABLE_AI_SCHEDULER=ON`. The `heuristic` policy is always available.

### Component Command

The `component` command provides management of the component system:

```
component list                              - List all registered components
component builtins                          - List available built-in components
component run <name>                        - Run a built-in component as a task
component swap <old> <new>                  - Hot-swap: replace old with new (preserves subscriptions)
component send <msg>                        - Send message to echo service
component register <name> <ver> <type> [pri] - Register a new component
component unregister <idx>                  - Unregister by index
component status <name|idx>                 - Show component details
```

**Types:** `service`, `driver`, `application`
**Priorities:** `idle`, `low`, `normal`, `high`, `critical`

Example:
```
SLM-OS> component builtins
Built-in Components (5 available):
  Name         Version  Type        Description
  ----         -------  ----        -----------
  counter      1.0      service     Counts to 10 with 500ms intervals
  echo         1.0      service     Echoes IPC messages back to sender
  listener     1.0      service     Listens on 'events' topic via message router
  sensor_monitor 1.0    service     Rule-based sensor threshold monitoring
  digit_classifier 1.0  application MNIST digit classification via inference engine

SLM-OS> component run listener
Component 'listener' v1.0 started (idx=0, task=7)

SLM-OS> component swap listener listener
Hot-swap: 'listener' (idx 0) -> 'listener' (idx 1), 1 subscription(s) transferred

SLM-OS> component list
Registered Components: 1
  Idx  Name                 Version   Type        State       Pri
    1  listener             1.0       service     running     idle
```

### Message Router Command

The `msg` command provides access to the topic-based publish/subscribe message router:

```
msg send <topic> <data>     - Publish a message to a topic
msg list                    - List all topics and their subscribers
msg subscribe <topic> <idx> - Subscribe a component to a topic
```

Example:
```
SLM-OS> component run listener
[listener] Started (component 0), subscribed to 'events'

SLM-OS> msg send events Hello from shell!
[listener] [events] "Hello from shell!" (msg #1)
Message delivered to 1 subscriber(s)

SLM-OS> msg list
Message Router (1 topics):
  Topic 'events' (1 subscribers):
    -> component 'listener' (idx 0)
```

### Network Commands

Network commands are available on QEMU (VirtIO-Net). See `docs/networking.md` for full details.

**Initialize networking:**
```
SLM-OS> net init
Initializing network...
Network initialized successfully

SLM-OS> net status
Network: UP
  Interface: sl0
  IP Address: 10.0.2.15
  Link: connected
```

**Ping a host:**
```
SLM-OS> ping 10.0.2.2
PING 10.0.2.2: 4 packets
Reply from 10.0.2.2: seq=1 time=1 ms
Reply from 10.0.2.2: seq=2 time=0 ms
Reply from 10.0.2.2: seq=3 time=0 ms
Reply from 10.0.2.2: seq=4 time=0 ms

--- 10.0.2.2 ping statistics ---
4 packets transmitted, 4 received, 0% packet loss
rtt min/avg/max = 0/0/1 ms
```

**Show interface configuration:**
```
SLM-OS> ifconfig
sl0: flags=UP,STATIC
     ether 52:54:00:12:34:56
     inet 10.0.2.15  netmask 255.255.255.0
     gateway 10.0.2.2
```

**Show network statistics:**
```
SLM-OS> netstat
Network Statistics:
  RX packets: 42  bytes: 6048
  TX packets: 38  bytes: 3192
  RX errors:  0  dropped: 0
  TX errors:  0
```

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

*Last updated: April 2026*
