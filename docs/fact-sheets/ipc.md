# IPC (Message Router) — Fact Sheet

Message routing, pub/sub, shared buffers, priority-inheritance mutexes.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Message router | Topic-based pub/sub | Same | Same | Same |
| Publisher API | `msg_router_publish(topic, msg, size)` | Same | Same | Same |
| Subscriber API | `msg_subscribe(topic, cb)` / `msg_unsubscribe` | Same | Same | Same |
| Topic name limit | Fixed size in struct | Same | Same | Same |
| Message shape | Pointer + size; no serialization | Same | Same | Same |
| Ack model | Per-message ack with timeout | Same | Same | Same |
| Ack timeout source | `pit_ticks` | `timer_get_count()` CNTPCT (post #80 fix) | Same | LAPIC timer |
| Shared buffers | Ring buffers with producer/consumer pointers | Same | Same | Same |
| Shared buffer cache maintenance | — | DC CVAC/CIVAC required | Same | — |
| Priority-inheritance mutex | ✅ (mutex.h) | Same | Same | Same |
| Message-queue ops (`queue_send`, `queue_recv`) | ✅ | ✅ | ✅ | ✅ |
| Shell surface | `msg send <topic> <payload>`, `msg list`, `msg subscribe <topic>` | Same | Same | Same |
| Lua bindings | `slm.msg_publish`, `slm.msg_subscribe`, `slm.msg_unsubscribe` | Same | Same | Same |
| Multi-state Lua msg fan-out | Shared `LUA_MSG_SUB_IDX` sentinel | Same | Same | Same |

## Skipped / Blocked

- **Cross-state `msg_subscribe` ack loss** — shared `LUA_MSG_SUB_IDX` ack-always hazard documented during PR #217 review. Fix queued as an issue; not currently filed as its own ticket. Non-blocking for demo.
- **`lua_msg_subs` spinlock** — the global Lua message-subscribers table is not currently protected by a spinlock. OK today because all shell-driven Lua access runs on CPU 0 and Lua states are per-session, but incorrect under truly concurrent access. Fix queued as an issue.
- **Pi 5 IPC ack timeouts used to break** (#80 closed) — `pit_ticks` never advanced on Pi 5, so `msg_router_publish` ack waits hung. Fixed by switching ack timeout to `timer_get_count()` via CNTPCT_EL0 (PR #102).
- **Message persistence** — no durable message store; all messages live in RAM and die on reboot.
- **Cross-OS / cross-machine IPC** — nothing. The message router is single-machine.
- **Message serialization format** — raw pointer + size. No versioning, no schema. Subscribers must know the producer's layout.

## See also

- `docs/ipc.md` (narrative)
- `docs/m7-message-router.md` (design doc)
- `docs/archive/investigations/echo-ipc-debugging.md` (historical debugging notes)
- `docs/component-development.md` §"Message router hookup"

*Last updated: 18 April 2026*
