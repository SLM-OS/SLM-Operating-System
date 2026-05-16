# IPC (Message Router) — Fact Sheet

Message routing, pub/sub, shared buffers, priority-inheritance mutexes.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Message router | Topic-based pub/sub | Same | Same | Same |
| Publisher API (default, ack-wait) | `msg_router_publish(topic, data)` | Same | Same | Same |
| Publisher API (priority, ack-wait) | `msg_router_publish_priority(topic, data, prio)` | Same | Same | Same |
| Publisher API (fire-and-forget) | `msg_router_publish_nowait(topic, data)` | Same | Same | Same |
| Subscriber API | `msg_router_subscribe(topic, component_idx)` / `msg_router_unsubscribe_all(idx)` | Same | Same | Same |
| Receive / ack API | `msg_router_receive(idx, topic_out)` + `msg_router_ack(idx)` | Same | Same | Same |
| Topic name limit | `TOPIC_NAME_LEN = 16` incl. NUL; oversized rejected (#69) | Same | Same | Same |
| Wildcard subscriptions | Pattern ending in `*` matches topic prefix (`/sensors/*`) | Same | Same | Same |
| Per-subscription delivery | Exact + wildcard match deliver **once per subscription** (DDS / ROS 2 / ZeroMQ contract) | Same | Same | Same |
| Cross-mailbox priority ordering | `receive()` returns highest-priority ready mailbox under lock-held scan | Same | Same | Same |
| LAST_RECEIVED ack-targeting | `ack` clears exactly the mailbox the most recent `receive` returned, even under cross-CPU activity | Same | Same | Same |
| Single-slot mailbox limitation | Back-to-back nowait deliver to same mailbox overwrites first message (#869 — post-capstone) | Same | Same | Same |
| Message shape | Pointer + NUL-terminated bytes (`MAX_MSG_LEN = 60`) | Same | Same | Same |
| Ack model | Per-message ack with 5 s CNTPCT timeout (no-ack-wait variant available) | Same | Same | Same |
| Ack timeout source | `timer_get_count()` CNTPCT | Same | Same | LAPIC timer |
| Shared buffers | Ring buffers with producer/consumer pointers | Same | Same | Same |
| Shared buffer cache maintenance | — | DC CVAC/CIVAC required | Same | — |
| Priority-inheritance mutex | ✅ (mutex.h) | Same | Same | Same |
| Message-queue ops (`queue_send`, `queue_recv`) | ✅ | ✅ | ✅ | ✅ |
| Shell surface | `msg send <topic> <payload>`, `msg list`, `msg subscribe <topic>` | Same | Same | Same |
| Lua bindings | `slm.msg_publish`, `slm.msg_subscribe`, `slm.msg_unsubscribe` | Same | Same | Same |
| Multi-state Lua msg fan-out | Shared `LUA_MSG_SUB_IDX` sentinel | Same | Same | Same |

### Concurrency stress tests (#67)

The router's concurrency contracts above are pinned by automated tests
that run on every `make test`:

| Test | Suite | What it pins |
|---|---|---|
| `test_msg_router_cross_cpu` | `test_integration.c` | Cross-CPU pub/recv/ack round-trip without router-internal deadlock (#66) |
| `test_msg_router_multi_subscriber` | `test_integration.c` | 2 subs on 2 CPUs both receive every message from a 3rd-CPU publisher; `delivered == 2` per publish (#864 / #67a) |
| `test_msg_router_priority_concurrent` | `test_integration.c` | High-prio + low-prio publishers don't starve each other under sustained load; cross-mailbox prio scan picks high-prio first (#864 / #67a) |
| `test_wildcard_exact_overlap_delivers_twice` | `test_msg_router.c` | Exact + wildcard overlap delivers twice (one per subscription) (#863 / #67b) |
| `test_wildcard_only_delivers_once` | `test_msg_router.c` | Wildcard-only match delivers once (#863 / #67b) |
| `test_msg_router_ack_targets_last_received` | `test_msg_router.c` | `ack` clears the right mailbox when a newer message landed in a different mailbox between receive and ack (#867 / #67c) |
| `test_msg_router_ack_targets_last_received_cross_cpu` | `test_integration.c` | Same as above but the second publish happens on a different CPU than receive/ack (#867 / #67c) |
| `test_publish_nowait_basic_contract` | `test_msg_router.c` | `publish_nowait` returns immediately with fan-out count, rejects NULL / oversized topics, overwrites unacked mailbox in place |

## Skipped / Blocked

- **Cross-state `msg_subscribe` ack loss** — shared `LUA_MSG_SUB_IDX` ack-always hazard. Non-blocking for demo; not currently filed as its own ticket.
- **`lua_msg_subs` spinlock** — the global Lua message-subscribers table is not protected by a spinlock. Safe today because all shell-driven Lua access runs on CPU 0 and Lua states are per-session; incorrect under truly concurrent access.
- **Message persistence** — no durable message store; all messages live in RAM and die on reboot.
- **Cross-OS / cross-machine IPC** — nothing. The message router is single-machine.
- **Message serialization format** — raw pointer + size. No versioning, no schema. Subscribers must know the producer's layout.

## See also

- `docs/ipc.md` (narrative)
- `docs/m7-message-router.md` (design doc)
- `docs/component-development.md` §"Message router hookup"

*Last updated: 15 May 2026 (#67 concurrency-test audit)*
