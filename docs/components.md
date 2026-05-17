# Component System

This document describes the SLM-OS component system for modular, hot-swappable functionality.

## Overview

Components are self-contained units of functionality that can be:
- Registered and unregistered at runtime
- Hot-swapped without system restart (stateless swap with subscription preservation)
- Isolated from each other (within kernel-space limits)
- Interconnected via topic-based message passing (pub/sub)

## Current Implementation (Phase 4)

Phase 4 implements the component registry and lifecycle management foundation:

- **Component Registry**: Thread-safe storage for up to 16 components
- **Lifecycle States**: Loaded → Initializing → Running → Suspended → Terminating → Unloaded
- **Shell Commands**: Manual component management via shell
- **C/Rust FFI**: C API wrapping Rust implementation

### What's Implemented

| Feature | Status | Notes |
|---------|--------|-------|
| Component registration | ✅ | Via shell or C API |
| Component unregistration | ✅ | Clean removal from registry |
| Lifecycle state tracking | ✅ | All states supported |
| State transitions | ✅ | Any valid transition |
| Component lookup (by name) | ✅ | O(n) scan |
| Thread-safe registry | ✅ | Spinlock protected |
| Shell commands | ✅ | list, builtins, run, swap, send, register, unregister, status |
| Built-in services | ✅ | counter, echo, listener |
| Message router (pub/sub) | ✅ | Topic-based, yield-based delivery |
| Echo IPC (shared mailbox) | ✅ | Atomic ready/ack, round-robin scheduling |

### Built-in Component Services

| Service | Description | IPC Method |
|---------|-------------|------------|
| `counter` | Counts to 10 with 500ms intervals | None (standalone) |
| `echo` | Echoes messages sent via `component send` | Shared mailbox (atomic flags) |
| `listener` | Prints messages from `events` topic | Message router (pub/sub) |

### Message Router

The message router (`runtime/src/msg_router.rs`) provides topic-based publish/subscribe messaging between components. Implemented in Rust with `#[no_mangle] extern "C"` FFI, components subscribe to named topics and receive messages via per-subscriber mailboxes with atomic ready/ack flags. See `docs/ipc.md` for the live narrative (and `docs/archive/plans/m7-message-router.md` for the original Phase-5 implementation log).

**Wildcard subscriptions:** Topics ending in `*` match all topics with the given prefix. For example, subscribing to `"/sensors/*"` receives messages published to `"/sensors/data"`, `"/sensors/temp"`, etc.

**Message priority:** `msg_router_publish_priority(topic, data, priority)` publishes with explicit priority (0 = normal, higher = more urgent). When a component has multiple pending messages, `msg_router_receive` returns the highest-priority one first.

Shell commands: `msg send <topic> <data>`, `msg list`, `msg subscribe <topic> <idx>`

### Hot-Swap

The `component_hot_swap(old_name, new_name)` function replaces a running component with a new instance while preserving all topic subscriptions. The process:

1. Saves the old component's topic subscriptions via `msg_router_get_subscriptions()`
2. Sets the old component state to `COMPONENT_UPDATING`
3. Removes old subscriptions and unregisters the old component
4. Runs the new component via `component_run()`
5. Re-subscribes the new component to all saved topics

Shell command: `component swap <old_name> <new_name>`

**Limitations:** This is a stateless swap — no internal state is transferred between old and new versions. State transfer is deferred to Phase 5+.

### Subscription Cleanup

When a component's task exits, `component_task_cleanup()` automatically calls `msg_router_unsubscribe_all()` to remove all of the component's topic subscriptions. Empty topics (no remaining subscribers) are reclaimed. This prevents orphaned subscriptions from accumulating after component unloads.

### Deferred to Phase 5+

| Feature | Reason |
|---------|--------|
| ELF loading from manifest | Requires component binary format |
| Dependency resolution | Requires manifest dependencies |
| Hot-swap with state transfer | Stateless swap works; stateful transfer needs explicit API |
| Component isolation | Requires user space separation |

## Component Manifest Format

**Design Decision:** Simple key-value format (not YAML/TOML/JSON).

*Rationale:* No `no_std` YAML parser exists for Rust. Key-value is trivial to parse and sufficient for current needs. Can upgrade format when features require it.

### Current Format

```
name=example-component
version=1.0.0
type=service
priority=normal
```

### Supported Fields

| Field | Required | Values | Description |
|-------|----------|--------|-------------|
| `name` | Yes | alphanumeric + hyphens | Unique component identifier (max 31 chars) |
| `version` | Yes | X.Y.Z | Semantic version (max 15 chars) |
| `type` | Yes | `service`, `driver`, `application` | Component category |
| `priority` | No | `low`, `normal`, `high` | Task priority (default: `normal`) |

### Future Fields (Phase 5+)

```
# Dependencies (planned)
dep.0=logger
dep.1=memory-pool

# Resources (planned)
memory_kb=256
model_blocks=0

# Binary (planned)
binary=example.elf
```

### Component Types

| Type | Description |
|------|-------------|
| `service` | Background service providing functionality to other components |
| `driver` | Hardware interface component (sensors, actuators, etc.) |
| `application` | User-facing application or inference pipeline |

### Priority Levels

| Priority | Value | Use Case |
|----------|-------|----------|
| `idle` | 0 | Background maintenance tasks |
| `low` | 2 | Non-time-critical services |
| `normal` | 4 | Default for most components |
| `high` | 6 | Time-sensitive processing |
| `critical` | 7 | Real-time or safety-critical |

## Component Lifecycle

```
                        ┌─────────────┐
                        │   Loaded    │
                        └──────┬──────┘
                               │ component_init()
                               ▼
                        ┌─────────────┐
                        │Initializing │
                        └──────┬──────┘
                               │ ready
                               ▼
         suspend         ┌─────────────┐         unload
      ┌─────────────────│   Running   │──────────────────┐
      │                  └──────┬──────┘                  │
      ▼                         │ hot_swap                ▼
┌─────────────┐                 ▼                  ┌─────────────┐
│  Suspended  │          ┌─────────────┐          │ Terminating │
└──────┬──────┘          │  Updating   │          └──────┬──────┘
       │ resume          └──────┬──────┘                 │
       └───────────────────────►│◄───────────────────────┘
                                │ complete
                                ▼
                         ┌─────────────┐
                         │  Unloaded   │
                         └─────────────┘
```

### State Transitions

| From | To | Trigger | Description |
|------|----|---------|-------------|
| - | Loaded | `component_load()` | Binary loaded into memory |
| Loaded | Initializing | automatic | Component's init function called |
| Initializing | Running | ready signal | Component active and processing |
| Running | Suspended | `component_suspend()` | Temporarily paused |
| Suspended | Running | `component_resume()` | Resumed from suspension |
| Running | Updating | `component_hot_swap()` | Being replaced by new version |
| Updating | Unloaded | complete | Old version cleaned up |
| Running | Terminating | `component_unload()` | Shutdown initiated |
| Terminating | Unloaded | cleanup complete | Resources freed |

## Component API

### C API (kernel/include/component.h)

```c
/* Registration */
int component_register(const char *name, const char *version,
                       uint8_t type, uint8_t priority);
int component_unregister(uint32_t index);

/* Query */
int component_find(const char *name);
int component_get_info(uint32_t index, component_info_t *info);
uint32_t component_count(void);

/* State management */
int component_set_state(uint32_t index, uint8_t state);

/* Runtime operations */
int component_run(const char *name);
int component_hot_swap(const char *old_name, const char *new_name);
int component_send_echo(const char *message);

/* Helper functions */
const char *component_state_name(uint8_t state);
const char *component_type_name(uint8_t type);

/* Initialization (called during kernel boot) */
void component_init(void);
```

### Component Types

```c
#define COMPONENT_TYPE_SERVICE     0
#define COMPONENT_TYPE_DRIVER      1
#define COMPONENT_TYPE_APPLICATION 2
```

### Component States

```c
#define COMPONENT_LOADED       0
#define COMPONENT_INITIALIZING 1
#define COMPONENT_RUNNING      2
#define COMPONENT_SUSPENDED    3
#define COMPONENT_UPDATING     4
#define COMPONENT_TERMINATING  5
#define COMPONENT_UNLOADED     6
```

### Priority Levels

```c
#define COMPONENT_PRIORITY_LOW    0
#define COMPONENT_PRIORITY_NORMAL 1
#define COMPONENT_PRIORITY_HIGH   2
```

### Rust Implementation

The component system is implemented in Rust (`runtime/src/component/`):

| File | Purpose |
|------|---------|
| `mod.rs` | FFI entry points (`rust_component_*` functions) |
| `state.rs` | `ComponentState`, `ComponentType`, `Priority`, `ComponentInfo` |
| `registry.rs` | Thread-safe component registry with spinlock |
| `manifest.rs` | Key-value manifest parser |

## Shell Commands

```
component list                              # List all registered components
component builtins                          # List available built-in components
component run <name>                        # Run a built-in component as a task
component swap <old> <new>                  # Hot-swap: replace old with new
component send <msg>                        # Send message to echo service
component register <name> <version> <type>  # Register a new component
component unregister <index>                # Unregister by slot index
component status <name|index>               # Show component details

msg send <topic> <data>                     # Publish message to a topic
msg list                                    # List topics and subscribers
msg subscribe <topic> <idx>                 # Subscribe component to topic
```

### Examples

```
slmos> component builtins
Built-in Components (3 available):
  counter      1.0      service     Counts to 10 with 500ms intervals
  echo         1.0      service     Echoes IPC messages back to sender
  listener     1.0      service     Listens on 'events' topic via message router

slmos> component run listener
Component 'listener' v1.0 started (idx=0, task=7)
[listener] Started (component 0), subscribed to 'events'

slmos> msg send events Hello from shell!
[listener] [events] "Hello from shell!" (msg #1)
Message delivered to 1 subscriber(s)

slmos> component swap listener listener
Hot-swap: 'listener' (idx 0) -> 'listener' (idx 1), 1 subscription(s) transferred

slmos> component list
Registered Components: 1
  Idx  Name                 Version   Type        State       Pri
    1  listener             1.0       service     running     idle
```

## Example Component

### Manifest Format (Phase 4)

```
name=hello-world
version=1.0.0
type=application
priority=low
```

### Future: Full Component (Phase 5+)

```
components/
└── hello-world/
    ├── manifest.txt    # Key-value manifest
    └── hello.elf       # Component binary
```

## Testing

The component system has comprehensive tests in `kernel/tests/test_component.c`:

| Test Category | Tests |
|---------------|-------|
| Registration | basic, types, null params, invalid type, exhaustion |
| Unregistration | basic, invalid index, empty slot, double-unregister |
| Query | get_info, null pointer, invalid index, empty slot, find by name |
| State transitions | valid transitions, invalid state, invalid index |
| Slot reuse | verify unregistered slots can be reused |
| Helpers | state_name, type_name |

Run tests with:
```bash
make BUILD_DIR=/c/temp/slmos-build test
```

## Integration with VFS

The `/components/` directory is mounted in the VFS:

```
/components/           # Mount point for component system
```

*Note:* Per-component virtual files (status, version, stats) are planned for Phase 5+.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         Shell Commands                          │
│                  (component list/register/etc.)                 │
└─────────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                    C API (component.h/.c)                       │
│              component_register, component_find, etc.           │
└─────────────────────────────────────────────────────────────────┘
                               │
                               ▼ FFI
┌─────────────────────────────────────────────────────────────────┐
│                  Rust Runtime (runtime/src/component/)          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │   mod.rs     │  │  registry.rs │  │  manifest.rs │          │
│  │  FFI entry   │  │  Component   │  │   Key-value  │          │
│  │   points     │  │   storage    │  │    parser    │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
│                           │                                     │
│                    ┌──────────────┐                             │
│                    │   state.rs   │                             │
│                    │ ComponentInfo│                             │
│                    │    enums     │                             │
│                    └──────────────┘                             │
└─────────────────────────────────────────────────────────────────┘
```

---

*Created: December 2025*
*Updated: April 2026 — Hot-swap, subscription cleanup, message routing complete*
