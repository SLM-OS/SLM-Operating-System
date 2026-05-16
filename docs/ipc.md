# Inter-Process Communication (IPC)

This document describes the IPC subsystem in SLM-OS, including message queues and shared buffers.

---

## Overview

SLM-OS provides a two-tier IPC architecture:

1. **Control Plane: Message Queues** — Small, fixed-size messages (default 64 bytes) for signaling, commands, and buffer handles
2. **Data Plane: Shared Buffers** — Zero-copy shared memory for large data transfers (tensors, model weights)

This design optimizes for SLM workloads where control messages are small but data transfers can be megabytes.

```
┌─────────────────────────────────────────────────────────────────────┐
│                         IPC Architecture                            │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│    ┌──────────────┐         Message Queue          ┌──────────────┐ │
│    │   Producer   │ ──── (64-byte messages) ────── │   Consumer   │ │
│    │     Task     │                                │     Task     │ │
│    └──────────────┘                                └──────────────┘ │
│           │                                               │         │
│           │  (buffer handle in message)                   │         │
│           ▼                                               ▼         │
│    ┌─────────────────────────────────────────────────────────────┐ │
│    │                  Shared Buffer (2MB+)                       │ │
│    │              (Zero-copy data transfer)                      │ │
│    └─────────────────────────────────────────────────────────────┘ │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Message Queues

### Design

Message queues are implemented as fixed-size ring buffers with:

- **Configurable message size** — Default 64 bytes (cache-line aligned), can be any size ≥ 4 bytes
- **Fixed capacity** — Set at creation time
- **Blocking semantics** — Sleep/wake for efficiency (no busy-waiting)
- **Thread-safe** — Protected by spinlocks

### API

```c
#include "ipc.h"

/* Create a queue with 16 slots, 64-byte messages */
struct msg_queue *q = msg_queue_create(16, 0);  /* 0 = MSG_SIZE_DEFAULT */

/* Create a queue with custom message size */
struct msg_queue *q = msg_queue_create(8, sizeof(struct my_message));

/* Send a message (blocking) */
struct slm_message msg = { .type = MSG_TYPE_REQUEST, ... };
int ret = msg_send(q, &msg, MSG_WAIT_FOREVER);

/* Send a message (non-blocking) */
ret = msg_send(q, &msg, MSG_NO_WAIT);
if (ret == IPC_ERR_FULL) {
    /* Queue is full, handle appropriately */
}

/* Receive a message (blocking) */
ret = msg_recv(q, &msg, MSG_WAIT_FOREVER);

/* Receive a message (non-blocking) */
ret = msg_recv(q, &msg, MSG_NO_WAIT);
if (ret == IPC_ERR_EMPTY) {
    /* Queue is empty */
}

/* Check queue depth */
size_t pending = msg_queue_count(q);

/* Find queue by ID (for sharing between tasks) */
struct msg_queue *found = msg_queue_lookup(queue_id);

/* Destroy queue */
msg_queue_destroy(q);
```

### Default Message Structure

The `struct slm_message` is the recommended format for SLM-OS messages:

```c
struct slm_message {
    uint32_t type;              /*  4 bytes: Message type identifier */
    uint32_t flags;             /*  4 bytes: Message-specific flags */
    uint32_t sender_id;         /*  4 bytes: Sending task ID */
    uint32_t reserved;          /*  4 bytes: Reserved */
    union {                     /* 48 bytes: Payload */
        uint8_t raw[48];
        struct {
            uint32_t buffer_handle;
            uint32_t offset;
            uint32_t length;
            uint8_t  extra[36];
        } buffer_ref;           /* For shared buffer references */
        struct {
            int32_t  status;
            uint32_t value;
            uint8_t  data[40];
        } response;
    } payload;
};
```

Total size: 64 bytes (matches typical CPU cache line).

### Error Codes

| Code | Meaning |
|------|---------|
| `IPC_OK` (0) | Success |
| `IPC_ERR_NOMEM` (-1) | Out of memory |
| `IPC_ERR_FULL` (-2) | Queue full (non-blocking send) |
| `IPC_ERR_EMPTY` (-3) | Queue empty (non-blocking recv) |
| `IPC_ERR_TIMEOUT` (-4) | Operation timed out |
| `IPC_ERR_INVALID` (-5) | Invalid parameter |
| `IPC_ERR_BUSY` (-6) | Resource busy |

---

## Priority-Based Message Queues

### Overview

Message queues support 8 priority levels (0-7), allowing high-priority messages to be received before lower-priority ones. This is essential for SLM workloads where inference requests may need to preempt batch processing.

```
Priority Levels:
┌─────────────────────────────────────────────────────────────────────┐
│  7 │ Highest   │ Critical system messages                          │
│  6 │ High      │ Urgent inference requests                         │
│  5 │           │                                                   │
│  4 │ Normal    │ Default priority (used by msg_send())             │
│  3 │           │                                                   │
│  2 │ Low       │ Background tasks                                  │
│  1 │           │                                                   │
│  0 │ Lowest    │ Best-effort, batch processing                     │
└─────────────────────────────────────────────────────────────────────┘
```

### API

```c
#include "ipc.h"

/* Send with explicit priority (0-7, higher = more urgent) */
int ret = msg_send_priority(queue, &msg, MSG_NO_WAIT, 7);  /* Highest priority */

/* Standard msg_send() uses default priority (4) */
ret = msg_send(queue, &msg, MSG_NO_WAIT);  /* Priority 4 */

/* Receive always returns highest-priority message first */
ret = msg_recv(queue, &msg, MSG_NO_WAIT);
```

### Priority Levels

| Level | Constant | Typical Use |
|-------|----------|-------------|
| 7 | `MSG_PRIORITY_HIGHEST` | System-critical messages |
| 6 | `MSG_PRIORITY_HIGH` | Urgent requests |
| 4 | `MSG_PRIORITY_NORMAL` | Default (standard msg_send) |
| 2 | `MSG_PRIORITY_LOW` | Background processing |
| 0 | `MSG_PRIORITY_LOWEST` | Best-effort, batch work |

### Starvation Prevention

To prevent low-priority messages from being starved indefinitely, the queue implements a threshold-based serving policy:

- After 16 consecutive high-priority messages are received
- One low-priority message is served (if available)
- Counter resets and high-priority serving resumes

This ensures all priorities eventually make progress while still prioritizing urgent messages.

### Implementation

Internally, each queue maintains 8 separate ring buffers (one per priority level):

```c
struct msg_queue {
    struct ring_buffer priority_rings[8];  /* Per-priority FIFOs */
    uint32_t total_pending;                 /* Messages across all levels */
    uint32_t starvation_counter;            /* Tracks consecutive high-pri */
    /* ... */
};
```

**Receive algorithm:**
1. Scan from highest priority (7) to lowest (0)
2. Return first message found
3. If starvation_counter >= 16, check low priorities first
4. Reset counter after serving a low-priority message

**FIFO within priority:** Messages at the same priority level are served in FIFO order.

---

## Shared Buffers

### Design

Shared buffers provide zero-copy access to large memory regions:

- **2MB alignment** — Matches VMM block size for efficient mapping
- **Reference counting** — Safe multi-task access
- **Permission control** — Read, write, or read-write per mapping
- **GPU-ready flags** — Prepared for future GPU integration

### API

```c
#include "ipc.h"

/* Create a shared buffer (size rounded up to 2MB) */
struct shared_buffer *buf = shared_buffer_create(1024 * 1024, SHM_RDWR);

/* Map into current task's address space */
void *addr = shared_buffer_map(buf, NULL, SHM_RDWR);  /* NULL = current task */

/* Write data to buffer */
memcpy(addr, source_data, data_size);

/* Get buffer handle to send via message queue */
uint32_t handle = buf->id;

/* --- On receiving task --- */

/* Look up buffer by handle */
struct shared_buffer *buf = shared_buffer_lookup(handle);

/* Map into this task */
void *addr = shared_buffer_map(buf, NULL, SHM_READ);

/* Read data directly (zero-copy) */
process_data(addr, data_size);

/* Unmap when done */
shared_buffer_unmap(buf, NULL);

/* --- On owner task --- */

/* Destroy buffer (only when refcount == 1) */
shared_buffer_destroy(buf);
```

### Flags

| Flag | Description |
|------|-------------|
| `SHM_READ` | Buffer is readable |
| `SHM_WRITE` | Buffer is writable |
| `SHM_RDWR` | Both read and write |
| `SHM_GPU_ACCESSIBLE` | Map with GPU-visible attributes (future) |
| `SHM_MODEL_PAGE` | Mark as model weight storage (future) |
| `SHM_INFERENCE_HOT` | Mark as hot inference data (future) |

### Reference Counting Rules

1. `refcount` starts at 1 (owner's implicit reference)
2. Each `shared_buffer_map()` increments refcount
3. Each `shared_buffer_unmap()` decrements refcount
4. `shared_buffer_destroy()` only succeeds when refcount == 1

This prevents use-after-free while allowing safe multi-task access.

---

## Usage Pattern: Large Data Transfer

The typical pattern for transferring large data between tasks:

```
┌───────────────────────────────────────────────────────────────────┐
│  Producer Task                          Consumer Task             │
├───────────────────────────────────────────────────────────────────┤
│                                                                   │
│  1. Create shared buffer                                          │
│     buf = shared_buffer_create(...)                               │
│                                                                   │
│  2. Map and write data                                            │
│     addr = shared_buffer_map(buf, ...)                            │
│     write_tensor_to(addr);                                        │
│                                                                   │
│  3. Send handle via message                                       │
│     msg.payload.buffer_ref.buffer_handle = buf->id                │
│     msg_send(queue, &msg, ...)                                    │
│                                         ◄──────────────────────── │
│                                         4. Receive message        │
│                                            msg_recv(queue, ...)   │
│                                                                   │
│                                         5. Map buffer             │
│                                            handle = msg.payload...│
│                                            buf = shared_buffer_   │
│                                                    lookup(handle) │
│                                            addr = shared_buffer_  │
│                                                    map(buf, ...)  │
│                                                                   │
│                                         6. Read data (zero-copy)  │
│                                            process_tensor(addr)   │
│                                                                   │
│                                         7. Unmap when done        │
│                                            shared_buffer_unmap()  │
│                                                                   │
│  8. Destroy when all done                                         │
│     shared_buffer_destroy(buf)                                    │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

---

## Implementation Details

### Memory Layout

Message queues allocate:
- 1 page (4KB) for the `struct msg_queue` descriptor
- N pages for the ring buffer (`capacity × msg_size` bytes, rounded up)

Shared buffers allocate:
- 1 page (4KB) for the `struct shared_buffer` descriptor
- M pages for the backing memory (size rounded up to 2MB)

### Blocking Implementation

When a task blocks on send (queue full) or recv (queue empty):

1. Task is added to the queue's wait list (`send_waiters` or `recv_waiters`)
2. Task state is set to `TASK_BLOCKED`
3. Task yields, and scheduler skips it until woken

When space/message becomes available:
1. `wake_one()` removes first task from wait list
2. Task state is set to `TASK_READY`
3. Task is added back to scheduler run queue

### Current Limitations

1. **Kernel address space only** — All tasks share kernel mappings. Future phases may add per-task user space.
2. **No priority inheritance for IPC** — Blocking on IPC can cause priority inversion. (Note: Priority-inheriting mutexes exist separately.)
3. **2MB minimum for shared buffers** — Smaller allocations are rounded up to 2MB block size.

---

## Configuration

| Constant | Default | Description |
|----------|---------|-------------|
| `MSG_SIZE_DEFAULT` | 64 | Default message size in bytes |
| `MSG_QUEUE_MAX` | 32 | Maximum number of queues |
| `SHM_BUFFER_MAX` | 64 | Maximum number of shared buffers |
| `SHM_MAPPING_MAX` | 16 | Maximum mappings per buffer |

---

## Topic-Based Pub/Sub (Message Router)

The message router (`runtime/src/msg_router.rs`) layers a topic-based
publish/subscribe surface on top of per-subscription mailboxes. Components
call `msg_router_subscribe(topic, idx)` to register interest in either an
exact topic name (`/sensors/data`) or a wildcard pattern ending in `*`
(`/sensors/*`); publishers call `msg_router_publish(topic, data)` and the
router fans the message out to every matching subscription.

### Per-subscription delivery contract

Each subscription gets its own independent mailbox. A component subscribed
to a topic via both an exact name and a matching wildcard pattern receives
the message **twice** — once per subscription. This matches the
per-subscriber delivery semantics of DDS, ROS 2, ZeroMQ, nanomsg, and Linux
notifier chains. The publish return value (`delivered`) is the count of
mailboxes the message fanned out to, so overlapping patterns increase the
count. Applications that want exactly-once delivery across overlapping
subscription patterns are responsible for deduplicating at the application
layer (typically by tagging messages with a sender-side sequence number).

Regression coverage:
`kernel/tests/test_msg_router.c::test_wildcard_exact_overlap_delivers_twice`
and `test_wildcard_only_delivers_once` pin the contract on every
`make test` run.

### Other guarantees pinned by automated tests

- **Cross-CPU publish/receive/ack** — a publisher and subscriber on
  different CPUs round-trip N messages without router-internal deadlock
  (`test_integration.c::test_msg_router_cross_cpu`, regression for #66).
- **Multi-subscriber fan-out under load** — two subscribers on two
  separate CPUs both receive every message from a third-CPU publisher;
  every publish returns `delivered == 2`
  (`test_msg_router_multi_subscriber`, regression for #864 / #67a).
- **Priority ordering across mailboxes** — when both a high-priority and
  a low-priority mailbox are ready at the moment `msg_router_receive`
  runs its lock-held scan, the high-priority message is returned first;
  no starvation deadlock under sustained two-publisher load
  (`test_msg_router_priority_concurrent`, regression for #864 / #67a).

### Single-slot mailbox limitation

The current `Mailbox` struct is a single-slot structure (one set of
`ready`/`ack`/`data` atomics per subscription). If two `publish_internal`
calls target the *same* mailbox before the subscriber has acked the first
message, the second `deliver()` overwrites the first message in place;
the subscriber then sees only the second message. This case requires
multiple publishers running concurrently against the same mailbox, which
the standard ack-wait publish path does not produce — its loop blocks on
ack between iterations, so a single publisher cannot back-to-back deliver
to one mailbox.

The single-mailbox overwrite drop is a real limitation of the current
single-slot design and is tracked for post-capstone follow-up as #869.
The concurrency-stress tests in `test_integration.c` document the
boundary explicitly so future maintainers don't mistake a deliberate
limitation for a regression.

---

## Future Enhancements

### Phase 3 (Completed)
- ✅ Timeout support for blocking operations (M6)
- ✅ Message queue statistics and monitoring (M6)
- ✅ Priority-based message queues with starvation prevention

### Phase 4+
- Per-task user address space with separate mappings
- GPU buffer sharing (`SHM_GPU_ACCESSIBLE` fully implemented)
- DMA-friendly buffer allocation

---

*Created: December 2025*
*Last updated: December 2025*
