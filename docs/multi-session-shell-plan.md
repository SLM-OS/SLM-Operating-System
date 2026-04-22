# Multi-Session Shell Plan (Phases 1 + 2 + 3)

Plan for bringing remote shell access to SLM-OS via TCP, with telnet
as the initial protocol layer and a proper daemon control surface
(`telnetd`) on top. This is foundational work for future SSH support
(Phase 4, tracked in #199) and for multi-user operation.

**Status:** Phases 1 + 2 + 3 complete. `nc localhost 2323` and
`telnet localhost 2323` both get a shell; telnet clients transition
into character-at-a-time server-echoed mode, raw clients see a
12-byte negotiation burst and otherwise behave identically. UART
console coexists; up to `MAX_TCP_SHELL_SESSIONS` concurrent remote
sessions (currently 16). Daemon-style operator controls (`telnetd
start/stop/status/sessions/kick`, `/etc/telnetd.conf` parser,
`NET_TELNETD_AUTOSTART` build flag, `slm.telnetd_*` Lua bindings)
all landed. Phase 4 (SSH, #199) is out-of-scope here.
**Last updated:** 22 April 2026

---

## Goals

- Allow users to open a shell session over TCP from a host machine
- Support multiple concurrent shell sessions on a single SLM-OS instance
- Ship as a long-running task (`telnetd`) that can be started, stopped,
  and configured at runtime, and optionally auto-started at boot
- Build an architecture that accommodates future SSH (#199) and
  multi-user work without rewrites
- No hardware dependencies — fully testable in QEMU

## Non-Goals

- Authentication (deferred to Phase 3 SSH)
- Confidentiality / encryption (deferred to Phase 3 SSH)
- Full PTY emulation (line-mode only; adequate for shell use)
- File transfer protocol (SFTP/SCP deferred)
- Job control, process groups, POSIX compliance (#33)

---

## Dependencies

Hard:
- Networking expansion Phases 1-2 complete (driver abstraction,
  x86-64 VirtIO-Net PCI, lwIP available on target platforms)
- Auto-DHCP at boot (#197) so the OS has an IP address without manual
  configuration — **closed 2026-04-16**, implemented in
  `kernel/net/lwip_slm.c` behind `NET_DHCP_AT_BOOT` (default ON)

Soft:
- Lua audit (#152) — scripting over TCP is more useful if the Lua
  surface is complete, but not required

### lwIP API constraint (important)

SLM-OS configures lwIP with `NO_SYS=1` and `LWIP_SOCKET=0` /
`LWIP_NETCONN=0` (see `kernel/include/lwipopts.h`). The BSD sockets API
(`lwip_socket`, `lwip_bind`, `lwip_accept`, etc.) is **not available**
— enabling it would require a `sys_arch.c` port with threading
primitives (mutexes, mailboxes, semaphores backed by SLM-OS tasks), a
substantial rewrite we do not need.

Instead, the TCP listener (§1.3) and the `shell_io_tcp` backend
(§1.1) use the lwIP **raw callback API** driven by the existing
`net_poll()` task. Existing reference: `kernel/net/lwip_slm.c` (DHCP /
ICMP integration) follows this exact pattern.

---

## Phase 1: Raw TCP Shell

Goal: Connect to SLM-OS from a host terminal (`nc localhost 2323`) and
get an interactive shell. No protocol negotiation — just bytes on the
wire.

### 1.1 Shell I/O Abstraction

**Problem:** The shell reads from UART via `uart_getc()` and writes via
`uart_puts()` directly. There is no abstraction layer between the shell
loop and the physical terminal.

**Fix:** Introduce a `struct shell_io`:

```c
struct shell_io {
    int  (*read_line)(struct shell_io *io, char *buf, size_t max);
    int  (*read_char)(struct shell_io *io);  // non-blocking
    void (*write)(struct shell_io *io, const char *buf, size_t len);
    void (*flush)(struct shell_io *io);
    void (*close)(struct shell_io *io);
    bool (*is_open)(struct shell_io *io);
    void *ctx;
};
```

Provide two implementations:
- `shell_io_uart` — calls into existing UART functions (behavior
  preserved for the primary console)
- `shell_io_tcp` — writes via `tcp_write()` on a lwIP raw TCP pcb;
  reads from a per-session ring buffer that is filled by the pcb's
  `tcp_recv` callback. The `read_char` op blocks by calling
  `task_yield()` until the ring buffer has data or the pcb is closed.

Refactor all shell code to take `struct shell_io *` instead of calling
UART directly. The most-invasive edits are in:
- `kernel/src/shell.c` — main REPL loop and command dispatch
- `kernel/src/lua_shell.c` — Lua REPL
- `kernel/src/lua_slm.c` — `slm.read_line()` and `slm.print()` bindings
- Shell commands that print output (most of `builtin_commands[]`)

**Effort:** ~500 lines refactored. Mechanical but tedious.

### 1.2 Per-Session State

**Problem:** Shell state is global: current working directory, Lua
interpreter state (`lua_State *L`), any command history, shell
configuration. Two concurrent sessions would clobber each other.

**Fix:** Introduce `struct shell_session`:

```c
struct shell_session {
    uint32_t session_id;
    struct shell_io *io;
    char cwd[VFS_PATH_MAX];
    lua_State *lua;          // per-session Lua state
    task_id_t task_id;       // shell task running this session
    uint64_t connected_at;
    struct sockaddr_in peer; // for TCP sessions; zeroed for UART
};
```

Migration strategy:
- Define `shell_session` and `shell_io`
- Create a default `console_session` that uses `shell_io_uart` — boot
  flow creates this session at startup
- Shell REPL takes a `shell_session *` arg; reads/writes go through
  `session->io`
- Lua REPL uses `session->lua` instead of the global
- Current-working-directory commands (`cd`, `pwd`, `ls` without args)
  use `session->cwd`
- Single-session behavior is preserved when no TCP sessions exist

**Effort:** ~300 lines of refactoring plus the new types.

### 1.3 TCP Listener (raw callback API)

Because lwIP is compiled `NO_SYS=1` / `LWIP_SOCKET=0`, the listener is
not a blocking `accept()` loop but a one-time setup that registers an
`accept` callback against a listening pcb. lwIP invokes the callback
from the `net_poll()` context whenever a SYN handshake completes.

```c
static struct tcp_pcb *shell_listen_pcb;

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    struct shell_session *s = session_alloc();
    if (!s) {
        /* Politely send "too many sessions" then close. */
        tcp_write(newpcb, "Too many sessions\r\n", 19, TCP_WRITE_FLAG_COPY);
        tcp_output(newpcb);
        tcp_close(newpcb);
        return ERR_MEM;
    }

    s->io = shell_io_tcp_create(newpcb);        /* wires recv/err/sent callbacks */
    struct task *t = task_create_with_priority("shell-tcp",
                                               shell_session_main, s,
                                               SHELL_PRIORITY);
    shell_session_bind(t, s);
    scheduler_add_task(t);
    return ERR_OK;
}

void tcp_shell_listener_init(uint16_t port, ip_addr_t bind_addr) {
    struct tcp_pcb *pcb = tcp_new();
    tcp_bind(pcb, &bind_addr, port);
    shell_listen_pcb = tcp_listen(pcb);
    tcp_accept(shell_listen_pcb, on_accept);
}
```

The session task (`shell_session_main`) runs the normal REPL via
`shell_run()`. Its `read_char` path yields (e.g. `task_yield()` or a
short `task_sleep_ms()`) while the per-session ring buffer is empty;
the `tcp_recv` callback fills that buffer and wakes the session. See
`kernel/net/lwip_slm.c` for the callback-wiring pattern already used
for DHCP / ICMP ping.

Design choices:
- Port: `2323` (avoids privileged 1-1023; easy to remember; not 23
  because we may want to run real telnet/SSH on standard ports later)
- `MAX_SHELL_SESSIONS = 16` initially (stack + Lua state ≈ 104 KB each)
- Session allocation from a fixed pool — no dynamic memory during a
  demo
- Listener task has its own priority separate from shell tasks

**Effort:** ~150 lines.

### 1.4 Session Task Lifecycle

Each shell task:
1. Receives its `shell_session *` argument
2. Runs the normal shell REPL loop against `session->io`
3. On EOF from `read_line` (peer disconnect) or on shell `exit`
   command, cleans up:
   - Closes the `shell_io` (which closes the lwIP socket)
   - Frees the Lua state
   - Returns the session slot to the pool
   - Task exits

Cleanup must be robust. A peer that drops without FIN (pulled network
cable, client crash) must be detected via TCP keepalive or read
timeout, else session slots leak.

**Effort:** ~200 lines including error paths.

### 1.5 Output Routing

**Decision: console-only for kernel logs, per-session for command output.**

Rationale: Kernel `uart_printf()`, panic messages, INFO logs, and
asynchronous component output all go to UART. TCP shell sessions only
see the output of commands they run themselves. This matches Unix
convention — `dmesg` shows kernel logs, SSH sessions don't receive them
asynchronously.

Alternatives rejected:
- Broadcast to all sessions: too noisy, output gets interleaved with
  user commands
- Per-session opt-in subscription: nice UX but adds code complexity
  that isn't needed for the initial demo; can be added later

Implementation: no change to `uart_printf()` path. The shell REPL's
command output uses `session->io->write()`, so it naturally goes to the
right session.

### 1.6 Concurrent Command Serialization

Most shell commands are read-only views (`tasks`, `mem`, `top`,
`sched stats`) — safe under concurrent access because underlying kernel
data structures already use locking or NC memory.

Commands that mutate global kernel state need serialization:
- `run`, `kill` — task lifecycle
- `sched policy`, `eviction policy` — active policy switching
- `component run`, `component hot_swap` — component lifecycle
- `net set-ip`, `net enable-dhcp` — network reconfig
- VFS mutations (`mkdir`, `rm`, `write`, `cp`, `mv`) — already
  protected by VFS lock

Solution: a single `shell_mutex` acquired around the mutating subset.
Introduce a flag in `struct shell_command` (`.mutates = true`) and let
the dispatcher take the lock. Read-only commands pass through without
contention.

**Effort:** ~50 lines plus annotation of existing commands.

### 1.7 Resource Limits

- `MAX_SHELL_SESSIONS = 16` hard cap
- Per-session stack: 64 KB (matches current `STACK_SIZE`)
- Per-session Lua state: ~32 KB
- Per-session buffers: 4 KB line buffer + 4 KB TCP receive buffer
- Total per TCP session: ~104 KB
- Total fixed overhead at 16 sessions: ~1.6 MB

These fit comfortably in existing memory budgets. If tightened later,
reduce `MAX_SHELL_SESSIONS` or disable Lua for TCP sessions.

### 1.8 Security Notes (Initial)

Raw TCP with no authentication is **not safe on untrusted networks**.

Mitigations for initial implementation:
- Default bind address: `127.0.0.1` (loopback only) — users can change
  to `0.0.0.0` explicitly after understanding the risk
- `NET_TCP_SHELL` build flag (default ON for QEMU, OFF for real
  hardware until Phase 3 SSH lands)
- `ifconfig` output displays TCP shell status prominently
- Startup log: `[WARN] TCP shell active on 2323 (unauthenticated)`

Full security comes with SSH (#199). Raw TCP shell is for lab use
during development and demos on trusted networks.

### 1.9 Testing

All in QEMU:

```
# Launch with port forwarding
make run PLATFORM=X86_64
# In another terminal:
nc localhost 2323
# Should get SLM-OS shell prompt

# Test concurrent sessions:
# Open two nc connections, both should work independently
# `tasks` from each should show 2 shell-tcp tasks plus the console
```

Regression tests:
- Session allocation/deallocation under stress
- Peer disconnect with pending output (no leaks)
- Peer disconnect with active command (graceful cancellation)
- Max-sessions exceeded (clean rejection)
- UART shell continues to work normally with TCP sessions active

Manual tests:
- Run `top` command from two TCP sessions simultaneously — both show
  live updates
- Start a long-running command (`bench smp`), disconnect mid-command,
  verify cleanup
- Kill the TCP listener task, verify existing sessions continue

### 1.10 Phase 1 Deliverable

At the end of Phase 1:
- `nc localhost 2323` connects to an SLM-OS shell
- Up to 16 concurrent sessions, each with its own cwd and Lua state
- UART console continues to work alongside TCP sessions
- All existing shell commands work via TCP
- All existing tests pass
- Backspace/arrow keys/control chars behave as raw bytes (no terminal
  emulation) — this is where Phase 2 helps

### Effort Summary

| Piece | Effort |
|-------|--------|
| I/O abstraction | 1-2 days |
| Per-session state | 1-2 days |
| TCP listener | 0.5 day |
| Session lifecycle + cleanup | 0.5 day |
| Command serialization | 0.5 day |
| Testing + polish | 1-2 days |
| **Phase 1 total** | **~5-7 days** |

---

## Phase 2: Telnet Upgrade

Goal: Proper line-editing, echo control, and control-character
handling over TCP so clients work predictably without dropping to
raw mode.

### 2.1 Why Telnet (and Why Not Just Stay Raw)

Raw TCP works, but:
- Backspace transmits as `\x7f` or `\x08` — server must handle it
- Ctrl+C doesn't interrupt running commands (no signal semantics in
  raw TCP)
- No standard way to communicate terminal size
- Client echoes locally by default, but in raw mode you may want
  server-side echo for passwords later

Telnet solves most of this with a tiny protocol layer — IAC (Interpret
As Command) sequences.

### 2.2 Telnet Protocol Scope

Implement (RFC 854 + selected options):
- IAC framing and escape handling
- Option negotiation: WILL/WONT/DO/DONT for these four options:
  - `ECHO` (RFC 857) — server will echo user input
  - `SUPPRESS-GO-AHEAD` (RFC 858) — makes session character-mode-like
  - `NAWS` (RFC 1073) — negotiate window size; client tells us its
    terminal dimensions for `top` command rendering
  - `TERMINAL-TYPE` (RFC 1091) — client tells us `xterm`/`vt100`/etc;
    useful for ANSI color support
- IAC commands: `IP` (interrupt process) → Ctrl+C equivalent; `AO`
  (abort output) optional

Skip:
- BINARY transmission mode (adds complexity for marginal benefit)
- LINEMODE (RFC 1184) — the complex line-mode option; not needed
- Authentication options (handled by Phase 3 SSH)

### 2.3 Telnet Handler Implementation

State machine layered between the lwIP socket and `shell_io_tcp`:

```c
struct telnet_state {
    enum { T_DATA, T_IAC, T_OPT_WILL, T_OPT_WONT,
           T_OPT_DO, T_OPT_DONT, T_SUBNEG } state;
    uint8_t subneg_buf[64];
    size_t  subneg_len;
    uint16_t window_width;
    uint16_t window_height;
    char term_type[32];
    bool echo_enabled;
};
```

Byte-at-a-time processor:
- `T_DATA`: pass through to shell, except `IAC` → enter `T_IAC`
- `T_IAC`: dispatch on next byte (IAC IAC = literal 0xFF; WILL/WONT/
  DO/DONT → option negotiation; SB → subnegotiation; IP → generate
  Ctrl+C)
- On connection open: send initial option negotiation ("I WILL ECHO,
  I WILL SUPPRESS-GO-AHEAD, please DO NAWS, please DO TERMINAL-TYPE")

### 2.4 Line Editing

Once ECHO is negotiated server-side, the shell handles:
- Backspace (0x7f or 0x08): erase last char, send `\b \b` to client
- Enter (0x0d 0x0a or 0x0d 0x00): submit line
- Tab: defer (command completion is future work)
- Arrow keys (ANSI ESC sequences): defer initially; can later add
  history navigation
- Ctrl+C: received as IAC IP from telnet client; shell command can
  check a session-scoped interrupt flag

### 2.5 Window Size for `top`

NAWS negotiation gives us client terminal width/height. Store in
`shell_session->window_cols` and `->window_rows`. The `top` command
(when implemented per #191) can use these to render correctly.

If the client doesn't support NAWS, default to 80x24.

### 2.6 Terminal Type and Color

TERMINAL-TYPE subnegotiation tells us `xterm-256color`, `vt100`, etc.
Store in `session->term_type`. Commands that use ANSI escapes can
check this — `top` renders colors on `xterm*`, plain text on `vt100` or
`dumb`.

### 2.7 Testing Phase 2

```
telnet localhost 2323
# Or the standard "connect to raw" nc still works — telnet server
# handles both (raw clients just don't negotiate options)

# Verify:
# - Backspace works
# - Ctrl+C interrupts a running `sleep 30`
# - Resizing the terminal causes NAWS updates (re-run `top` to see)
# - TERM="dumb" telnet shows plain output; TERM=xterm shows colors
```

### 2.8 Phase 2 Deliverable

- Standard `telnet` client usable against SLM-OS with proper line
  editing
- Terminal window size respected
- Ctrl+C interrupts shell commands
- ANSI color output when client supports it
- `top` renders correctly with NAWS-provided dimensions
- Raw-TCP clients (`nc`) still work (no negotiated options, falls back
  to default behavior)

### Effort Summary

| Piece | Effort |
|-------|--------|
| IAC state machine | 1 day |
| Option negotiation (ECHO, SGA) | 0.5 day |
| NAWS + TERMINAL-TYPE | 0.5 day |
| Line editing (backspace, Ctrl+C) | 1 day |
| Testing + interop | 1 day |
| **Phase 2 total** | **~3-4 days** |

**Phases 1 + 2 combined: ~8-11 days.**

### Phase 2 implementation notes

Deviations from the plan that landed in the final implementation:

- **Parser lives as a separate module**, not inlined into shell_io_tcp.
  `kernel/src/telnet.c` is pure (no lwIP, no shell_session) with a
  `struct telnet_ops` vtable supplying the caller's inject/send/NAWS/
  TTYPE/interrupt callbacks. Makes the state machine unit-testable
  without a real TCP stack.
- **Telnet responses go direct, not via the TX ring.** From on_recv
  (net_pump context), the parser calls tcp_write for negotiation
  replies. Keeps negotiation bytes ahead of any session output
  queued by the shell task and avoids contending with the shell
  task's TX ring.
- **CR/LF normalization done at the parser, not the shell.** The
  T_CR state in telnet.c swallows a trailing LF or NUL after CR so
  `shell_read_line` sees exactly one submit per keypress whether
  the client sends CR LF (telnet default) or bare LF (raw nc).
- **IAC IP sets a session flag AND injects 0x03.** Both the
  `session->interrupt_requested` flag (for long-running commands
  that poll) and the `^C` byte (for `shell_read_line`'s existing
  cancel path) are triggered, so shell commands need no knowledge
  of telnet.
- **AYT (Are You There) returns `[SLM-OS]`.** Small nicety — telnet
  clients use this to ping a hung connection.
- **Ctrl+C injection into the RX ring is unconditional** on IAC IP
  even when the ring is near-full; if the ring is truly full the
  byte is dropped (same as any other byte) but the session-level
  `interrupt_requested` flag still flips.

---

## Phase 3: Startup & Runtime Control

Goal: the telnet server behaves like a proper daemon — `telnetd` can be
started and stopped from the shell, configured via a VFS file, and
optionally auto-started at boot.

This work is small (~2 days) and can be interleaved with Phase 1 or
Phase 2 rather than sequenced strictly after. It is separated here
because the scope is "making the server operator-controllable" rather
than "making it work."

### 3.1 Compile-Time Gating

Supersedes the `NET_TCP_SHELL` flag from §1.8 once Phase 2 lands:

- `NET_TELNETD=ON` — builds the telnetd sources into the kernel.
  Default ON for QEMU and x86-64. Pi 5 lab/demo builds now also force
  it ON as an explicit trusted-network operator choice; Jetson remains
  gated on networking plus the same security caveat.
- `NET_TELNETD=OFF` — sources not compiled in; runtime control
  commands print "telnetd not built in" and return an error.

Separate flag `NET_TELNETD_AUTOSTART` (default ON for Pi 5 lab/demo
builds, OFF elsewhere) controls whether the boot path starts the
daemon by default. This lets a build include the feature without
silently opening a port on platforms that still leave it disabled.

### 3.2 Boot-Time Auto-Start

Hook point: the boot startup path that spawns the UART shell task
(currently in `kernel/src/demo_init.c` / `kernel/src/main.c`). After
lwIP + DHCP (#197) come up, the boot path consults the auto-start
configuration and spawns `tcp_shell_listener_task` if enabled.

Auto-start config hierarchy (first match wins):

1. **`/etc/telnetd.conf` in VFS** — if present, parse it (§3.5). A
   missing file is not an error.
2. **Lua init script** — if `/boot/init.lua` exists and calls
   `slm.telnetd.start(...)`, that wins.
3. **Compile-time default** — `NET_TELNETD_AUTOSTART`.

### 3.3 Runtime Control (Shell)

New shell commands in `kernel/src/shell_telnetd.c`:

| Command | Effect |
|---|---|
| `telnetd start [port] [bind]` | Launch listener task; default port 2323, bind 127.0.0.1 |
| `telnetd stop` | Set shutdown flag, close listen socket, drain sessions |
| `telnetd status` | Print daemon state, port, bind, session count, uptime |
| `telnetd sessions` | List active sessions (id, peer, connected_at, current command) |
| `telnetd kick <id>` | Force-disconnect a session (cleans up per §1.4) |

All mutating subcommands annotated `.mutates = true` so the shell
dispatcher serializes them (§1.6).

### 3.4 Runtime Control (Lua)

Lua bindings (new, in `kernel/src/lua_slm.c`):

- `slm.telnetd.start(port, bind) -> bool, err`
- `slm.telnetd.stop() -> bool`
- `slm.telnetd.status() -> { port, bind, session_count, uptime }`
- `slm.telnetd.sessions() -> array of session tables`
- `slm.telnetd.kick(session_id) -> bool`

This is what makes `/boot/init.lua`-driven startup work without
forcing the init path through the shell parser.

### 3.5 Config File Format

`/etc/telnetd.conf` — flat `key=value` per line. Keep the parser under
~100 lines of C. Intentionally not TOML/JSON: the project has no
parser for those and doesn't need one for five keys.

```
enabled=true
port=2323
bind=0.0.0.0
max_sessions=16
idle_timeout_sec=600
```

Unknown keys: log a warning and ignore. Malformed lines: log and
continue. Parser is called once during boot — no hot-reload initially.

### 3.6 Observability

Register the daemon with the component runtime so `top`, `ps`, and
`component list` see it:

- `name=telnetd`
- `state=running | stopped | starting | draining`
- `stats=port, bind, sessions_active, sessions_total, bytes_rx, bytes_tx, start_time`

This aligns with the demo-readiness observability work (#191, #194).

### 3.7 Phase 3 Deliverable

- `telnetd start` / `telnetd stop` / `telnetd status` work from the shell
- `/etc/telnetd.conf` controls boot-time behavior
- Daemon visible in `component list` and (when #191 lands) `top`
- Lua scripts can start/stop the daemon and enumerate sessions
- All of Phase 1 + 2 regression tests continue to pass

### Effort Summary

| Piece | Effort |
|---|---|
| Shell commands (`telnetd start/stop/status/...`) | 0.5 day |
| Config file parser | 0.5 day |
| Boot hook + auto-start plumbing | 0.5 day |
| Lua bindings | 0.5 day |
| **Phase 3 total** | **~2 days** |

**Phases 1 + 2 + 3 combined: ~10-13 days.**

### Phase 3 implementation notes

Deviations / design choices that landed:

- **`telnetd` lives in `net_shell.c`**, not a separate
  `shell_telnetd.c`, because it's tightly coupled with `net_init`
  and the existing `net`/`ping`/`ifconfig` commands. `tcpsh` is
  registered as a deprecated alias that dispatches through the
  same handler (uses `argv[0]` for "Usage:" / error prefix).
- **Lua bindings are flat (`slm.telnetd_start`, `slm.telnetd_stop`,
  ...)** rather than the nested `slm.telnetd.start` shape the plan
  mentioned. This matches the project's existing convention
  (`slm.component_run`, `slm.msg_publish`, `slm.sched_policy`).
- **Autostart config precedence:** `/etc/telnetd.conf` always wins
  when present. Missing file → fall back to the
  `NET_TELNETD_AUTOSTART` compile-time default. A Lua
  `/boot/init.lua` hook (plan §3.2 item 2) is not implemented in
  this phase — the build flag + config file cover the common
  cases; scripts that want custom init logic can call
  `slm.telnetd_start(...)` from wherever they like.
- **Kick semantics:** `telnetd kick <id>` sets the session's
  `closed` flag. Long-running commands complete first, then
  `shell_read_line` returns -1 on its next read and the REPL
  exits. The kicked session disappears from `telnetd sessions`
  immediately (foreach filters `closed`) even though the underlying
  TCP connection may still be alive while the active command runs
  to completion. Aligns with typical daemon `kick` semantics.
- **Component registry integration (§3.6) is deferred.** The
  `top`/`ps`/`component list` entries would be valuable but
  require uptime tracking + bytes_rx/tx counters that aren't
  wired up today; left for a follow-up that can coordinate with
  #191/#194.

---

## Key Files

### Existing (to be modified)

- `kernel/src/shell.c` — Main shell REPL (refactor for `shell_io`)
- `kernel/src/lua_shell.c` — Lua REPL (refactor for `shell_io`)
- `kernel/src/lua_slm.c` — Lua bindings (`slm.read_line`, `slm.print`)
- `kernel/include/shell.h` — Shell public API
- `kernel/include/net.h` — Networking API
- `CMakeLists.txt` — Build gates

### New

- ✅ `kernel/include/shell_io.h` — I/O abstraction interface
- ✅ `kernel/include/shell_session.h` — Session struct + pool API
- ✅ `kernel/include/shell_io_tcp.h` — TCP backend entry points
- ✅ `kernel/include/tcp_shell_server.h` — Listener entry points
- ✅ `kernel/src/shell_io.c` — backend-independent helpers
  (`shell_io_puts`, `shell_io_printf`, `shell_io_vprintf`)
- ✅ `kernel/src/shell_io_uart.c` — UART backend
- ✅ `kernel/src/shell_io_tcp.c` — lwIP raw-callback TCP backend
- ✅ `kernel/src/shell_session.c` — Session pool + lifecycle
  (console singleton + TCP pool of size `MAX_TCP_SHELL_SESSIONS`)
- ✅ `kernel/src/tcp_shell_server.c` — Listener (accept callback) + `tcpsh` command glue
- ✅ `kernel/include/telnet.h` (Phase 2) — IAC state machine API
- ✅ `kernel/src/telnet.c` (Phase 2) — IAC state machine
- ✅ `kernel/tests/test_telnet.c` (Phase 2) — 17 parser unit tests
- ✅ `kernel/src/net_shell.c` (Phase 3) — `telnetd` shell commands
  (folded into the existing net command file; `tcpsh` kept as a
  deprecated alias)
- ✅ `kernel/include/telnetd_config.h` + `kernel/src/telnetd_config.c`
  (Phase 3) — `/etc/telnetd.conf` parser
- ✅ `kernel/include/telnetd_autostart.h` +
  `kernel/src/telnetd_autostart.c` (Phase 3) — boot-time hook
- ✅ `kernel/tests/test_shell_session.c` — Session + shell_io unit tests (22 tests)
- ✅ `kernel/tests/test_telnetd_config.c` (Phase 3) — Config parser tests (15 tests)

---

## Phase Boundaries and Demo Readiness

| State | Demo-worthy? |
|-------|--------------|
| Before Phase 1 | UART console only |
| Phase 1 complete (raw TCP) | "Connect from my laptop" demo works with `nc` — crude but impressive |
| Phase 2 complete (telnet) | Polished: `telnet` client works cleanly, supports `top` with proper sizing |
| Phase 3 complete (daemon control) | `telnetd start/stop`, `/etc/telnetd.conf`, auto-start at boot |
| Phase 4 complete (SSH, #199) | Production-quality: `ssh` from any laptop, encrypted, authenticated |

Each phase is independently demo-able. Stopping after Phase 3 yields a
real, working multi-session shell suitable for lab use, configurable
and controllable like a proper service.

---

*Created: 15 April 2026*
