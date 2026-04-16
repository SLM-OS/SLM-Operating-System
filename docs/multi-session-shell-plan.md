# Multi-Session Shell Plan (Phases 1 + 2)

Plan for bringing remote shell access to SLM-OS via TCP, with telnet
as the initial protocol layer. This is foundational work for future
SSH support (Phase 3, tracked in #199) and for multi-user operation.

**Status:** Planning
**Last updated:** 15 April 2026

---

## Goals

- Allow users to open a shell session over TCP from a host machine
- Support multiple concurrent shell sessions on a single SLM-OS instance
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
  configuration

Soft:
- Lua audit (#152) — scripting over TCP is more useful if the Lua
  surface is complete, but not required

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
- `shell_io_tcp` — writes to a lwIP socket, reads from an incoming
  data buffer

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

### 1.3 TCP Listener Task

A long-running task that accepts new TCP connections and spawns shell
sessions.

```c
void tcp_shell_listener_task(void *arg) {
    int listen_sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    bind(listen_sock, ..., TCP_SHELL_PORT);
    listen(listen_sock, MAX_SHELL_SESSIONS);

    while (1) {
        int client = lwip_accept(listen_sock, ...);
        if (client < 0) continue;

        struct shell_session *s = session_alloc();
        if (!s) {
            const char *msg = "Too many sessions\r\n";
            lwip_send(client, msg, strlen(msg), 0);
            lwip_close(client);
            continue;
        }

        s->io = shell_io_tcp_create(client);
        s->task_id = task_create("shell-tcp",
                                 shell_session_main, s,
                                 SHELL_STACK_SIZE, SHELL_PRIORITY);
    }
}
```

Design choices:
- Port: `2323` (avoids privileged 1-1023; easy to remember; not 23
  because we may want to run real telnet/SSH on standard ports later)
- `MAX_SHELL_SESSIONS = 4` initially (stack + Lua state ≈ 64 KB each)
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

- `MAX_SHELL_SESSIONS = 4` hard cap
- Per-session stack: 16 KB (same as existing shell task)
- Per-session Lua state: ~32 KB
- Per-session buffers: 4 KB line buffer + 4 KB TCP receive buffer
- Total per TCP session: ~56 KB
- Total fixed overhead at 4 sessions: ~224 KB

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
- Up to 4 concurrent sessions, each with its own cwd and Lua state
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

- `kernel/include/shell_io.h` — I/O abstraction interface
- `kernel/include/shell_session.h` — Session struct + pool API
- `kernel/src/shell_io_uart.c` — UART backend
- `kernel/src/shell_io_tcp.c` — lwIP socket backend
- `kernel/src/shell_session.c` — Session pool + lifecycle
- `kernel/src/tcp_shell_server.c` — Listener task
- `kernel/src/telnet.c` (Phase 2) — IAC state machine
- `kernel/tests/test_shell_session.c` — Session unit tests

---

## Phase Boundaries and Demo Readiness

| State | Demo-worthy? |
|-------|--------------|
| Before Phase 1 | UART console only |
| Phase 1 complete (raw TCP) | "Connect from my laptop" demo works with `nc` — crude but impressive |
| Phase 2 complete (telnet) | Polished: `telnet` client works cleanly, supports `top` with proper sizing |
| Phase 3 complete (SSH, #199) | Production-quality: `ssh` from any laptop, encrypted, authenticated |

Each phase is independently demo-able. Stopping after Phase 2 yields a
real, working multi-session shell suitable for lab use.

---

*Created: 15 April 2026*
