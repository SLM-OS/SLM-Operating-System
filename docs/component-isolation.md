# Component Isolation (Phase 5 M4)

EL0/EL1 privilege separation for isolating user-mode components from the kernel.

**Status:** Live. Syscall dispatch, fault handling, and EL0 execution are all shipped on ARM64. The VMM_FLAG_USER work that originally blocked EL0 execution closed via the per-task L1 + TTBR0 swap ladder tracked in `docs/archive/plans/pi5-el0-execution-plan.md`. Built-in EL0 component example: `kernel/src/user_hello.c` linked into the kernel via `kernel/src/user_hello_embed.S` and launched through `task_create_user_elf`.

---

## Overview

SLM-OS components can run at EL0 (user mode) on ARM64, where hardware enforces privilege restrictions. A component running at EL0 cannot execute privileged instructions, write to kernel memory, or access hardware directly. All interaction with the kernel occurs through the syscall interface (SVC #0).

If a component at EL0 triggers a fault (page fault, illegal instruction, alignment error), the kernel catches the exception and terminates only the faulting component. The kernel itself continues running. This is a fundamental improvement over running everything at EL1, where any component crash would bring down the entire system.

---

## Syscall ABI

The syscall calling convention follows a custom ABI (not Linux-compatible):

| Register | Purpose             |
|----------|---------------------|
| `x8`     | Syscall number      |
| `x0-x5`  | Arguments           |
| `x0`     | Return value        |

Invocation is via `SVC #0`. Negative return values indicate errors.

### Syscall Table

| Number | Name         | Signature                                          | Description                        |
|--------|--------------|----------------------------------------------------|------------------------------------|
| 0      | `SYS_EXIT`   | `sys_exit(code)`                                   | Terminate the calling component    |
| 1      | `SYS_YIELD`  | `sys_yield()`                                      | Voluntarily yield the CPU          |
| 2      | `SYS_SEND`   | `sys_send(topic, data, len) -> int`                | Publish a message to a topic       |
| 3      | `SYS_RECV`   | `sys_recv(topic_out, buf, len, timeout) -> int`    | Receive a message from a topic     |
| 4      | `SYS_INFER`  | `sys_infer(model, in, in_len, out, out_len) -> int`| Run inference on a loaded model    |
| 5      | `SYS_SLEEP`  | `sys_sleep(ms)`                                    | Sleep for the given duration       |
| 6      | `SYS_LOG`    | `sys_log(str, len)`                                | Log a message to the kernel UART   |

The sentinel `SYS_MAX` (7) marks the end of the table. Invalid syscall numbers return -1 in `x0`.

---

## Exception Handling

Three EL0 exception vectors are installed in `vectors.S`:

### EL0 Synchronous (`el0_sync`)

The `el0_sync_handler()` reads the Exception Class (EC) field from `ESR_EL1`:

- **EC 0x15 (SVC from AArch64):** Dispatched to `syscall_dispatch()`, which reads the syscall number from `frame->x8`, calls the corresponding handler, and writes the result to `frame->x0`. Execution returns to EL0 via `restore_regs` + `ERET`.
- **All other EC values** (data abort, instruction abort, illegal instruction, PC/SP alignment fault, etc.): Routed to `handle_user_fault()`, which logs diagnostics and terminates the component via `task_exit()`.

### EL0 IRQ (`el0_irq`)

Reuses the EL1 IRQ handler (`el1_irq_handler`). This enables timer preemption of user-mode components. When a timer interrupt fires during EL0 execution, the scheduler runs and may context-switch to another task. The `ERET` from the IRQ vector returns to EL0 with the saved `ELR_EL1` and `SPSR_EL1`.

### EL0 SError (`el0_serror`)

Asynchronous external abort from EL0. Handled by `el0_serror_handler()`, which calls `handle_user_fault()` to terminate the component.

### Fault Handling (`handle_user_fault`)

When a user-mode fault occurs, `handle_user_fault()`:

1. Identifies the faulting task from `task_current()`
2. Logs the exception type, ELR (faulting PC), ESR, FAR (for aborts), and SPSR
3. Calls `task_exit()` to terminate the task
4. The scheduler picks the next runnable task; the kernel continues

This is the key isolation property: a component crash does not crash the kernel.

---

## User Task Creation

### API

```c
struct task *task_create_user(const char *name, task_entry_t user_entry,
                              void *arg, uint8_t priority);
```

Creates a new task that transitions to EL0. Available on ARM64 only (x86-64 not yet supported).

### Mechanism

1. `task_create_user()` creates a kernel task with `user_task_wrapper` as the kernel-side entry point. The real EL0 entry point is stored in `task->user_entry`, and `task->is_user` is set to 1.
2. When scheduled for the first time, `user_task_wrapper()` runs at EL1.
3. `user_task_wrapper()` calls `user_task_enter(entry, stack_top, arg)` (in `user_entry.S`).
4. `user_task_enter` sets up the EL0 return state:
   - Writes the entry point to `ELR_EL1`
   - Sets `SPSR_EL1` to `0x0` (EL0t: EL0, SP_EL0, all interrupts enabled)
   - Sets `SP_EL0` to the user stack pointer
   - Clears all general-purpose registers (x1-x30) to prevent kernel data leaks
   - Passes the argument in `x0`
5. `ERET` transitions to EL0 at the specified entry point.

When the EL0 function completes, it issues `SVC #0` with `x8 = SYS_EXIT` to terminate cleanly.

---

## User Syscall Stubs

The header `kernel/include/user_syscall.h` provides inline assembly wrappers for all seven syscalls. User-mode components include this header instead of calling kernel functions directly.

Each stub places arguments in the correct registers, loads the syscall number into `x8`, and executes `SVC #0`. For syscalls that return a value, the result is read from `x0`.

Example (`sys_log`):

```c
static inline void sys_log(const char *str, uint32_t len)
{
    register uint64_t x0 __asm__("x0") = (uint64_t)str;
    register uint64_t x1 __asm__("x1") = (uint64_t)len;
    register uint64_t x8 __asm__("x8") = SYS_LOG;
    __asm__ volatile("svc #0" :: "r"(x0), "r"(x1), "r"(x8) : "memory");
}
```

---

## Pointer Validation

Before accessing user-provided pointers, each syscall handler calls `validate_user_ptr(ptr, len)`. Currently this performs basic sanity checks (non-NULL, no overflow). With a shared address space (no per-component page tables yet), the validation is permissive. Future work will restrict pointers to the component's own memory region once per-component page tables are implemented.

---

## Known Limitations

- **VMM_FLAG_USER on kernel pages causes a QEMU hang.** Setting `VMM_FLAG_USER` on the QEMU virt kernel RAM mapping (so AP[1]=1, UXN=0 on kernel pages that are already W and X) wedges the CPU exactly at the `msr sctlr_el1` instruction that enables the MMU. Re-confirmed on QEMU 8.2.2 + cortex-a76 (May 2026, see #697 step 1). PAN/EPAN/PAN3 are ruled out (cortex-a76 is ARMv8.2-A, no FEAT_PAN3; explicit `msr pan, #0` had no effect). `SCTLR_EL1.WXN` is 0 in baseline. Most likely a QEMU TCG quirk specific to user-flagged executable kernel pages. **The production design avoids this by construction**: per-task `TTBR0_EL1` (user pages live in a separate L1 table), kernel keeps `TTBR1_EL1` with AP=00, and user mappings are never applied to kernel pages.
- **Per-component page tables:** All tasks currently share the kernel's page table (`TTBR1_EL1`). Per-component address spaces via `TTBR0_EL1` are tracked in [#697](https://github.com/SLM-OS/SLM-Operating-System/issues/697) and are the path that unblocks real EL0 execution.
- **x86-64 Ring 3:** Not yet implemented. `task_create_user()` is ARM64-only.
- **Restart policy:** Faulting components are terminated but not automatically restarted. Configurable restart is planned for a future milestone.

---

## Source Files

| File                              | Purpose                                              |
|-----------------------------------|------------------------------------------------------|
| `kernel/include/syscall.h`        | Syscall numbers, dispatch prototype, SPSR_EL0T       |
| `kernel/include/trap.h`           | Trap frame struct (matches vectors.S register layout) |
| `kernel/include/user_syscall.h`   | Inline assembly syscall stubs for EL0 components     |
| `kernel/src/syscall.c`            | Syscall dispatch table and handler implementations   |
| `kernel/arch/arm64/user_entry.S`  | EL1-to-EL0 transition via ERET                       |
| `kernel/arch/arm64/vectors.S`     | EL0 exception vectors (sync, IRQ, SError)            |
| `kernel/arch/arm64/exceptions.c`  | EL0 exception handlers and `handle_user_fault()`     |
| `kernel/sched/task.c`             | `task_create_user()` and `user_task_wrapper()`        |
| `kernel/include/task.h`           | `task_create_user()` prototype                        |

---

*Last updated: May 2026*
