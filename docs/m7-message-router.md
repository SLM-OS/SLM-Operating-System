# M7 MessageRouter Implementation Log

**Goal:** Implement topic-based publish/subscribe message routing for inter-component IPC.

---

## Architecture

The MessageRouter provides a topic-based pub/sub system for components:

```
Shell                     MessageRouter                Components
  |                           |                            |
  | msg send events "hello"   |                            |
  |-------------------------->|                            |
  |                           | deliver to subscribers     |
  |                           |--------------------------->|
  |                           |        ack                 |
  |                           |<---------------------------|
  |  delivered to 1           |                            |
  |<--------------------------|                            |
```

### Data Structures

- **msg_topic**: Named topic with up to 4 subscribers
- **msg_subscription**: Links a component_idx to a per-subscriber mailbox
- **msg_mailbox**: Per-subscriber message buffer with atomic `ready`/`ack` flags

### API

| Function | Description |
|----------|-------------|
| `msg_router_init()` | Initialize router (called during boot) |
| `msg_router_subscribe(topic, idx)` | Subscribe component to topic (auto-creates topic) |
| `msg_router_publish(topic, data)` | Publish to all subscribers, wait for ack |
| `msg_router_receive(idx, topic_out)` | Check for pending message |
| `msg_router_ack(idx)` | Acknowledge received message |
| `msg_router_unsubscribe_all(idx)` | Remove all subscriptions for a component (reclaims empty topics) |
| `msg_router_get_subscriptions(idx, names, count, max)` | Query which topics a component is subscribed to |
| `msg_router_list()` | Print topics and subscribers |

### Shell Commands

| Command | Description |
|---------|-------------|
| `msg send <topic> <data>` | Publish a message to a topic |
| `msg list` | List all topics and subscribers |
| `msg subscribe <topic> <idx>` | Subscribe a component to a topic |

---

## Subscription Lifecycle

1. **Subscribe:** Component calls `msg_router_subscribe(topic, idx)` during initialization. Topic is auto-created if it does not exist.
2. **Publish/Receive:** Messages published to a topic are copied into each subscriber's mailbox. Subscribers poll via `msg_router_receive()` and acknowledge via `msg_router_ack()`.
3. **Unsubscribe on unload:** When a component's task exits, `component_task_cleanup()` calls `msg_router_unsubscribe_all(idx)` to remove all subscriptions. Topics with no remaining subscribers are reclaimed.
4. **Hot-swap preservation:** During `component_hot_swap()`, subscriptions are saved via `msg_router_get_subscriptions()`, the old component is unregistered, the new component is started, and saved subscriptions are re-applied to the new component index.

---

## Implementation Progress

### Step 1: Core msg_router.c (Complete)

Wrote `kernel/src/msg_router.c` with:
- Topic registry (8 topics max, 4 subscribers each)
- Per-subscriber mailboxes with atomic ready/ack flags
- yield()-based polling with 5-second timeout for publish ack
- String helpers (no libc dependency)

### Step 2: Build Integration (Complete)

- Added `msg_router.c` to `CMakeLists.txt`
- Added `msg_router_init()` call in `main.c` (after component_system_init)
- Boot log shows: `[INFO] Message router: OK`

### Step 3: Shell Commands (Complete)

- Added `cmd_msg` to `shell_component.c` with send/list/subscribe subcommands
- Added `msg` to shell command table in `shell.c`
- Registered in `shell_internal.h`

### Step 4: Listener Service (Complete)

Created built-in "listener" component in `component_runtime.c`:
- Subscribes to "events" topic on startup
- Polls `msg_router_receive()` in a yield loop
- Prints received messages with topic and sequence number
- 60-second idle timeout (resets on activity)

### Step 5: UART yield() Fix (Complete)

**Root cause:** `uart_getc()` on x86-64 busy-looped without yielding. Background tasks (listener, echo) never got CPU time while the shell was waiting for input.

**Fix:** Added `yield()` call in the `uart_getc()` polling loop (`kernel/drivers/uart_x86.c`). The shell now round-robins with component tasks during input wait.

### Step 6: Tests (Complete)

Added 20 tests to `test_x86_boot.c`:

**Message Router Unit Tests (18):**
1. `test_msg_router_init_succeeds` — init + list doesn't crash
2. `test_msg_router_subscribe_creates_topic` — subscribe to new topic
3. `test_msg_router_subscribe_multiple` — 2 subscribers same topic
4. `test_msg_router_subscribe_multiple_topics` — subscribers on different topics
5. `test_msg_router_receive_no_message` — receive returns NULL when empty
6. `test_msg_router_receive_unsubscribed` — receive returns NULL for non-subscriber
7. `test_msg_router_publish_no_subscribers` — publish to nonexistent topic
8. `test_msg_router_list_no_crash` — list with multiple topics
9. `test_msg_router_subscribe_max_per_topic` — 5th subscriber on topic returns -1
10. `test_msg_router_subscribe_max_topics` — 9th topic returns -1
11. `test_msg_router_ack_no_pending` — ack with no message doesn't crash
12. `test_msg_router_receive_null_topic_out` — NULL topic_out parameter safe
13. `test_msg_router_reinit_clears_state` — re-init wipes all subscriptions
14. `test_msg_router_publish_existing_topic_no_ack` — publish timeout path doesn't crash
15. `test_msg_router_shell_subscribe_command` — `msg subscribe` via shell
16. `test_msg_router_shell_send_no_topic` — `msg send` to nonexistent topic returns -1
17. `test_msg_router_shell_command_registered` — `msg list` shell command works
18. `test_component_run_listener` — listener starts and registers successfully

**Component Service Tests (2):**
19. `test_component_run_listener` — listener starts and registers successfully
20. `test_component_run_echo_and_send` — echo service starts, send doesn't crash

All 20 C-side tests pass. Plus 10 Rust-native tests in `rust_run_tests()` (30 total).

### Step 7: Makefile Platform Fixes (Complete)

Fixed the top-level Makefile to support `PLATFORM=X86_64`:
- **Toolchain selection**: x86-64 uses `cmake/toolchain-x86_64-none-elf.cmake`
- **QEMU binary**: `qemu-system-x86_64` (not aarch64)
- **QEMU settings**: `q35` machine, `max` CPU, 256M RAM
- **Rust target**: `x86_64-unknown-none`
- **Test execution**: Creates GRUB ISO, uses `isa-debug-exit` device
- **Exit code mapping**: `isa-debug-exit` returns `(code << 1) | 1`, so success = exit 1

### Step 8: Semihosting for x86-64 (Complete)

Replaced x86-64 semihosting stub (infinite `hlt`) with QEMU `isa-debug-exit`:
- Port 0x501, writes `(exit_code << 1) | 1`
- Test harness exits cleanly, no more timeout-based termination

---

## QEMU Verification

### Message Router (Interactive)

```
slmos> component run listener
Component 'listener' v1.0 started (idx=0, task=7)
[listener] Started (component 0), subscribed to 'events'

slmos> msg send events Hello from shell!
[listener] [events] "Hello from shell!" (msg #1)
Message delivered to 1 subscriber(s)

slmos> msg send events Second message
[listener] [events] "Second message" (msg #2)
Message delivered to 1 subscriber(s)
```

### Multi-Service (Listener + Echo)

```
slmos> component run listener
[listener] Started (component 0), subscribed to 'events'

slmos> component run echo
[echo] Started (component 1), listening for messages...

slmos> msg send events Multi-task test
[listener] [events] "Multi-task test" (msg #1)
Message delivered to 1 subscriber(s)

slmos> component send Echo still works!
[echo] Received: "Echo still works!" (msg #1)
Message delivered to echo service
```

Three tasks (shell, listener, echo) round-robin on CPU 0 with IDLE priority.

---

## Files Changed

| File | Change |
|------|--------|
| `kernel/src/msg_router.c` | New — message router implementation |
| `kernel/src/component_runtime.c` | Added listener service |
| `kernel/src/shell_component.c` | Added `cmd_msg` handler |
| `kernel/src/shell.c` | Added `msg` to command table |
| `kernel/include/shell_internal.h` | Added `cmd_msg` declaration |
| `kernel/src/main.c` | Added `msg_router_init()` to boot sequence |
| `kernel/drivers/uart_x86.c` | Added `yield()` in `uart_getc()` |
| `kernel/drivers/timer.c` | Added `pit_ticks` for ARM64 cross-platform compat |
| `kernel/src/semihosting.c` | x86-64: use `isa-debug-exit` |
| `kernel/tests/test_x86_boot.c` | 20 C-side message router + component tests |
| `runtime/src/lib.rs` | 10 Rust-native message router tests in rust_run_tests() |
| `CMakeLists.txt` | Added `msg_router.c` to build |
| `Makefile` | Platform-aware toolchain, QEMU, and test execution |
| `runtime/src/msg_router.rs` | Rust implementation (replaces C version) |
| `runtime/src/lib.rs` | Added `msg_router` module |
| `docs/x86-64-port.md` | M7 subsection, feature matrix, test counts |
| `docs/components.md` | Message router integration, built-in services |
| `TODO_-_PHASE_4X.md` | Inter-component IPC section |
| `CLAUDE.md` | x86-64 build commands and platform selection |

## Rust Implementation Notes

The message router was initially implemented in C (`msg_router.c`) and later
ported to Rust (`runtime/src/msg_router.rs`). The Rust version:

- Exports the same `#[no_mangle] extern "C"` symbols as the C version
- Uses `core::sync::atomic::AtomicU32` for mailbox ready/ack flags
- Accesses `pit_ticks` via `core::ptr::read_volatile` (ISR-modified)
- Calls `yield()` (renamed to `sched_yield` via `#[link_name]` since `yield` is a Rust keyword)
- Calls `uart_printf` for formatted output and `uart_puts` for simple strings
- Uses `component_get_info` from the Rust component module for subscriber name lookup

The C `msg_router.c` is kept in the repo but excluded from the build via CMakeLists.txt.
The Rust staticlib is linked with `--whole-archive`, making all symbols available to C.
