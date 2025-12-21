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

## C11 Freestanding Headers

In bare-metal code, only these standard headers are safe (no libc required):

| Header | Provides |
|--------|----------|
| `<float.h>` | Floating-point limits |
| `<iso646.h>` | Alternative operator spellings |
| `<limits.h>` | Integer limits |
| `<stdalign.h>` | `alignas`, `alignof` |
| `<stdarg.h>` | `va_list`, `va_start`, `va_arg`, `va_end` |
| `<stdbool.h>` | `bool`, `true`, `false` |
| `<stddef.h>` | `NULL`, `size_t`, `ptrdiff_t`, `offsetof` |
| `<stdint.h>` | `uint32_t`, `int64_t`, `uintptr_t`, etc. |
| `<stdnoreturn.h>` | `noreturn` macro |

**NOT safe:** `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>` — these require libc.

---

## C11 Strict Compliance

The kernel is compiled with `-std=c11` (no GNU extensions). This means:

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

**Use `_Alignas` instead of `alignas` (or include `<stdalign.h>`):**
```c
/* Works without stdalign.h */
_Alignas(16) uint8_t buffer[64];

/* Requires #include <stdalign.h> */
alignas(16) uint8_t buffer[64];
```

---

## Platform Abstraction

- Use compile-time `#ifdef` for driver selection (UART, timer)
- Platform-specific values go in `kernel/include/platform.h`
- See `docs/platform-abstraction.md` for full strategy

---

*Last updated: December 2025*
