# Echo IPC Debugging Log

**Goal:** Get `component send Hello` to deliver a message to the running echo service via shared mailbox.

**Blocker:** Echo task doesn't see `mailbox.ready = 1` set by the shell's `component send`. Both tasks are on CPU 0 (pinned). The scheduler preempt_disabled fix prevents deadlock, but the mailbox poll doesn't receive.

---

## Session Start

### Current state
- Scheduler preempt_disabled fix in place (commit `a5e9167`)
- `sleep 2000` with echo running: completes but slowly (~10-40s instead of 2s)
- `component send`: shell returns after ack timeout, echo exits with 0 messages
- System doesn't deadlock anymore

### Hypothesis 1: echo_running flag visibility
The `component_send_echo` checks `echo_running` (volatile int). Pre-set to 1 in `component_run` before task starts. Should be visible since it's set by the same thread that later calls `component_send_echo`.

### Hypothesis 2: Mailbox polling too slow
Echo polls every 100ms. Shell ack timeout is 2s (40 × 50ms). If yields are slow (scheduling overhead), the echo might not poll within the 2s window.

### Hypothesis 3: yield() not actually switching to echo
The echo task is on CPU 0. When shell yields, schedule() should pick echo from the queue. But if echo isn't in the queue (was preempted and not re-enqueued properly), it never runs.

---

## Resolution

### Root cause found: Priority starvation

The shell task has priority IDLE (0). The echo service had priority NORMAL (4). The priority-ordered scheduler ALWAYS picks the higher-priority task. When both are in the queue, echo wins every time. Shell only runs when echo exits or is blocked.

This explains why:
- `sleep_ms` was "slow" — the shell's yield gave CPU to echo permanently
- The ack was never seen — the shell never got CPU time to check it
- The system didn't deadlock — echo eventually exited (timeout) and shell resumed

### Fix

1. Set echo service priority to IDLE (same as shell) — enables round-robin
2. Use yield() + pit_ticks timeout (not sleep_ms) for polling
3. Use __atomic_load_n / __atomic_store_n for mailbox fields

### Result

```
slmos> component run echo
Component 'echo' v1.0 started (idx=0, task=11)

slmos> component send First message
[echo] Received: "First message" (msg #1)
Message delivered to echo service

slmos> component send Second message
[echo] Received: "Second message" (msg #2)
Message delivered to echo service

slmos> component send Third time is the charm!
[echo] Received: "Third time is the charm!" (msg #3)
Message delivered to echo service
```

Three consecutive messages delivered and acknowledged on i7-6700 hardware.

---

