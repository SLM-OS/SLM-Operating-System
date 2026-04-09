# System Call API Reference

API reference for the SLM-OS system call interface. System calls provide the mechanism
for user-mode (EL0) components to request kernel services. The current implementation
defines 7 syscalls covering process lifecycle, scheduling, IPC, inference, and I/O.

**Source files:**
- `kernel/include/syscall.h` -- Syscall numbers and dispatch declaration
- `kernel/include/user_syscall.h` -- User-mode inline assembly stubs
- `kernel/src/syscall.c` -- Syscall handler implementations
- `kernel/include/trap.h` -- Trap frame layout

---

## Syscall ABI (AArch64)

SLM-OS uses a register-based calling convention for system calls on AArch64:

| Register | Purpose |
|----------|---------|
| `x8` | Syscall number (0--6) |
| `x0`--`x5` | Arguments (up to 6) |
| `x0` | Return value (negative values indicate errors) |

**Invocation:** The `SVC #0` instruction triggers a synchronous exception. The processor
transitions from EL0 to EL1, where the exception vector dispatches to `el0_sync_handler`.
When the Exception Class (EC) field of ESR_EL1 equals `0x15` (SVC from AArch64),
`syscall_dispatch()` is called with the saved trap frame.

**Return path:** The handler writes the return value into `frame->x0`. On exception return
(`ERET`), the processor restores registers from the trap frame and resumes execution at
`ELR_EL1` in EL0.

---

## Syscall Table

| Number | Name | Signature | Description |
|--------|------|-----------|-------------|
| 0 | `SYS_EXIT` | `sys_exit(code)` | Terminate the calling component |
| 1 | `SYS_YIELD` | `sys_yield()` | Voluntarily yield the CPU |
| 2 | `SYS_SEND` | `sys_send(topic, data, len)` | Publish a message to a topic |
| 3 | `SYS_RECV` | `sys_recv(topic_out, buf, len, timeout_ms)` | Receive a message |
| 4 | `SYS_INFER` | `sys_infer(model_idx, in, in_len, out, out_len)` | Run model inference |
| 5 | `SYS_SLEEP` | `sys_sleep(ms)` | Sleep for N milliseconds |
| 6 | `SYS_LOG` | `sys_log(str, len)` | Write a string to the kernel UART |

`SYS_MAX` (7) serves as the dispatch table sentinel. Syscall numbers >= `SYS_MAX` are
rejected with a return value of `-1`.

---

## Syscall Descriptions

### SYS_EXIT (0)

Terminate the calling component.

```c
void sys_exit(int code);
```

| Argument | Register | Type | Description |
|----------|----------|------|-------------|
| `code` | `x0` | `int` | Exit code (logged but not propagated) |

**Behavior:** Marks the current task as terminated via `task_exit()` and invokes the
scheduler. This syscall does not return.

**Return value:** None (does not return).

---

### SYS_YIELD (1)

Voluntarily yield the CPU to the scheduler.

```c
void sys_yield(void);
```

No arguments.

**Behavior:** Calls `yield()` to invoke the scheduler, which may select another
ready task to run.

**Return value:** `0` (always succeeds).

---

### SYS_SEND (2)

Publish a message to a named topic via the Rust message router.

```c
int sys_send(const char *topic, const char *data, uint32_t len);
```

| Argument | Register | Type | Description |
|----------|----------|------|-------------|
| `topic` | `x0` | `const char *` | Null-terminated topic name |
| `data` | `x1` | `const char *` | Message payload |
| `len` | `x2` | `uint32_t` | Payload length in bytes |

**Behavior:** Validates the `topic` and `data` pointers, then calls the Rust
`msg_router_publish()` function.

**Return value:** `0` on success, `-1` on error (invalid pointer or publish failure).

---

### SYS_RECV (3)

Receive a message from the Rust message router.

```c
int sys_recv(char *topic_out, char *buf, uint32_t len, uint32_t timeout_ms);
```

| Argument | Register | Type | Description |
|----------|----------|------|-------------|
| `topic_out` | `x0` | `char *` | Buffer for received topic name |
| `buf` | `x1` | `char *` | Buffer for message payload |
| `len` | `x2` | `uint32_t` | Buffer capacity in bytes |
| `timeout_ms` | `x3` | `uint32_t` | Timeout (currently unused) |

**Behavior:** Validates the `buf` pointer, then queries `msg_router_receive()` for
a pending message addressed to the current task. If a message is available, the
payload is copied into `buf` and receipt is acknowledged via `msg_router_ack()`.

**Return value:** `0` if a message was received, `-1` if no message is available or
pointers are invalid. The `timeout_ms` parameter is accepted but not yet implemented
(the call is non-blocking).

---

### SYS_INFER (4)

Run inference on a loaded ONNX model.

```c
int sys_infer(uint32_t model_idx, const void *input, uint32_t in_len,
              void *output, uint32_t out_len);
```

| Argument | Register | Type | Description |
|----------|----------|------|-------------|
| `model_idx` | `x0` | `uint32_t` | Model registry index |
| `input` | `x1` | `const void *` | FP32 input array |
| `in_len` | `x2` | `uint32_t` | Number of input floats |
| `output` | `x3` | `void *` | FP32 output buffer |
| `out_len` | `x4` | `uint32_t` | Output buffer capacity (in floats) |

**Behavior:** Validates both pointers (checking `in_len * 4` and `out_len * 4` bytes),
then calls `rust_infer()` to execute the model's forward pass.

**Return value:** Number of output floats on success, `-1` on pointer validation failure.
The `rust_infer` function itself returns `-2` on inference engine errors.

---

### SYS_SLEEP (5)

Sleep the calling task for a specified duration.

```c
void sys_sleep(uint32_t ms);
```

| Argument | Register | Type | Description |
|----------|----------|------|-------------|
| `ms` | `x0` | `uint32_t` | Sleep duration in milliseconds |

**Behavior:** Calls `slm_sleep_ms()` which blocks the current task for the requested
duration. A value of 0 returns immediately.

**Return value:** `0` (always succeeds).

---

### SYS_LOG (6)

Write a string to the kernel UART console.

```c
void sys_log(const char *str, uint32_t len);
```

| Argument | Register | Type | Description |
|----------|----------|------|-------------|
| `str` | `x0` | `const char *` | String to print |
| `len` | `x1` | `uint32_t` | String length in bytes (max 256) |

**Behavior:** Validates the pointer, then outputs the string character-by-character
via `uart_putc()`. Output is capped at 256 characters regardless of `len`.

**Return value:** `0` on success, `-1` on pointer validation failure.

---

## User-Mode Stubs

The header `kernel/include/user_syscall.h` provides inline assembly wrappers for each
syscall. User-mode components include this header to invoke syscalls without writing
assembly directly. The stubs are only available when compiling for AArch64
(`__aarch64__` defined).

### Usage Example

```c
#include "user_syscall.h"

void user_main(void) {
    sys_log("Hello from EL0\n", 16);
    sys_sleep(1000);

    int result = sys_send("status", "ready", 5);
    if (result < 0) {
        sys_log("Send failed\n", 12);
    }

    sys_exit(0);
}
```

### Stub Signatures

```c
static inline void sys_exit(int code);
static inline void sys_yield(void);
static inline void sys_sleep(uint32_t ms);
static inline void sys_log(const char *str, uint32_t len);
static inline int  sys_send(const char *topic, const char *data, uint32_t len);
static inline int  sys_recv(char *topic_out, char *buf, uint32_t len, uint32_t timeout_ms);
static inline int  sys_infer(uint32_t model_idx, const void *input, uint32_t in_len,
                             void *output, uint32_t out_len);
```

Each stub loads arguments into the appropriate registers (`x0`--`x4`), places the
syscall number in `x8`, and executes `SVC #0`. For syscalls that return a value,
the stub reads the result from `x0` after the `SVC` returns.

---

## Trap Frame Layout

Defined in `kernel/include/trap.h`. The trap frame is pushed onto the kernel stack by
the `save_regs` macro in `vectors.S` when an exception occurs.

```c
struct trap_frame {
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;       /* x0-x7   */
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;  /* x8-x15  */
    uint64_t x16, x17, x18, x19, x20, x21, x22, x23; /* x16-x23 */
    uint64_t x24, x25, x26, x27, x28, x29;           /* x24-x29 */
    uint64_t x30;      /* Link register (LR)              */
    uint64_t elr;      /* Exception Link Register (ELR_EL1) */
    uint64_t spsr;     /* Saved Program Status Register     */
};
```

| Field | Size | Description |
|-------|------|-------------|
| `x0`--`x30` | 248 bytes | General-purpose registers (31 x 8 bytes) |
| `elr` | 8 bytes | Return address (instruction after `SVC #0`) |
| `spsr` | 8 bytes | Processor state at time of exception |

**Total struct size:** 264 bytes. The assembly allocates 272 bytes on the stack
(`TRAP_FRAME_ALLOC`) to maintain 16-byte SP alignment.

Syscall arguments are read from `frame->x0` through `frame->x5`. The syscall number
is in `frame->x8`. The return value is written back to `frame->x0` before `ERET`.

---

## Pointer Validation

The `validate_user_ptr()` function in `syscall.c` performs basic safety checks on
user-provided pointers before the kernel dereferences them.

```c
static int validate_user_ptr(const void *ptr, size_t len);
```

**Current behavior (Phase 5 -- shared address space):**

1. `NULL` pointer with `len == 0` passes (treated as "no access needed")
2. Overflow check: rejects if `ptr + len` wraps around
3. All kernel-mapped addresses are accepted (shared address space)

**Returns:** `1` if the pointer is valid, `0` if invalid.

In the current shared-address-space design, all tasks share the kernel's page tables,
so any mapped address is technically reachable. Future phases with per-component page
tables will restrict validation to each component's own memory region.

---

## Syscall Dispatch

```c
void syscall_dispatch(struct trap_frame *frame);
```

Called from `el0_sync_handler` when the exception class is SVC (EC = `0x15`).

**Dispatch logic:**

1. Read syscall number from `frame->x8`
2. If the number is >= `SYS_MAX` (7), set `frame->x0 = -1` and return
3. Look up the handler in `syscall_table[]`
4. If the handler is `NULL`, set `frame->x0 = -1` and return
5. Call the handler, passing the trap frame
6. Write the handler's return value to `frame->x0`

The dispatch table is a static array of function pointers:

```c
typedef int64_t (*syscall_handler_t)(struct trap_frame *frame);

static syscall_handler_t syscall_table[SYS_MAX] = {
    [SYS_EXIT]  = sys_exit_handler,
    [SYS_YIELD] = sys_yield_handler,
    [SYS_SEND]  = sys_send_handler,
    [SYS_RECV]  = sys_recv_handler,
    [SYS_INFER] = sys_infer_handler,
    [SYS_SLEEP] = sys_sleep_handler,
    [SYS_LOG]   = sys_log_handler,
};
```

---

## Fault Handling

If a syscall handler triggers a synchronous fault (e.g., accessing an unmapped address
from a bad user pointer), the fault is caught by the kernel's synchronous exception
handler at the current EL. The kernel logs the fault and terminates the offending task.

Invalid syscall numbers (>= `SYS_MAX`) do not fault; they return `-1` to the caller
with a diagnostic message printed to UART.

---

## SPSR for EL0

When launching a user-mode component, the kernel sets `SPSR_EL1` to `SPSR_EL0T` (`0x0`),
which configures:

- Exception level: EL0 (unprivileged)
- Stack pointer: SP_EL0
- All interrupts enabled (DAIF clear)

---

*Last updated: April 2026*
