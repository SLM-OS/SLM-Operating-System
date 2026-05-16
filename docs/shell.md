# Debug Monitor (Shell)

A minimal debug shell for hardware bring-up and demos. Runs on the
UART console by default and — with networking enabled — also accepts
connections over TCP so multiple users (or a user + a script) can
share the same instance.

**Status:** Implemented. Multi-session (TCP) support: Plans §1 + §2
+ §3 complete (see `docs/archive/plans/multi-session-shell-plan.md`). Both `nc
localhost 2323` and `telnet localhost 2323` work with proper telnet
IAC negotiation, per-session state, boot-time autostart (via
`/etc/telnetd.conf` or the `NET_TELNETD_AUTOSTART` build flag),
and Lua scripting hooks (`slm.telnetd_*`). SSH (§4 / #199) is
deferred.

---

## Overview

A simple command loop with table-driven dispatch. The shell runs as a kernel task on CPU 0 and provides interactive access to system status and diagnostics.

The industrial IoT deployment doesn't involve humans typing commands, so this is purely a development/demo tool.

---

## Commands

`help` (no argument) groups commands by category and alphabetizes
within each category. Empty categories (e.g. `Network` on a build
without `ENABLE_NETWORKING`) are skipped. Categories are defined by
`shell_cmd_category_t` in `kernel/include/shell.h`; the order they
appear here matches the enum order. The source-side
`builtin_commands[]` array is also kept grouped + alphabetized so
"where is command X registered" and "where does X show up in `help`"
give the same answer; the regression test
`test_builtin_commands_grouped_and_sorted` enforces that invariant.

```
SLM-OS> help
Available commands:

Shell:
  clear        Clear screen
  help         List available commands
  reboot       Restart the system

Filesystem:
  append       Append to file (append <path> <content>)
  cat          Show file contents (cat <path>)
  cd           Change directory (cd [path])
  cp           Copy file (cp <src> <dst>)
  df           Filesystem stats (df [path])
  find         Find files (find <path> <pattern>)
  grep         Search in file (grep <pattern> <path>)
  hexdump      Hex dump file (hexdump <path> [offset] [len])
  ls           List directory (ls [path])
  mkdir        Create directory (mkdir <path>)
  mv           Move/rename (mv <src> <dst>)
  put          Write binary hex to file (put [-a] <path> <hex>)
  pwd          Print working directory
  rm           Remove file/dir (rm <path>)
  stat         Show file info (stat <path>)
  touch        Create empty file (touch <path>)
  tree         Recursive directory listing (tree [path])
  truncate     Truncate file (truncate <path> <size>)
  wc           Count lines/words/bytes (wc <path>)
  write        Write to file (write <path> <content>)
  xput         Framed upload (xput begin|chunk|status|finish|abort)

System info:
  cpu          Show CPU status
  dtb          Show device tree info
  ipc          Show IPC statistics
  mem          Show memory statistics
  top          Live dashboard (top [-n <iter>] [refresh_secs])
  uptime       Show system uptime
  vmm          Show virtual memory info

Processes & scheduling:
  bench        Performance benchmarks (bench <context|irq|ipc|stats|all>)
  eviction     AI eviction (eviction [policy [<name>] | stats])
  kill         Terminate a task by ID
  model        Model management (load/list/info/unload/pools)
  sched        Scheduler (sched [policy [<name>] | model ... | stats])
  sleep        Sleep for N ms (sleep <ms>)
  tasks        List all tasks

Components & messaging:
  component    Component system (list/register/status)
  msg          Message router (send/list/subscribe)

Scripting & programs:
  elftest      Test ELF loader
  lua          Lua scripting (concurrent-safe REPL or script)
  lua-admin    Lua scripting with global admin bindings
  run          Run a program (run <name>)

Network:                              # only when ENABLE_NETWORKING=ON
  http         HTTP client (get <url> <dest>)
  ifconfig     Network interface config
  net          Network control (init/status)
  netstat      Network statistics
  ping         Send ICMP echo request
  tcpsh        Alias for telnetd (legacy name)
  telemetry    Admin telemetry feed (stats / server start|stop|...)
  telnetd      Telnet shell daemon (start|stop|status|sessions|kick)

Hardware control & diagnostics:       # mix of always-available + platform-gated
  bpmp         BPMP IPC smoke test (Jetson only)
  diag         Pi 5 IRQ-delivery diagnostics (PI5_IRQ_DIAG only)
  gpu          Show GPU info (non-x86); see also nvidia_gpu_register on x86-64
  hailo        Hailo NPU control
  hspdiag      HSP/BPMP doorbell probe (Jetson only)
  irqtest      Pi 5 brief DAIF.I unmask probe (PI5_IRQ_DIAG only — see warning below)
  kernel       Manage staged / active boot kernel
  macbdiag     MACB IRQ delivery diagnostic (Pi 5 + networking)
  nvgpu        Jetson nvgpu bringup (Jetson only)
  pcietrain    Tegra PCIe C8 host init + link train (Jetson only)
  peek         Read physical memory
  poke         Write 32-bit word
  rtldiag      RTL8168 PCIe probe (Jetson + networking)
  timdiag      Timer/interrupt delivery diagnostic (non-x86; subcommands on Pi 5)
  xhci         Tegra XHCI controller info (Jetson only)
  xhcidiag     Tegra XHCI CBB-at-EL2 probe (Jetson + networking)
```

### Adding a new command

1. Pick the right category from `shell_cmd_category_t` in
   `kernel/include/shell.h`.
2. Add the entry to the appropriate block in `builtin_commands[]`
   (kernel/src/shell.c) in alphabetical order. `#if`-gated entries
   are interleaved by name like everything else.
3. Set `category = SHELL_CAT_<...>` in the struct literal. The
   regression test `test_builtin_commands_grouped_and_sorted` will
   fail at the next `make test` if the layout is wrong.
4. External commands (registered via `shell_register_command` from
   net/lua/hailo/kernel/etc.) follow the same convention — set the
   `category` field on each `shell_cmd_t` literal.

### Command Descriptions

| Command | Description |
|---------|-------------|
| `help [cmd]` | List commands, or show detailed help for a specific command |
| `mem` | Show PMM statistics (total pages, free, allocated) |
| `tasks` | List all tasks with ID, state, CPU affinity, priority, and name |
| `cpu` | Show per-core status (online state, current task) |
| `cpu resurrect <N>` | Pi 5 only. Manually resurrect a dormant secondary CPU via `psci_cpu_on`. Drains the target's run queue (any tasks left there are leaked), then re-issues PSCI CPU_ON. Returns 0 on success (incl. cache-incoherency fallback), `-1` invalid CPU id, `-2` ALREADY_ON (software wedge — drain alone was the recovery), `-4` real PSCI failure (CPU left offline). The auto-supervisor (kernel/sched/cpu_supervisor.c, #216 Tier 2) drives this same path automatically when a secondary's `sched_diag_idle_loops` counter is frozen for 6 samples; the manual subcommand exists for integration tests and operator use. Stubs to `-1` on Jetson / QEMU / x86-64. |
| `uptime` | Show system uptime in seconds and milliseconds |
| `vmm` | Show virtual memory statistics (page tables, mapped regions) |
| `ipc` | Show IPC statistics (message queues, shared buffers) |
| `model` | Show model pool stats and loaded model list |
| `model load <path>` | Load an ONNX model from VFS (e.g., `/mnt/files/mnist.onnx`) |
| `model list` | List loaded models with parameter count, weight size, and node count |
| `model info <name\|idx>` | Show detailed information for a loaded model |
| `model unload <name\|idx>` | Unload a model by name or registry index and free its memory |
| `model infer <name\|idx>` | Run inference on model with zero input, print output probabilities and predicted class |
| `model infer-file <name\|idx> <path>` | Run inference with raw little-endian fp32 input read from VFS — file size must match the model's expected input element count (e.g. 3,136 bytes for MNIST 1×1×28×28). Echoes "predicted: N (us)" to the active session and full logits to UART. |
| `model use-gpu <name\|idx> <on\|off>` | Per-model GPU-dispatch toggle (default ON at load). Layered on top of the master `gpu use inference` flag — both must be ON for the engine to route through GPU. Useful for forcing a specific model back to CPU without disturbing the master. |
| `model gpu` | Show GPU status, capabilities, and inference backend |
| `model pools` | Show weight and workspace memory pool statistics |
| `model stats` | Show inference performance statistics (latency, throughput, errors) |
| `model bench <name> [N]` | Benchmark model inference latency (default 10 iterations) |
| `infer batch on\|off` | Enable/disable runtime batched inference dispatch (default OFF). Per the exploratory-OS framing in #848, batching is a runtime-toggleable mode rather than a default behavior change. |
| `infer batch config <size> <us>` | Configure batch threshold (1..32) and partial-batch flush timeout in microseconds (100..100000). Defaults: 8 / 5000. |
| `infer batch status` | Print the dynamic-batching dispatcher state: mode, batch size, timeout, queue depth, and the `SchedulerStats` counter snapshot (`batches_full`, `batches_timeout`, `bypassed_deadline`, `singleton_dispatches`, etc.). |
| `bench infer-stress N [iters]` | Spawn N concurrent task workers (1..32), each running `iters` MNIST inferences (default 50) against the batched dispatcher. Reports aggregate req/s, min/avg/max per-iteration latency, and the per-run counter deltas. Loads MNIST automatically if not already present. |
| `gpu use status` | Tabular view of the three GPU consumer toggles (sched, eviction, inference) with last-change timestamps |
| `gpu use inference <on\|off>` | Master toggle for GPU inference dispatch. Default OFF. On Jetson with `--no-gpu-suspend` kexec + a v6 channel handoff, flipping ON routes the MNIST model through the GA10B fastpath. Other platforms accept the flag but engine still uses CPU NEON. |
| `gpu use sched <on\|off>` | Master toggle for AI-scheduler GPU dispatch (`ai_mlp` policy). When ON and a sched-MLP v6 handoff is in DRAM (Jetson with `--no-gpu-suspend` kexec + `gpu-kernel-sched-mlp` host helper running pre-kexec), every `ai_mlp_assign_cpu` runs the 4-layer forward on GA10B and falls back to CPU NEON only on dispatch error. Default OFF. Other policies (`heuristic`, `ai_ppo`, `ai_hailo`) ignore the flag. |
| `gpu use eviction <on\|off>` | Operator-intent flag for eviction-policy GPU dispatch. Accepted with a "scaffold only" warning today — no eviction policy declares a GPU backend yet. See `docs/design/gpu-policy-models.md` PR-5/PR-6 for the wiring plan. |
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
| `telnetd start [port]` | Start the TCP shell listener (default port 2323) |
| `telnetd stop` | Stop the listener (existing sessions keep running) |
| `telnetd status` | Show listener state, port, and session counts |
| `telnetd sessions` | List active sessions (id, peer ip/port, age) |
| `telnetd kick <id>` | Force-disconnect a session by id |
| `tcpsh ...` | Deprecated alias for `telnetd`; same behaviour |
| `telemetry server start [port]` | Start the telemetry-feed listener (default port 2325). Push-only TCP server bridging `tel.*` msg_router topics to the network. |
| `telemetry server stop` | Stop the telemetry-feed listener |
| `telemetry server status` | Listener state + per-server counters (samples dequeued / delivered / dropped) |
| `telemetry server sessions` | List connected telemetry peers |
| `telemetry server kick <id>` | Force-disconnect a telemetry session |
| `sched` | Show current scheduler policy name |
| `sched policy` | List all registered scheduling policies |
| `sched policy <name>` | Switch to a named scheduling policy |
| `sched stats` | Show scheduler statistics and per-CPU utilization |
| `timdiag` | Timer/interrupt delivery diagnostic — ARM64 only. Dumps timer state, GIC group configuration, CPU interface registers, and SPI group bitmap. Helps investigate whether hardware timer preemption is available on the platform. See `docs/archive/investigations/jetson-preemption-investigation.md` for interpretation. |
| `timdiag fiq` | Same diagnostic but also runs the FIQ delivery test (writes `ICC_IGRPEN0` and unmasks `DAIF.F`). **May crash on Jetson** if TF-A traps Group 0 register access. Use only for investigation. |
| `timdiag sgi` | Pi 5 + `PI5_IRQ_DIAG` only. Self-SGI probe (Track B of #134) — fires SGI 0 to the calling CPU and polls `GICC_HPPIR` to verify the GIC distributor → CPU interface path works. DAIF stays masked the whole time so the IRQ vector never runs. Reports SGI propagation outcome (HPPIR=0 means propagated). See `docs/pi5-armstub-track-c.md` for interpretation. |
| `timdiag bypass` | Pi 5 + `PI5_IRQ_DIAG` only. Bypass-direction probe (Track B of #134) — toggles `GICC_CTLR.IRQBypDisGrp1`/`FIQBypDisGrp1` and observes whether the per-CPU IRQ counter advances over a 50 ms window. **Requires `SECONDARY_PREEMPT=ON`** to run; without it, prints a SKIPPED message because the probe unmasks `DAIF.I` and would otherwise hang Pi 5 hardware (the IRQ vector calls `schedule()` from IRQ context — see #98). |
| `timdiag smc` | Pi 5 + `PI5_IRQ_DIAG` only. SMC fingerprint probe (Track B of #134) — times PSCI_VERSION and a deliberately invalid SMC over 8 round-trips each; reports min/max cycles + microseconds. Establishes the EL3 dispatch cost (relevant for deciding whether a custom-armstub Track C is feasible). Read-only — does not perturb GIC or DAIF state. |
| `irqtest` | Pi 5 + `PI5_IRQ_DIAG` only. Brief DAIF.I unmask probe (Track C Stage 2 / 2.5 of #134). Unmasks `DAIF.I` for ~100 µs and observes whether `diag_vec_counts.irq` advances on the calling CPU. **WARNING: without `SECONDARY_PREEMPT=ON` this command WILL HANG the system if a timer IRQ fires** — that's the diagnostic (the hang itself proves SCR_EL3 IRQ routing works) but operators who run it without context will need to **power-cycle to recover**. The command prints a 5-second-pause warning before unmasking when SECONDARY_PREEMPT is off, giving Ctrl-C a chance. With SECONDARY_PREEMPT on, the probe completes cleanly and reports IRQ-delivered / FIQ-delivered / nothing-delivered. The command also emits checkpoint trace markers '1' through '7' via direct UART writes when built with `STAGE25_TRACE_PI5=ON` so a hang can be pinned to a specific instruction. See `docs/pi5-stage25-irqtest-findings.md`. |
| `irqtest noirq` | Same probe but skips the `daifclr` entirely — control baseline. Confirms whether the `daifclr` itself or some other code path is the trigger when `irqtest` hangs. Always safe to run. |
| `irqtest fiq` | Variant that unmasks both `DAIF.I` and `DAIF.F`. Same hang risk as bare `irqtest`. |
| `lua` | Enter Lua REPL |
| `lua -e "code"` | Execute Lua code directly |
| `lua <file>` | Run Lua script from filesystem |
| `clear` | Clear terminal screen (ANSI escape sequence) |
| `reboot` | Restart system via PSCI (QEMU: triggers exit) |
| `kernel status` | Show boot-media availability, staged image presence, tryboot-armed flag, last activate/promote/rollback outcomes. Pi 5 only — short-circuits with "boot media unsupported" elsewhere. |
| `kernel stage <vfs-path>` | Copy a built image (e.g. `/mnt/files/slmos.bin`) into `0:/slmstore/staged.img` on the FAT boot partition. Drives BCM2712 EMMC2 via `boot_media_acquire/release`. |
| `kernel activate` | Arm Pi 5 tryboot one-shot via BCM mailbox `RPI_FIRMWARE_SET_REBOOT_FLAGS=1` + `RPI_FIRMWARE_NOTIFY_REBOOT`. Requires `tryboot.txt` on boot FAT (see `deploy/pi5/tryboot.txt`). On reboot, firmware loads `tryboot.txt` once, then auto-reverts to `config.txt`. |
| `kernel promote` | After a successful tryboot session: copy `staged.img` over `kernel_2712.img` and clear the staged slot. Makes the swap permanent. |
| `kernel rollback` | Clear tryboot flag (`SET_REBOOT_FLAGS=0`) and remove the staged image. Use before reboot to abort an arming, or after a failed tryboot to clean up. |

### Getting Help

The `help` command has two modes:

**List all commands** (grouped by category, alphabetized within each
group — see the [Commands](#commands) section above for the full
output):
```
SLM-OS> help
Available commands:

Shell:
  clear        Clear screen
  help         List available commands
  reboot       Restart the system

Filesystem:
  append       Append to file (append <path> <content>)
  cat          Show file contents (cat <path>)
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

### NVIDIA GPU Command (x86-64 only)

Registered by `nvidia_gpu_register_shell_commands()` in
`kernel/arch/x86_64/nvidia_gpu.c` when an NVIDIA GPU is discovered
during PCI enumeration. Available subcommands:

| Subcommand | Description |
|---|---|
| `gpu` | Default — print GPU info (chip ID, BARs, PMC_ENABLE, PMC_INTR_HOST) |
| `gpu init` | Run GSP-RM bringup: Phase 0 firmware load → FWSEC-FRTS → Booter Load → RISC-V start. Reports per-phase state and dumps SEC2 diagnostics on failure |
| `gpu sec2` | Read SEC2 + GSP Falcon state directly (CPUCTL, HWCFG2, MAILBOX0/1, OS, DEBUGINFO, BROM) without running bringup. Used to diagnose the SEC2 priv-lock state at any point |
| `gpu vram` | Test VRAM via BAR1 (write pattern, read back) at multiple offsets across the aperture |
| `gpu regs` | Dump key BAR0 registers (PMC, PTIMER, PBUS, PSTRAPS) |

**Hardware-only:** these subcommands no-op gracefully when no NVIDIA
GPU is present (e.g., QEMU). `gpu init` requires the firmware blobs
to be embedded at build time (`ENABLE_GSP_FIRMWARE`). See
`docs/x86-64-gpu-inference-status.md` for the GSP-RM bringup status
and `docs/archive/test-runs/x86-gpu-bringup-2026-04-15.md` for the most recent
hardware validation report.

---

## Multi-Session Shell

The same REPL serves the physical UART and TCP clients. Plans §1–§3
of `docs/archive/plans/multi-session-shell-plan.md` cover the architecture.
Phase 1 landed the TCP backend + REPL plumbing; Phase 2 added the
telnet IAC state machine; Phase 3 wraps it in a `telnetd`-style
daemon with config file, autostart, and Lua bindings. Phase 4
(SSH, #199) is deferred.

### Bringing up TCP sessions

```
slmos> net init              # enable lwIP + DHCP
slmos> telnetd start          # listen on 0.0.0.0:2323
[TCPSH] Listening on 0.0.0.0:2323 (unauthenticated — trusted networks only)
telnetd: listening on port 2323

# From the host, either nc or telnet works:
$ telnet 127.0.0.1 2323
Trying 127.0.0.1...
Connected to 127.0.0.1.
Escape character is '^]'.

SLM-OS Debug Shell (tcp)
Type 'help' for available commands.

slmos> pwd
/
slmos> telnetd status
telnetd: running on port 2323 — accepted=1 active=1 max=2
slmos> telnetd sessions
   ID  Peer IP          Port  Connected
  ---  ---------------  ----  ---------
    1  10.0.2.2         40876  5 s
```

The QEMU Makefile binds `hostfwd=tcp:127.0.0.1:2323-:2323` so the
guest's listener is reachable on the host's loopback only. Real
hardware platforms (Pi 5, Jetson) have no hostfwd — if TCP shell
starts there, it's reachable on the board's LAN IP with no
authentication, so gate carefully.

### Telnet protocol (§2)

On connection the server sends a 12-byte option-negotiation burst
(IAC WILL ECHO, IAC WILL SGA, IAC DO NAWS, IAC DO TERMINAL-TYPE).
Telnet clients respond with their side of the negotiation and
transition to character-at-a-time server-echoed mode. Raw `nc`
clients see the 12 bytes as garbage (0xFF-prefixed) at the top of
the stream and otherwise behave as before — no regression from
Phase 1's raw-TCP shell.

Recognised IAC commands:
- **IAC IAC** — literal 0xFF data byte.
- **IAC IP** (Interrupt Process) — sets the session's
  `interrupt_requested` flag and injects a ^C into the RX ring so
  `shell_read_line` cancels the current input. Long-running
  commands can poll `shell_interrupt_requested()`.
- **IAC AYT** (Are You There) — server replies with `[SLM-OS]`.
- **IAC SB NAWS** (window-size subneg) — updates
  `session->window_cols` / `session->window_rows`. `top` will use
  these once #191 lands; default is 80×24.
- **IAC SB TERMINAL-TYPE** — updates `session->term_type` (e.g.
  `xterm-256color`). Commands can check this to decide whether to
  emit ANSI colour.

The parser handles CR/LF normalization: CR LF and CR NUL both
submit once; bare LF (raw nc) still works.

### Session model

- The console session (UART) is a singleton; each TCP connection
  allocates a slot from a pool of size `MAX_TCP_SHELL_SESSIONS = 16`
  (see `kernel/include/shell_session.h`). When the pool is full the
  listener politely sends "Too many sessions\r\n" and drops.
- Every session carries its own cwd and Lua interpreter slot — `cd`
  in one session does not affect another.
- Kernel logs (`[INFO]`, `[WARN]`, panic output, driver messages)
  stay on UART and do **not** broadcast to TCP sessions. Only command
  output written via `shell_puts` / `shell_printf` reaches the session
  that ran the command.
- Commands that mutate global state (`run`, `kill`, `sched policy`,
  `model load`, etc.) serialize on a priority-inheriting mutex so two
  concurrent sessions can't race each other. Read-only commands
  (`ls`, `mem`, `tasks`) skip the lock. See `.mutates` in each
  command-table entry.

### Daemon control (§3)

The `telnetd` command is the operator interface; `tcpsh` is kept
as a deprecated alias. Subcommands:

| Subcommand | Effect |
|---|---|
| `telnetd start [port]` | Start the listener. Default port 2323. |
| `telnetd stop` | Stop accepting; existing sessions run to EOF. |
| `telnetd status` | Running? Port, accepted-count, active, max. |
| `telnetd sessions` | Table of active sessions with peer + age. |
| `telnetd kick <id>` | Force-disconnect; session exits on next read. |

### Autostart and `/etc/telnetd.conf`

Pi 5 and Jetson lab/demo builds default `NET_TELNETD_AUTOSTART=ON`.
Other platforms still need `cmake -DNET_TELNETD_AUTOSTART=ON` to start
`telnetd` at boot. A `/etc/telnetd.conf` in the VFS can flip that
on/off at runtime and tune the settings:

```
# /etc/telnetd.conf — flat key=value
enabled=true
port=2323
bind=0.0.0.0
max_sessions=16
```

Unknown keys and malformed lines are logged + counted but do not
stop parsing. Missing file → use the compile-time default.

### Lua bindings

```lua
slm.telnetd_start(2323)            -- returns rc (0 on success)
slm.telnetd_stop()                 -- returns true if was running
local s = slm.telnetd_status()      -- { running, port, accepted, active, max }
for _, sess in ipairs(slm.telnetd_sessions()) do
    print(sess.id, sess.peer_ip, sess.peer_port, sess.connected_at)
end
slm.telnetd_kick(1)                 -- returns true if found
```

Flat namespace (`slm.telnetd_*`) to match the project's existing
binding convention.

### Security

Raw TCP has **no authentication** and **no encryption**. The QEMU
hostfwd binds to `127.0.0.1` only. Pi 5 lab/demo images currently
auto-start telnetd by operator choice on trusted networks; other
platforms still require explicit enablement via
`NET_TELNETD_AUTOSTART=ON` or `/etc/telnetd.conf`. Do not expose the
port on untrusted networks until Phase 4 SSH (#199) lands.

---

## Implementation

Located in `kernel/src/shell.c` (core) with a thin I/O abstraction
in `kernel/src/shell_io*.c` and session state in
`kernel/src/shell_session.c`. TCP session support lives in
`kernel/src/shell_io_tcp.c` and `kernel/src/tcp_shell_server.c`.

### Key Functions

| Function | Description |
|----------|-------------|
| `shell_init()` | Initialize shell, create the console session, register built-in commands |
| `shell_start()` | Spawn the console shell task on CPU 0 |
| `shell_run()` | REPL loop — reads from `shell_session_current()->io`, dispatches commands |
| `shell_register_command()` | Register an external command at runtime |
| `shell_session_current()` | Return the session bound to the running task (falls back to console) |
| `shell_session_alloc()` / `_free()` | TCP pool slot management |
| `shell_io_tcp_create()` | Wire a new pcb to a TCP shell_io and its ring buffers (accept callback) |
| `tcp_shell_server_start(port)` | tcp_new/bind/listen on `port`, register accept callback |

### Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                 shell_run  (REPL — task-agnostic)                    │
│            reads/writes via shell_session_current()->io              │
└────────────┬─────────────────────────────────────┬───────────────────┘
             │                                     │
   ┌─────────▼──────────┐                ┌─────────▼─────────┐
   │ console shell task │                │ shell-tcp<N> task │
   │    (UART session)  │                │  (TCP session)    │
   └─────────┬──────────┘                └─────────┬─────────┘
             │                                     │
   ┌─────────▼──────────┐                ┌─────────▼─────────┐
   │  shell_io_uart     │                │   shell_io_tcp    │
   │ (uart_getc/puts)   │                │  rings + lwIP pcb │
   └────────────────────┘                └─────────┬─────────┘
                                                   │
                                   ┌───────────────▼──────────────┐
                                   │ net_pump task: net_poll() →  │
                                   │ tcp_recv cb fills RX ring,   │
                                   │ shell_io_tcp_poll drains TX  │
                                   └──────────────────────────────┘
```

The `shell_io` vtable (read_char, try_read_char, write, flush,
close, is_open) is the only API command handlers need. `shell_*`
wrappers (`shell_puts`, `shell_printf`, `shell_putc`, `shell_getc`)
look up the current session's io and route through it.

Kernel logs and driver output continue to call `uart_*` directly so
they always reach the physical console.

### Command Dispatch

Table-driven dispatch with a `.mutates` flag (mutating commands are
serialized with a priority-inheriting mutex so TCP clients can't race
each other's global-state changes) and a `.category` field that drives
the `help` output grouping:

```c
static const shell_cmd_t builtin_commands[] = {
    /* --- Shell session --- */
    {"clear",  cmd_clear,  "Clear screen",            false, SHELL_CAT_SHELL},
    {"help",   cmd_help,   "List available commands", false, SHELL_CAT_SHELL},
    {"reboot", cmd_reboot, "Restart the system",      true,  SHELL_CAT_SHELL},

    /* --- Filesystem (alphabetized) --- */
    {"append", cmd_append, "Append to file ...",      false, SHELL_CAT_FILESYSTEM},
    {"cat",    cmd_cat,    "Show file contents ...",  false, SHELL_CAT_FILESYSTEM},
    /* ... */

    /* --- Hardware control & diagnostics (with #if-gates inline) --- */
#if defined(PLATFORM_JETSON_ORIN_NANO)
    {"bpmp",   cmd_bpmp,   "BPMP IPC smoke test ...", false, SHELL_CAT_HARDWARE},
#endif
    {"peek",   cmd_peek,   "Read physical memory",    false, SHELL_CAT_HARDWARE},
    {"poke",   cmd_poke,   "Write 32-bit word",       true,  SHELL_CAT_HARDWARE},
    /* ... */
};
```

`shell_register_command()` is still supported for runtime registration
(net, pci, gpu, lua, hailo, kernel); the caller sets `.mutates` and
`.category` appropriately. See `kernel/include/shell.h` for the full
`shell_cmd_category_t` enum.

---

## Line editing and history

The REPL line reader (`shell_read_command` in `kernel/src/shell.c`)
supports:

- **Backspace / DEL** — removes the rightmost character from the
  in-progress line.
- **Ctrl+C** — cancels the current line, prints `^C`, and resets the
  history browse cursor back to the live edit buffer.
- **Up arrow (`ESC [ A`)** — recalls the previous command from the
  per-session history ring; further up arrows walk older. Recall is
  rendered by emitting `\r` + `ESC [ K` + the prompt + the recalled
  text; the cursor lands at end-of-input.
- **Down arrow (`ESC [ B`)** — walks back toward the live edit
  buffer; once past the most recent entry the input region clears.
- **Enter** — submits the (possibly edited) line. The submitted line
  is captured into history per the rules in
  `docs/archive/plans/shell-command-history-plan.md`: empty / whitespace-only lines
  and exact duplicates of the most recent entry are skipped, anything
  longer than 127 chars is truncated.

Each session keeps its own 32-entry × 128-byte ring buffer
(`SHELL_HISTORY_DEPTH` × `SHELL_HISTORY_LINE_MAX` in `shell.h`); two
operators on different telnet sessions never see each other's
recall.

## Not In Scope

The following features are explicitly out of scope:

- Scripting
- Pipes
- Job control
- Reverse search (Ctrl-R), prefix-search history, history expansion
  (`!!` / `!N`)
- Persistent history across reboot
- Left/right arrow in-line cursor movement
- Tab completion

---

*Last updated: April 2026 (multi-session Phase 1).*
