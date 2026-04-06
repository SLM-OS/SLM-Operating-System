# Kernel-Specific Notes

Notes for working on the C kernel code.

---

## Critical Struct Layout Rules

### struct task Field Ordering

**CRITICAL:** The `context` field in `struct task` MUST remain at offset 0x20. The assembly code in `context.S` has a hardcoded `TASK_CONTEXT_OFFSET = 0x20`.

If you need to add new fields to `struct task`:
- Add them AFTER the `context` field, not before
- Or update `TASK_CONTEXT_OFFSET` in `context.S` to match

If the offset is wrong, context switches will corrupt memory and cause crashes (typically instruction abort at address 0x0).

---

## Assembly/C Interface

### Struct Offsets in Assembly

**Issue:** Hardcoded struct offsets in assembly files (like `context.S`) must match the actual C struct layout. When C structs change — especially due to alignment attributes like `alignas(16)` — the assembly offsets become stale and cause subtle bugs (typically alignment faults jumping to exception vectors).

**Example:** Adding `alignas(16)` to a struct member inserts padding, shifting all subsequent field offsets.

**Prevention:**
- When modifying structs accessed by assembly, update the corresponding `#define` offsets in `.S` files
- Consider using `offsetof()` in C to generate offsets and pass to assembly
- Add comments linking assembly offsets to their C struct definitions

**Affected files:**
- `kernel/include/task.h` — defines `struct cpu_context` and `struct task`
- `kernel/src/context.S` — has `CTX_*` and `TASK_CONTEXT_OFFSET` defines

---

## C23 Freestanding Headers

In bare-metal code, only these standard headers are safe (no libc required):

| Header | Provides |
|--------|----------|
| `<float.h>` | Floating-point limits |
| `<iso646.h>` | Alternative operator spellings |
| `<limits.h>` | Integer limits |
| `<stdarg.h>` | `va_list`, `va_start`, `va_arg`, `va_end` |
| `<stddef.h>` | `NULL`, `nullptr`, `size_t`, `ptrdiff_t`, `offsetof` |
| `<stdint.h>` | `uint32_t`, `int64_t`, `uintptr_t`, etc. |

**C23 keywords (no header needed):**
- `bool`, `true`, `false` — boolean type and constants
- `alignas`, `alignof` — alignment specifiers
- `nullptr` — type-safe null pointer
- `static_assert` — compile-time assertions

**NOT safe:** `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>` — these require libc.

---

## C23 Strict Compliance

The kernel is compiled with `-std=c23 -Wpedantic` (no GNU extensions). This means:

**Use `__asm__` instead of `asm`:**
```c
/* Correct */
__asm__ volatile("wfi");

/* Incorrect - will not compile */
asm volatile("wfi");
```

**Use `__asm__` for register constraints:**
```c
/* Correct */
register uint64_t x0 __asm__("x0") = value;

/* Incorrect */
register uint64_t x0 asm("x0") = value;
```

**C23: `alignas` is now a keyword (no header needed):**
```c
/* C23 - alignas works directly */
alignas(16) uint8_t buffer[64];

/* _Alignas also still works */
_Alignas(16) uint8_t buffer[64];
```

**C23: Use `__VA_OPT__` for variadic macros:**
```c
/* Correct - C23 standard */
#define INFO(fmt, ...) uart_printf("[INFO] " fmt "\n" __VA_OPT__(,) __VA_ARGS__)

/* Incorrect - GNU extension, triggers -Wpedantic warning */
#define INFO(fmt, ...) uart_printf("[INFO] " fmt "\n", ##__VA_ARGS__)
```

---

## Cache Maintenance (Pi 5 / No SMPEN)

On Pi 5, TF-A does not set SMPEN for secondary cores. Use `cache.h` helpers for cross-CPU data:

**Critical rule: DC CIVAC writes back dirty data before invalidating.**

If CPU 0 has a dirty cacheline and you call `cache_invalidate()` (DC CIVAC), it first writes CPU 0's stale data to PoC, overwriting any newer value written by another CPU. Always `cache_clean()` or `cache_clean_range()` shared data on the writer side before another CPU uses `cache_invalidate()` to read it.

**Pattern for cross-CPU data init:**
```c
/* CPU 0: initialize shared data, then clean before secondary boot */
for (uint32_t i = 0; i < cpu_count; i++)
    init_data(i);
cache_clean_range(shared_data, sizeof(shared_data));

/* Secondary CPU: write, then clean */
shared_data[cpu].field = value;
cache_clean(&shared_data[cpu].field);

/* CPU 0 (reader): invalidate, then read */
cache_invalidate(&shared_data[cpu].field);
val = shared_data[cpu].field;
```

**False sharing:** `struct per_cpu` is 40 bytes. Adjacent entries share 64-byte cachelines. A dirty write to `cpu_data[0]` can be written back by CIVAC when invalidating `cpu_data[1]`.

---

## Non-Cacheable Shared Memory (Pi 5 + Jetson)

On real ARM64 hardware (Pi 5, Jetson), per-core L2 caches are incoherent despite SMPEN. DC CIVAC doesn't propagate through per-core L2. Non-cacheable (NC) memory bypasses L1/L2 entirely, making writes instantly visible to all CPUs. Enabled when `PLATFORM_HAS_NC_MEMORY` is defined (set in `ncmem.h`).

**NC region:** Platform-specific 2MB block mapped as MAIR index 2 (Normal Non-Cacheable, Inner Shareable) via L2 table entry in `vmm.c`. Reserved from PMM in `pmm.c`.
- Pi 5: `0xFFE00000` (last 2MB of 4GB RAM)
- Jetson: `0xBDE00000` (last 2MB of region 1, before OP-TEE carveout)

**Allocator:** `ncmem_alloc(size, align)` in `kernel/include/ncmem.h` / `kernel/mm/ncmem.c`. Simple bump allocator, no free. Used for permanent kernel-lifetime structures.

**Scheduler run queues:** `cpu_rq(cpu)` returns an NC address computed from `NC_MEM_BASE` (compile-time constant). No cacheable pointer indirection — secondary CPUs can compute the address without reading any cacheable data.

**Spinlock separation:** `rq_lock[MAX_CPUS]` is a separate cacheable array. ARM exclusive load/store (`ldaxr`/`stxr`) used by spinlocks requires cacheable memory on BCM2712. The lock is NOT in the `cpu_runqueue` struct — it's accessed via `rq_lock_irqsave(cpu)` / `rq_unlock_irqrestore(cpu, flags)`.

**Task table:** `task_table` is allocated from NC memory at boot via `task_table_init()` in `task.c`. All task struct fields are NC-visible. The `task_table_fallback[MAX_TASKS]` BSS array is used on platforms without NC memory. Task STACKS remain in cacheable PMM (only accessed by owning CPU).

**When to use NC memory:** Only for data that MUST be visible across CPUs without cache maintenance. NC memory is slower than cached memory (every access goes to DRAM). Do not use for hot-path per-CPU data.

**NC memory layout:**
| Offset from NC_MEM_BASE | Size | Contents |
|---|---|---|
| 0x000 | 256B | `cpu_runqueue[MAX_CPUS]` (run queue metadata) |
| 0x100 | 24KB | `task_table[MAX_TASKS]` (all task structs) |
| ~0x6100 | ... | Available for future NC allocations |

**Cross-CPU dispatch status (April 2026):** NC run queues + NC task table are validated. CPU 0 pinning is still active because removing it causes hangs — see `docs/pi5-baremetal-status.md` Known Limitations item 3 for investigation notes.

---

## Idle Task DAIF

The idle task's `msr daifclr, #2` (IRQ unmask) must be **inside** the `while(1)` loop, not before it. When idle is preempted by the timer ISR, ARM hardware masks IRQ on exception entry. `context.S` saves this masked DAIF into idle's context. On resume, the restored DAIF keeps IRQ masked. If the unmask is only at function entry, idle would loop forever in `wfi` with IRQ disabled.

---

## Platform Abstraction

- Use compile-time `#ifdef` for driver selection (UART, timer)
- Platform-specific values go in `kernel/include/platform.h`
- See `docs/platform-abstraction.md` for full strategy

---

## Buddy Allocator (PMM)

The physical memory manager uses a buddy allocator (`kernel/mm/pmm.c`).

### Key Implementation Details

**Orders and Block Sizes:**
- Order 0: 1 page (4KB)
- Order 18: 262144 pages (1GB) - maximum
- Total: 19 orders (0-18)

**Data Structures:**
```c
struct free_block {
    struct free_block *next;
    struct free_block *prev;  // Doubly-linked for O(1) removal
};

struct buddy_state {
    struct free_block *free_lists[MAX_ORDER + 1];  // One list per order
    size_t free_counts[MAX_ORDER + 1];             // Blocks free at each order
    // ... statistics
};
```

**Buddy Address Calculation:**
```c
// Two blocks are buddies if XOR gives the parent block address
uintptr_t buddy_addr = block_addr ^ (PAGE_SIZE << order);
```

**Allocation:**
1. Round request up to power of 2 (order = log2_ceil(count))
2. Search from target order up to MAX_ORDER for a free block
3. Split larger blocks recursively, adding smaller halves to free lists

**Freeing:**
1. Round count up to power of 2 to find block order
2. Check if buddy is free at same order (look it up in free list)
3. If buddy is free, remove it and merge into parent (order + 1)
4. Repeat until buddy is not free or MAX_ORDER reached

**Statistics API:**
```c
void pmm_get_buddy_stats(struct pmm_buddy_stats *stats);
// Returns free_counts[], alloc_count, free_count, split_count, merge_count
```

### Testing

Tests in `kernel/tests/test_pmm.c` verify:
- Basic alloc/free
- Power-of-two rounding
- Block splitting creates correct buddies
- Coalescing enables larger allocations
- Exhaustion and recovery
- Mixed workload stress

---

*Last updated: December 2025*
