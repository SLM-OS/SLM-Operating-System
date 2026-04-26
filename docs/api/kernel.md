# SLM-OS C Kernel API Reference

This document provides a concise reference for the SLM-OS C kernel API. All functions are declared in the corresponding header files under `kernel/include/`.

---

## Task Management (`task.h`)

### Types and Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `CPU_AFFINITY_ANY` | `(uint32_t)-1` | Task may run on any CPU |
| `TASK_PRIORITY_IDLE` | 0 | Background/idle tasks |
| `TASK_PRIORITY_LOW` | 2 | Low priority |
| `TASK_PRIORITY_NORMAL` | 4 | Default priority |
| `TASK_PRIORITY_HIGH` | 6 | High priority |
| `TASK_PRIORITY_CRITICAL` | 7 | Highest priority |

Task states: `TASK_READY`, `TASK_RUNNING`, `TASK_BLOCKED`, `TASK_TERMINATED`.

### Functions

```c
struct task *task_create(const char *name, task_entry_t entry, void *arg);
```
Create a task with default priority (`TASK_PRIORITY_NORMAL`). Returns task pointer or `NULL`.

```c
struct task *task_create_with_priority(const char *name, task_entry_t entry, void *arg, uint8_t priority);
```
Create a task with specified priority (0--7). Returns task pointer or `NULL`.

```c
struct task *task_create_user(const char *name, task_entry_t user_entry, void *arg, uint8_t priority);
```
Create a user-mode (EL0) task. ARM64 only. The task transitions to EL0 via ERET on first schedule.

```c
void task_exit(void);
```
Terminate the current task. Does not return.

```c
struct task *task_current(void);
```
Return a pointer to the currently running task.

```c
struct task *task_get(uint32_t id);
```
Look up a task by ID. Returns `NULL` if not found.

```c
void task_set_affinity(struct task *task, uint32_t cpu);
```
Pin a task to a specific CPU, or pass `CPU_AFFINITY_ANY` for any CPU.

```c
uint32_t task_get_affinity(struct task *task);
```
Return the task's CPU affinity setting.

```c
void task_set_priority(struct task *task, uint8_t priority);
```
Set task priority (0--7, clamped to valid range).

```c
uint8_t task_get_priority(struct task *task);
```
Return the task's base priority.

```c
uint8_t task_get_effective_priority(struct task *task);
```
Return the task's effective priority (may include deadline boost).

```c
void task_set_deadline(struct task *task, uint64_t deadline_ns);
```
Set an absolute deadline in nanoseconds (0 = no deadline).

```c
uint64_t task_get_deadline(struct task *task);
```
Return the task's deadline in nanoseconds.

```c
void task_destroy(struct task *task);
```
Free a terminated task's resources. The task must be in `TASK_TERMINATED` state.

```c
void task_set_cleanup(struct task *task, task_cleanup_t cleanup, void *cleanup_arg);
```
Register a cleanup callback invoked when the task is destroyed.

---

## Scheduler (`sched.h`)

### Functions

```c
void scheduler_init(void);
```
Initialize the scheduler on the boot CPU. Must be called before other scheduler functions.

```c
void scheduler_init_secondary(uint32_t cpu);
```
Initialize the scheduler for a secondary CPU. Creates the idle task for that CPU.

```c
int scheduler_is_initialized(void);
```
Return nonzero if `scheduler_init()` has completed.

```c
void scheduler_add_task(struct task *task);
```
Add a ready task to the run queue. The active scheduling policy selects the target CPU.

```c
void scheduler_add_task_to_cpu(struct task *task, uint32_t cpu);
```
Add a ready task directly to a specific CPU's run queue.

```c
void scheduler_remove_task(struct task *task);
```
Remove a task from the run queue.

```c
void schedule(void);
```
Select the next task and perform a context switch. Called by `yield()` and the timer interrupt.

```c
void yield(void);
```
Voluntarily yield the CPU. The current task moves to the back of its run queue.

```c
void scheduler_start(uint32_t cpu);
```
Start scheduling on the given CPU. Enables the timer, unmasks interrupts, and switches to the first task. Does not return.

```c
void scheduler_get_stats(struct sched_stats *stats);
```
Fill a `sched_stats` struct with task count, ready count, context switches, and timer ticks.

```c
int sched_migrate_task(struct task *task, uint32_t target_cpu);
```
Migrate a non-running task to a different CPU's run queue. Returns 0 on success.

```c
void scheduler_tick(void);
```
Timer tick handler. Decrements time slices and triggers preemption.

```c
void scheduler_dump(void);
```
Print scheduler state to UART for debugging.

### Core Isolation

```c
int sched_isolate_core(uint32_t cpu);
```
Isolate a core from general scheduling. CPU 0 cannot be isolated. Returns 0 on success.

```c
int sched_unisolate_core(uint32_t cpu);
```
Remove isolation from a core, returning it to general scheduling.

```c
int sched_is_core_isolated(uint32_t cpu);
```
Return 1 if the core is isolated, 0 otherwise.

```c
uint32_t sched_get_isolated_cores(void);
```
Return a bitmask of isolated cores (bit N set = CPU N isolated).

```c
int sched_set_task_affinity(struct task *task, uint32_t cpu);
```
Set task CPU affinity. Returns 0 on success, -1 for invalid CPU.

### Context Switch (Assembly)

```c
extern void switch_to(struct task *old, struct task *new);
```
Save context of `old`, restore context of `new`. Implemented in `context.S`.

---

## Scheduler Policies (`sched_policy.h`)

Policies control CPU assignment for new tasks via a vtable. The scheduler core (run queues, locking, context switch) is unchanged across policies.

### Policy Operations Struct

```c
struct sched_policy_ops {
    const char *name;
    int  (*init)(void);
    void (*shutdown)(void);
    uint32_t (*assign_cpu)(struct task *task);
    void (*tick)(uint32_t cpu);
};
```

- `assign_cpu` -- Select a CPU for a new task with `CPU_AFFINITY_ANY`. Required.
- `tick` -- Per-tick callback for rebalancing. Optional (may be `NULL`).
- `init`/`shutdown` -- Lifecycle hooks called on policy activation/deactivation.

### Functions

```c
int sched_register_policy(const struct sched_policy_ops *policy);
```
Register a policy for selection by name. Returns 0 on success, -1 if the registry is full.

```c
int sched_set_policy(const struct sched_policy_ops *policy);
```
Switch to a different scheduling policy. Calls `shutdown()` on the old policy and `init()` on the new one. Returns 0 on success.

```c
const char *sched_get_policy(void);
```
Return the name of the currently active policy.

```c
const struct sched_policy_ops *sched_find_policy(const char *name);
```
Look up a registered policy by name. Returns `NULL` if not found.

```c
int sched_policy_count(void);
```
Return the number of registered policies.

```c
const struct sched_policy_ops *sched_policy_get(int index);
```
Return a registered policy by index for enumeration.

Built-in policy: `sched_policy_heuristic` (round-robin load balancing with deadline pressure). AI policies (MLP, PPO) are available when `CONFIG_AI_SCHEDULER` is enabled.

---

## IPC -- Message Queues (`ipc.h`)

### Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `IPC_OK` | 0 | Success |
| `IPC_ERR_NOMEM` | -1 | Out of memory |
| `IPC_ERR_FULL` | -2 | Queue full (non-blocking) |
| `IPC_ERR_EMPTY` | -3 | Queue empty (non-blocking) |
| `IPC_ERR_TIMEOUT` | -4 | Operation timed out |
| `IPC_ERR_INVALID` | -5 | Invalid parameter |
| `IPC_ERR_BUSY` | -6 | Resource busy |

### Priority Levels

`MSG_PRIO_LOW` (0), `MSG_PRIO_NORMAL` (1), `MSG_PRIO_HIGH` (2), `MSG_PRIO_URGENT` (3). Starvation prevention triggers after 8 consecutive high-priority receives.

### Functions

```c
void ipc_init(void);
```
Initialize the IPC subsystem. Must be called before creating queues or buffers.

```c
struct msg_queue *msg_queue_create(size_t capacity, size_t msg_size);
```
Create a message queue. `msg_size` of 0 uses the default (64 bytes). Returns `NULL` on failure.

```c
int msg_queue_destroy(struct msg_queue *queue);
```
Destroy a queue. Returns `IPC_ERR_BUSY` if tasks are waiting.

```c
int msg_send(struct msg_queue *queue, const void *msg, int timeout_ms);
```
Send a message at normal priority. Timeout: 0 = non-blocking, -1 = wait forever.

```c
int msg_send_priority(struct msg_queue *queue, const void *msg, int priority, int timeout_ms);
```
Send a message at specified priority (`MSG_PRIO_LOW` through `MSG_PRIO_URGENT`).

```c
int msg_recv(struct msg_queue *queue, void *msg, int timeout_ms);
```
Receive the highest-priority available message. Message data is copied to the caller's buffer.

```c
size_t msg_queue_count(struct msg_queue *queue);
```
Return the number of pending messages in the queue.

```c
struct msg_queue *msg_queue_lookup(uint32_t id);
```
Look up a queue by its ID. Returns `NULL` if not found.

```c
void msg_queue_stats(struct msg_queue *queue, uint64_t *msgs_sent, uint64_t *msgs_recv, size_t *high_water);
```
Retrieve per-queue statistics. Any output pointer may be `NULL` to skip that field.

```c
void ipc_get_stats(struct ipc_stats *stats);
```
Retrieve global IPC statistics (queue count, buffer count, total messages).

---

## IPC -- Shared Buffers (`ipc.h`)

### Flags

| Flag | Description |
|------|-------------|
| `SHM_READ` | Buffer is readable |
| `SHM_WRITE` | Buffer is writable |
| `SHM_RDWR` | Read and write |
| `SHM_GPU_ACCESSIBLE` | Map with GPU-visible attributes |
| `SHM_MODEL_PAGE` | Mark as model weight storage |
| `SHM_INFERENCE_HOT` | Mark as hot inference data |

### Functions

```c
struct shared_buffer *shared_buffer_create(size_t size, uint32_t flags);
```
Create a shared buffer. Size is rounded up to 2 MB. Starts with refcount 1 (owner).

```c
void *shared_buffer_map(struct shared_buffer *buffer, struct task *task, uint32_t perms);
```
Map a buffer into a task's address space. Pass `NULL` for the current task. Returns virtual address or `NULL`.

```c
int shared_buffer_unmap(struct shared_buffer *buffer, struct task *task);
```
Unmap a buffer from a task. Decrements refcount.

```c
int shared_buffer_destroy(struct shared_buffer *buffer);
```
Destroy a buffer. Only the owner may destroy it, and only when refcount is 1.

```c
struct shared_buffer *shared_buffer_lookup(uint32_t id);
```
Look up a shared buffer by ID.

```c
void *shared_buffer_phys_addr(struct shared_buffer *buffer);
```
Return the physical address of a buffer's backing memory. Useful for GPU operations.

---

## Timer (`timer.h`)

```c
void timer_init(void);
```
Configure the timer for periodic interrupts at `TIMER_HZ`. Does not start the timer.

```c
void timer_start(void);
```
Begin generating periodic timer interrupts.

```c
void timer_stop(void);
```
Disable timer interrupts.

```c
void timer_handler(void);
```
Timer IRQ handler. Reloads the timer and calls `scheduler_tick()`.

```c
uint64_t timer_get_count(void);
```
Return the current hardware timer counter value. Always advances regardless of IRQ mask state.

```c
uint64_t timer_get_frequency(void);
```
Return the timer frequency in Hz.

```c
void timer_percpu_init(void);
```
Per-CPU timer initialization. Called by each secondary CPU after boot.

```c
void sleep_ms(uint32_t ms);
```
Block the calling task for the given number of milliseconds. Must not be called from interrupt context.

```c
void sleep_us(uint64_t us);
```
Block the calling task for the given number of microseconds. Precision is limited by the timer tick rate.

```c
void timer_wake_sleepers(void);
```
Wake tasks whose sleep deadline has passed. Called from `scheduler_tick()`.

---

## SMP (`smp.h`)

### Data Structures

```c
struct per_cpu {
    uint32_t cpu_id;
    uint64_t mpidr;
    volatile bool online;
    void *stack_top;
    uint64_t boot_time_ns;
};
```

Global state: `cpu_data[MAX_CPUS]`, `cpu_count`, `cpus_online`.

### Functions

```c
void smp_init(void);
```
Initialize SMP. Discovers CPUs and brings up secondary cores via PSCI.

```c
uint32_t cpu_id(void);
```
Return the current CPU's logical ID. Inline function (reads MPIDR on ARM64, LAPIC ID on x86-64).

```c
struct per_cpu *this_cpu(void);
```
Return the per-CPU data structure for the current CPU.

```c
int cpu_logical_id(uint64_t mpidr);
```
Map a physical CPU identifier (MPIDR or LAPIC ID) to a logical ID. Returns -1 if not found.

```c
int psci_cpu_on(uint64_t target_mpidr, uintptr_t entry_point, uintptr_t context_id);
```
Power on a secondary CPU via PSCI. Returns 0 on success.

```c
void psci_cpu_off(void);
```
Power off the calling CPU. Does not return on success.

```c
void psci_system_reset(void);
```
Reset the entire system. Does not return.

```c
void psci_system_off(void);
```
Power off the entire system. Does not return.

```c
void secondary_init(uint32_t cpu_id);
```
Secondary CPU entry point (called from `smp_boot.S`). Performs per-CPU init and enters idle loop.

---

## Physical Memory Manager (`pmm.h`)

Buddy allocator with O(log n) allocation. Block sizes are powers of 2, from 4 KB (order 0) to 1 GB (order 18).

```c
void pmm_init(void);
```
Initialize the PMM. Called early in boot after UART is available.

```c
void *pmm_alloc_page(void);
```
Allocate a single 4 KB page. Returns physical address or 0.

```c
void *pmm_alloc_pages(size_t count);
```
Allocate contiguous pages. Count is rounded up to the next power of 2. Returns physical address or 0.

```c
void pmm_free_page(void *page);
```
Free a single page.

```c
void pmm_free_pages(void *page, size_t count);
```
Free contiguous pages. Automatically coalesces buddies.

```c
void pmm_get_stats(struct pmm_stats *stats);
```
Fill a `pmm_stats` struct (total, free, used, reserved pages; heap bounds).

```c
void pmm_get_buddy_stats(struct pmm_buddy_stats *stats);
```
Retrieve buddy allocator internals (free counts per order, split/merge counts).

```c
size_t pmm_get_free_pages(void);
```
Return the number of free pages.

```c
size_t pmm_get_total_pages(void);
```
Return the total number of managed pages.

```c
void pmm_dump_stats(void);
```
Print PMM statistics to UART.

---

## Virtual Memory Manager (`vmm.h`)

ARM64 page tables using 4 KB granule with 2-level tables (L1 -> L2 blocks) for 2 MB mappings. 39-bit virtual address space (512 GB).

```c
void vmm_init(void);
```
Create kernel page tables and enable the MMU. After this call, all code runs at virtual addresses.

```c
int vmm_map_block(uint64_t virt, uint64_t phys, uint32_t flags);
```
Map a single 2 MB block. Both addresses must be 2 MB aligned. Returns 0 on success.

```c
int vmm_unmap_block(uint64_t virt);
```
Unmap a 2 MB block. Returns 0 on success, -1 if not mapped.

```c
int vmm_map_region(uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags);
```
Map a contiguous region (size rounded up to 2 MB). Returns 0 on success.

```c
uint64_t vmm_virt_to_phys(uint64_t virt);
```
Translate a virtual address to physical. Returns 0 if not mapped.

```c
bool vmm_is_mapped(uint64_t virt);
```
Check whether a virtual address is mapped.

```c
void vmm_invalidate_tlb(uint64_t virt);
```
Invalidate the TLB entry for a single address on all CPUs (broadcasts via inner-shareable).

```c
void vmm_invalidate_tlb_range(uint64_t start, uint64_t end);
```
Invalidate TLB entries for a range. Falls back to full flush for large ranges (> 32 pages).

```c
void vmm_invalidate_tlb_all(void);
```
Invalidate the entire TLB on all CPUs.

```c
void vmm_get_stats(struct vmm_stats *stats);
```
Retrieve VMM statistics (table counts, blocks mapped, bytes mapped).

```c
void vmm_dump(void);
```
Print page table mappings to UART.

### Memory Flags (`VMM_FLAG_*`)

| Flag | Description |
|------|-------------|
| `VMM_FLAG_DEVICE` | Device memory (nGnRnE) |
| `VMM_FLAG_NOCACHE` | Non-cacheable normal memory |
| `VMM_FLAG_READ` | Readable |
| `VMM_FLAG_WRITE` | Writable |
| `VMM_FLAG_EXEC` | Executable |
| `VMM_FLAG_USER` | EL0 accessible |
| `VMM_FLAG_GPU_MAPPED` | Mapped to GPU |
| `VMM_FLAG_MODEL_PAGE` | Contains model data |
| `VMM_FLAG_INFERENCE_HOT` | Hot inference page |

Common combinations: `VMM_FLAGS_KERNEL_CODE`, `VMM_FLAGS_KERNEL_DATA`, `VMM_FLAGS_DEVICE`, `VMM_FLAGS_USER_CODE`, `VMM_FLAGS_USER_DATA`.

---

## UART (`uart.h`)

Platform-independent UART interface. Implementation selected at compile time: PL011 (QEMU, Pi 5) or Tegra186-UART (Jetson Orin Nano). Output functions are spinlock-synchronized for multi-CPU safety.

```c
void uart_init(void);
```
Initialize UART hardware. Must be called before other UART functions.

```c
void uart_putc(char c);
```
Send a single character. Not synchronized.

```c
char uart_getc(void);
```
Receive a single character (blocking).

```c
void uart_puts(const char *s);
```
Send a null-terminated string. Synchronized. Converts `\n` to `\r\n`.

```c
int uart_printf(const char *fmt, ...);
```
Formatted output (synchronized). Supports `%c`, `%s`, `%d`/`%i`, `%u`, `%x`/`%X`, `%p`, `%%`. Length modifiers: `%l` and `%ll` (both treated as 64-bit on the kernel targets — `long` and `long long` are 64-bit on aarch64-elf and x86_64-elf), and `%z` for `size_t`. Width and zero-pad flags are supported (e.g. `%016llx`, `%-12s`).

```c
int uart_snprintf(char *buf, size_t size, const char *fmt, ...);
```
Formatted output to a buffer. Writes at most `size - 1` characters, always null-terminates.

Unlocked variants (`uart_puts_unlocked`, `uart_printf_unlocked`, `uart_vprintf`) are provided for panic handlers and early boot where locks are unsafe.

---

## FFI Bridge (`slm_ffi.h`)

Stable C ABI functions callable from the Rust runtime.

### Memory

```c
void *slm_alloc_pages(size_t count);
```
Allocate contiguous 4 KB pages. Returns physical address or 0.

```c
void slm_free_pages(void *addr, size_t count);
```
Free previously allocated pages.

```c
int slm_map_region(uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags);
```
Map a region into kernel virtual address space (2 MB aligned).

```c
int slm_unmap_region(uint64_t virt, uint64_t size);
```
Unmap a region from kernel virtual address space.

### Timing

```c
uint64_t slm_get_time_ns(void);
```
Return nanoseconds elapsed since boot. Uses a split-multiply
(`secs * 1e9 + (frac_ticks * 1e9) / freq`) internally so the
conversion does not overflow past a few seconds of uptime on
high-frequency timers like the x86-64 TSC (#171).

```c
uint64_t slm_time_ticks_to_ns(uint64_t ticks, uint64_t freq);
```
Pure helper: convert a raw tick count at a given timer frequency
into nanoseconds using the same overflow-safe formula as
`slm_get_time_ns`. Primarily useful for unit tests that need to
exercise the conversion against synthetic tick values.

```c
void slm_sleep_ms(uint32_t ms);
```
Sleep the current task for the given milliseconds.

### Task Management

```c
uint32_t slm_task_create(const char *name, slm_task_entry_t entry, void *arg);
```
Create a kernel task. Returns task ID (nonzero) on success, 0 on failure.

```c
int slm_task_set_priority(uint32_t task_id, uint8_t priority);
```
Set task priority by task ID.

```c
int slm_task_set_deadline(uint32_t task_id, uint64_t deadline_ns);
```
Set task deadline by task ID.

```c
uint32_t slm_task_current(void);
```
Return the current task's ID.

### IPC

```c
int slm_msg_send(uint32_t queue_id, const void *msg, size_t msg_size, int timeout_ms);
```
Send a message to a queue by ID.

```c
int slm_msg_recv(uint32_t queue_id, void *msg, size_t msg_size, int timeout_ms);
```
Receive a message from a queue by ID.

### Debug

```c
void slm_print(const char *s);
```
Print a null-terminated string to UART.

### GPU Cache Coherency

```c
void slm_gpu_sync_for_device(void *addr, size_t size);
```
Flush CPU caches so the GPU sees the latest data (DC CVAC).

```c
void slm_gpu_sync_for_cpu(void *addr, size_t size);
```
Invalidate CPU caches so the CPU sees GPU-written data (DC IVAC).

```c
int slm_gpu_available(void);
```
Return 1 if the GPU subsystem is available, 0 otherwise.

```c
int slm_gpu_get_info(RustGpuInfo *info);
```
Fill a `RustGpuInfo` struct with GPU device information.

---

## Component System (`component.h`)

### Registration and Query

```c
int component_register(const char *name, const char *version, uint8_t type, uint8_t priority);
```
Register a new component. Returns component index on success, -1 on error.

```c
int component_run(const char *name);
```
Run a built-in component by name. Creates a task and links it to the component.

```c
int component_find(const char *name);
```
Find a component by name. Returns index or -1.

### Hot-Swap

```c
int component_hot_swap(const char *old_name, const char *new_name);
```
Replace a running component. Saves subscriptions, unregisters old, starts new, restores subscriptions.

```c
int component_hot_swap_stateful(const char *old_name, const char *new_name,
                                component_state_export_fn export_fn);
```
Stateful hot-swap: calls `export_fn` to serialize old component's state (up to 256 bytes), then performs standard hot-swap. New component retrieves state via `component_get_swap_state()` during init.

```c
uint32_t component_get_swap_state(uint8_t *buf, uint32_t max_size);
```
Read the state buffer from the most recent stateful hot-swap. One-shot (clears after read). Returns bytes copied, or 0 if no state.

### Direct Messaging

```c
int component_direct_channel_create(int sender_idx, int receiver_idx);
```
Create a point-to-point message channel bypassing topic routing. Returns channel ID (>= 0).

```c
int component_direct_send(int channel, const char *data, uint32_t len);
```
Send a message on a direct channel. Waits for ack (2s timeout). Returns 0 on success, -2 on timeout.

```c
const char *component_direct_receive(int channel);
```
Poll for a pending message. Returns pointer to data or NULL.

```c
void component_direct_ack(int channel);
```
Acknowledge receipt of a direct message.

### Large Message Publish

```c
int msg_router_publish_large(const char *topic_name, const char *data, uint32_t data_len);
```
Publish a large message. Currently delegates to `msg_router_publish` (copies data). Future versions will pass by reference for true zero-copy in the shared address space.
