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

**Same pattern for work-stealing:** `steal_deque_lock[MAX_CPUS]` in `kernel/sched/sched.c` is also a separate cacheable array. The deque struct itself (`steal_deque_t`) lives in NC memory on `PLATFORM_HAS_NC_MEMORY` for cross-CPU visibility, but the lock guarding it must be cacheable — LDAXR/STXR on NC memory is never executed. Pre-S4 the lock was embedded in `steal_deque_t` in NC memory, which caused Pi 5's WORK_STEALING=ON boot to hang (#158) because concurrent pushes corrupted the deque. Acquire `steal_deque_lock[cpu]` before every `steal_deque_*` call; the deque's own API is lockless. See `docs/scheduler.md` §Work stealing for the full model.

**Spinlock hardware mode (Pi 5 + Jetson):** Both platforms now share the same `spinlock_hw_enabled` runtime-flag model (set by `vmm_init` after MMU enable). Pre-MMU the flag is 0 and every `spin_lock_irqsave` falls through to a barrier-only path. Post-MMU it is 1, so LDAXR/STXR run for real and provide proper cross-CPU mutual exclusion on cacheable memory. Jetson previously defined `SPINLOCK_SKIP_LOCKING` unconditionally in `platform.h`, which silently turned every cacheable spinlock (PMM, task-table, rq_lock, steal_deque_lock) into a no-op. That surfaced as issue #166 — a `pmm_free_pages` free-list page fault during `bench stealing` with WORK_STEALING=ON, because multiple CPUs mutated the buddy free list without serialization. If you need a cross-CPU critical section on Jetson, a plain cacheable `spinlock_t` + `spin_lock_irqsave` is the right primitive (not barrier-only, not NC-embedded).

**Pi 5 spinlock DRAM flush (April 16, 2026):** On BCM2712 with SMPEN unset, `STLR` from `spin_unlock` stays in the releaser's L2 and never propagates to other CPUs' L2. A waiter's `LDAXR` then reads a stale "locked" cacheline from its own L2 and spins forever (observed as effective deadlock under `sched_try_steal` contention during yield-heavy workloads). `kernel/include/spinlock.h` gates a Pi 5-only path under `#if defined(PLATFORM_RASPI5)` that adds `DC CIVAC` + `DSB SY` before every `LDAXR` (invalidate stale local copy so the load goes to DRAM) and after every `STLR` (push the unlocked value through to DRAM so other CPUs' reads see it). QEMU, Jetson, and x86-64 are unaffected. This fix is what makes cross-CPU integration tests reliable on Pi 5; without it, any scheduler path that contends `rq_lock` or `steal_deque_lock` across CPUs can stall.

**Task table:** `task_table` is allocated from NC memory at boot via `task_table_init()` in `task.c`. All task struct fields are NC-visible. The `task_table_fallback[MAX_TASKS]` BSS array is used on platforms without NC memory. Task STACKS remain in cacheable PMM (only accessed by owning CPU).

**Current-task pointers:** `current_task[MAX_CPUS]` is also allocated from NC memory in `task_table_init()` (SCHED-C2 fix, April 2026). Previously this lived in BSS and relied on DC CIVAC for cross-CPU visibility, which has a known failure mode on Pi 5 (writer's stale cacheline can be written back during an invalidate, clobbering a newer cross-CPU write). NC relocation makes writes from one CPU's `task_set_current` instantly visible to other CPUs' `task_current` without cache maintenance. `current_task_fallback[MAX_CPUS]` is the BSS fallback if NC allocation fails.

**When to use NC memory:** Only for data that MUST be visible across CPUs without cache maintenance. NC memory is slower than cached memory (every access goes to DRAM). Do not use for hot-path per-CPU data.

**NC memory layout** (allocator is a bump allocator — offsets shown are approximate, based on allocation order in `scheduler_init` then `task_table_init`):
| Contents | Size | Notes |
|---|---|---|
| `cpu_runqueue[MAX_CPUS]` | ~256B | First alloc; address pinned via `cpu_rq()` computing from `NC_MEM_BASE` |
| `nc_cpu_logical_map[MAX_CPUS]` | ~32B | MPIDR→logical CPU map |
| `sched_diag_*[MAX_CPUS]` | ~1KB | Diagnostic counters (tick, schedule, picked, idle_loops) |
| `task_table[MAX_TASKS]` | ~24KB | All task structs |
| `current_task[MAX_CPUS]` | ~32B | Per-CPU current running task pointer |
| (Trace slots at `NC_MEM_SIZE - 256`) | 256B | Reserved for `nc_trace.h` diagnostics (not allocated) |

**Cross-CPU dispatch status (April 16, 2026):** FULL. `bench smp`, `bench stealing`, and all 15 integration tests pass on Pi 5 hardware when secondary CPUs wake as expected; see issue #216 for a boot-to-boot dormancy pattern that occasionally keeps one or more secondaries from entering `schedule()` and knocks out unrelated multi-CPU tests. Timer-based preemption on secondary CPUs still uses the cooperative path (CNTPCT-driven tick at yield points); hardware timer IRQ delivery remains a separate blocker tracked under #134. NC run queues, NC task table, NC current-task pointers. All tasks run with `DAIF.I=1` (no in-task timer preemption). See this file's "Pi 5 spinlock DRAM flush" section for what unblocked the last gap.

**`test_work_stealing_distributes_load` success criterion (April 16, 2026):** The test now asserts "at least one task ran on a CPU other than the owner (CPU 1)" — the semantic meaning of "stealing distributes load". The earlier assertion required ≥2 distinct stealer CPUs per attempt, which flaked under the #216 dormancy pattern whenever only one of CPUs 2/3 was alive (a single awake stealer grabs all 5 tasks before the others wake, giving `distinct == 1` but still successfully moving work off the owner). On `PLATFORM_RASPI5`, the test prints a `ws-diag` line per attempt with per-CPU `(steal_attempts / successes / stale / schedule / picked)` deltas so future flake investigations can tell "no stealer awake" apart from "one stealer monopolized" apart from "stealing rejected" without reflashing diagnostic builds.

**`sched_migrate_task` double-queue race (fixed April 16, 2026):** Between `task->assigned_cpu` read at the top and `rq_lock` acquire, a work-stealing thief on another CPU can pull the task from `old_cpu`'s queue onto its own. The original code then called `remove_from_cpu_queue_locked(task, old_cpu)` — which returned 0 silently because the task was no longer there — and followed it with `add_to_cpu_queue_locked(task, target_cpu)` unconditionally, linking the task into `target_cpu`'s queue while it was still on the thief's queue. Two CPUs then picked the same task and ran its entry wrapper on shared stack state. Fix in `sched.c`: check the `remove_from_cpu_queue_locked` return value; if zero, just update `task->assigned_cpu` for future placement instead of queuing. Also added `smp_notify_cpu(target_cpu)` after the unlock so the target CPU reliably wakes from WFE even when the post-unlock broadcast SEV is missed. The DC CIVAC spinlock fix made stealing effective enough that this race surfaced on every test run; before it, stealing rarely succeeded so the race was hidden.

**Leaky tests that block CPU 1 (fixed April 16, 2026):** `task_destroy` silently refuses to reclaim tasks that are not in state `TASK_TERMINATED` (it warns and returns). Test loops that time out waiting for a task to run and then call `task_destroy` therefore LEAK the unterminated task into its target CPU's run queue. On Pi 5, `test_isolated_core_latency` was the worst offender — 10+ `lat_iso` tasks at `TASK_PRIORITY_HIGH` pinned to CPU 1 could accumulate and block every subsequent multi-CPU integration test. When writing or modifying a test that creates a task it expects to run to completion, ALWAYS `scheduler_terminate_task(t)` on the timeout branch before `task_destroy(t)`. The same pattern applies to any code that drops a task reference it expected to finish.

**Snapshot/lock/recheck pattern for `task->assigned_cpu` reads (April 2026):** Anywhere a function reads `task->assigned_cpu` outside `rq_lock` to choose which CPU's queue lock to take, follow the pattern below. A concurrent work-stealing thief or `sched_migrate_task` on another CPU can change `assigned_cpu` between the read and the lock acquire, causing the wrong CPU's lock to be taken silently. `sched_migrate_task`, `scheduler_remove_task`, and `scheduler_terminate_task` use this pattern; new code that touches `assigned_cpu` outside the lock must too.

```c
for (int attempts = 0; attempts < 8; attempts++) {
    uint32_t cpu = task->assigned_cpu;
    if (cpu >= cpu_count) return;       /* defensive */
    irq_flags_t flags = rq_lock_irqsave(cpu);
    if (task->assigned_cpu != cpu) {
        rq_unlock_irqrestore(cpu, flags);
        continue;                        /* moved between read and lock */
    }
    /* ... do the work under cpu's rq_lock ... */
    rq_unlock_irqrestore(cpu, flags);
    return;
}
WARN("retry budget exhausted");          /* should be unreachable */
```

The 8-attempt bound is generous: each successful steal/migrate moves a task at most once per scheduling cycle, so retries succeed in 1-2 attempts in practice. `WARN` on exhaustion makes any pathological wedge visible. Regression coverage: `test_buffer_destroy_busy_keeps_slot` in `kernel/tests/test_ipc.c` exercises a related lock-ordering invariant.

---

## Idle Task DAIF

The idle task's `msr daifclr, #2` (IRQ unmask) must be **inside** the `while(1)` loop, not before it. When idle is preempted by the timer ISR, ARM hardware masks IRQ on exception entry. `context.S` saves this masked DAIF into idle's context. On resume, the restored DAIF keeps IRQ masked. If the unmask is only at function entry, idle would loop forever in `wfi` with IRQ disabled.

**Pi 5/Jetson platform split:** CPU 0's idle task does `daifclr` + `wfi` (timer-driven preemption). Secondary CPUs use `wfe` only (cooperative via SEV) because timer IRQs on secondary CPUs cause an exception handler hang (under investigation). This means `pit_ticks` only advances when CPU 0 is idle.

---

## ARM64 Hardware Timer IRQs — cooperative preemption (April 2026)

**Hardware timer IRQs do not deliver to EL1/EL2 on Pi 5 or Jetson.** The GIC in both platforms runs with two security states; the Group register that routes a PPI to IRQ vs FIQ is owned by EL3 firmware and Non-secure writes are silently ignored. Pi 5 evidence: `docs/archive/investigations/pi5-preemption-resolution.md`. Jetson evidence: `docs/archive/investigations/jetson-preemption-investigation.md` — an 8-path empirical investigation confirmed every NS-accessible route is blocked (PPIs/SGIs/SPIs all Group 0, ICC_IGRPEN0 reads trap to EL3, SCR_EL3.FIQ=1 routes FIQ to EL3).

**Diagnostics:** The `timdiag` shell command (`kernel/src/shell_sys.c`) dumps live GIC and timer state on any ARM64 platform. Run after boot to see the current group configuration. The command deliberately skips `ICC_IGRPEN0_EL1` reads because TF-A traps them (causes EC=0x18 exception on Jetson). Optional `timdiag fiq` argument runs an additional FIQ delivery test — DO NOT use on Jetson, it writes ICC_IGRPEN0 which crashes the EL3 handler.

**Resolution — `COOP_PREEMPT` (CMake option, default `ON` for RASPI5 and JETSON_ORIN_NANO):** `schedule()` checks `CNTPCT_EL0` on every entry and synthesizes a `scheduler_tick()` call whenever ≥10 ms has elapsed on that CPU since the last tick. See `coop_preempt_maybe_tick()` in `kernel/sched/sched.c`. This drives the AI scheduler, deadline boosts, migration, and `pit_ticks` / `timer_handler_count` observability at yield points rather than preemptively.

The `PI5_COOP_PREEMPT` spelling is retained as a deprecated Makefile/CMake alias for backward compatibility; source code uses `COOP_PREEMPT` everywhere.

**Consequences:**
- `pit_ticks` and `timer_handler_count` advance on all CPUs when those CPUs yield.
- A pure CPU-bound loop that never yields still monopolizes its CPU. The **`slm_preempt_point()`** macro in `kernel/include/preempt_point.h` is the policy fix: any in-tree loop that may iterate >1 000 times without a yielding primitive must call it on the back-edge. Cheap when the quantum hasn't expired (~5 instructions); falls into `schedule()` when ≥10 ms has elapsed. Compile-time no-op on non-`COOP_PREEMPT` platforms. See `docs/scheduler.md` §"Preemption Model" for the full policy.
- `timer_get_count()` (CNTPCT_EL0) remains the authoritative wall-clock source for timeouts; see `hw_timeout_start()` / `hw_timeout_expired()` in `component_runtime.c`. Works regardless of IRQ delivery state.

**`sched_set_policy()` must hold IRQs disabled.** The function wraps init/swap/shutdown in `irq_save`/`irq_restore`. Retained because the ELR-trampoline path (PR #98, inert under coop-preempt but kept for future hardware-IRQ restoration) still relies on it.

---

## Secondary-CPU preemption — `SECONDARY_PREEMPT` (April 2026)

The ELR-trampoline infrastructure (`kernel/sched/preempt.c`,
`resched_trampoline` in `kernel/arch/arm64/vectors.S`) is gated behind
the CMake option `SECONDARY_PREEMPT` (default OFF). When ON, timer IRQs
on secondary CPUs are deferred to task context via the trampoline
rather than calling `schedule()` from the ISR (which corrupts the
abandoned exception frame on real ARM64 hardware).

**Invocation:**

- `make kernel SECONDARY_PREEMPT=ON` — enable on any ARM64 platform.
- `make kernel PI5_SECONDARY_PREEMPT=ON` — deprecated alias; still maps
  to `SECONDARY_PREEMPT=ON`. Pi 5 Makefiles that use the old name keep
  working.

**Platform status:**

- **Pi 5:** functional in combination with `COOP_PREEMPT` (the
  trampoline path is inert today because timer IRQs don't deliver;
  infrastructure kept for when #134 restores hardware IRQ delivery).
- **Jetson:** compiles but **not safe to enable yet** — the
  `resched_trampoline` in `vectors.S:377-380` uses
  `(mpidr & 0xFF) | ((mpidr >> 8) & 0xFF)` to compute the CPU index,
  which collides on dual-cluster CPU 4/5. Jetson plan P3 step 2 owns
  the fix. **Enforced at boot (#137):** `preempt_check_cpu_mpidr`
  (kernel/sched/preempt.c) panics from `scheduler_init` /
  `secondary_init` if the fold disagrees with the caller's logical
  CPU id, so the soft documentation warning can no longer be
  bypassed silently — a Jetson build with `SECONDARY_PREEMPT=ON`
  halts loudly on the first secondary bring-up.
- **QEMU:** works today but rarely needed — QEMU's timer IRQ from an
  ISR doesn't crash the kernel; the default cooperative path is fine.

The option was originally `PI5_SECONDARY_PREEMPT`; it was renamed
during the Jetson capstone Prereq #2 (commit `b820bee`) so Jetson
builds could compile the trampoline, which had been gated behind
`PLATFORM STREQUAL "RASPI5"`. See `docs/smp.md` §"Secondary-CPU
Preemption" for the full history.

---

## Cross-CPU notification — `smp_notify_cpu()`

Scheduler code calls `smp_notify_cpu(cpu)` to wake a specific CPU that
may have just gained work on its run queue. API in
`kernel/include/smp.h`, platform implementations split:

- **ARM64** (`kernel/sched/smp.c`): `sev` broadcast. SEV wakes every
  CPU in WFE and the target picks up the task on its next idle loop
  iteration. Upgrade to `gic_send_sgi(cpu, SGI_RESCHED)` once Jetson
  plan P3 step 3 fixes `gic_send_sgi` for dual-cluster MPIDR.
- **x86-64** (`kernel/arch/x86_64/platform_x86.c`): LAPIC IPI on
  `RESCHED_VECTOR` (49). The handler calls `schedule()` directly —
  not `scheduler_tick()` — so quantum accounting stays owned by the
  local timer. Runs on IST1 so a task-stack overflow cannot corrupt
  the IPI frame.

Both backends short-circuit self-notifications and out-of-range
`cpu` ids. Replaces the previous
`#if !defined(PLATFORM_X86_64) __asm__ volatile("sev") #endif` pattern
in `scheduler_add_task_to_cpu()`. Platform-specific wake logic no
longer leaks into `kernel/sched/sched.c`. See `docs/smp.md` §"IPI /
Cross-CPU Notification".

---

## x86-64 LAPIC post-kexec gotcha (April 2026)

`kernel/arch/x86_64/lapic.c`'s `lapic_force_xapic_mode()` runs at
the top of `lapic_init` and `lapic_percpu_init` to make the LAPIC
is in xAPIC (MMIO at `0xFEE00000`) mode no matter what the prior
boot environment left behind. Two relevant cases:

- **Fresh UEFI/GRUB boot:** APIC_BASE already has `EN=1`, `EXTD=0`.
  `lapic_force_xapic_mode` is a no-op (it early-returns).
- **Kexec from Linux with x2APIC enabled:** Linux's
  `lapic_shutdown()` disables the APIC via SVR but leaves
  `IA32_APIC_BASE.EXTD=1`. MMIO at `0xFEE00000` then returns
  `0xFFFFFFFF` (the window is inactive in x2APIC mode) and every
  downstream LAPIC read is garbage (typical symptom:
  `[LAPIC] Initialized ... ID=255, version=0xff` followed by a
  hang when `lapic_timer_setup` writes to dead MMIO).

The transition `x2APIC → xAPIC` has to go through the DISABLED
state per Intel SDM Vol 3A §10.12.5 Table 10-6. **Do NOT do the
`EN=0 → EN=1` toggle unconditionally** — some implementations
(verified on QEMU TCG) silently reject the re-enable, locking the
APIC out until RESET. The helper checks `EXTD` first and only
takes the toggle path when it's actually set; a healthy xAPIC
APIC is left alone.

Similar rule for timer calibration: `lapic.c` and `timer_x86.c`
prefer **CPUID leaf 0x15** (core crystal Hz) with **leaf 0x16**
fallback (base MHz) before touching PIT channel 2, because the
PCH on recent Intel boards disables PIT channel 2 post-kexec and
the busy-wait in the PIT path would otherwise hang forever. The
PIT fallback still exists but has a TSC-based ~200 ms timeout so
it can never hang. On any Skylake-or-newer host, CPUID 0x15
returns the numbers directly and PIT is never touched.

---

## x86-64 GA10x Falcon — Per-page IMEMT in PIO IMEM upload (April 2026)

`kernel/gpu/nvidia/falcon.c:falcon_pio_upload_imem` writes the
page-tag register `IMEMT` once at the start of the upload **and**
re-arms it whenever the byte index crosses a 256-byte boundary.

**Why both writes are needed.** The `IMEMC` AINCW bit auto-
increments the write *address* within a 256-byte page, but the
page tag is a separate register. Without per-page IMEMT updates:

- Non-secure ucode happens to keep working — the running firmware
  never re-validates the tag.
- HS-secure code (Booter Load on SEC2 is the immediate consumer;
  any future >256 byte secure FWSEC path too) silently fails: the
  HS-bootrom recomputes a signature over `(tag, code)` per page
  with the on-disk signature pinned to monotonic tags 0,1,2,...
  When every page reads back as page 0, the signature mismatches
  and the bootrom STOPs at the first instruction. CPUCTL ends up
  at `0x20` (STOPPED), MAILBOX0 still carries the input WPR meta
  (no booter-side response), and `falcon_wait_halted` times out.

Reference: nvgpu's `gk20a_falcon_copy_to_imem` mirrors this in
three sites in
`../slmos-reference-cache/nvidia/nvgpu-hal-falcon-falcon_gk20a_fusa.c`. SLM-OS
matches the pattern. Regression coverage in
`host-tools/gsp-harness/test_falcon.c`:
`test_pio_imem_multi_page_writes_tag_per_page` (4-page upload,
asserts 4 tag writes), `test_pio_imem_sub_page_writes_one_tag`
(252-byte upload, asserts 1), `test_pio_imem_exact_page_writes_one_tag`
(256-byte upload, asserts 1 — the boundary check fires AT the
crossing, not at the final word of the page being filled).

`falcon_pio_upload_dmem` does NOT need this — DMEM has no page
tag.

---

## x86-64 GA10x — SEC2 BROM aperture state (corrected April 2026)

**Retraction notice:** A prior version of this section claimed the
SEC2 BROM aperture stays priv-locked even under nouveau, citing
`peek BAR0+0x842200..220` returning `0xbadf5040` from both Linux
and SLM-OS post-kexec. That conclusion was based on **reading the
wrong offsets**.

`NV_PSEC2_BROM_BASE = 0x00841000` (`falcon.h:39`). The real Falcon-v4
BROM registers per `falcon.h:168-171` are:

- `FALCON_BROM_PARAADDR0 = 0x210` → absolute `0x841210`
- `FALCON_BROM_UCODE_ID  = 0x198` → absolute `0x841198`
- `FALCON_BROM_ENGIDMASK = 0x19C` → absolute `0x84119C`
- `FALCON_BROM_MOD_SEL   = 0x180` → absolute `0x841180`

`peek 0x53842200/210/220` (BAR0=0x53000000) hit BAR0+0x842200..220 =
`NV_PSEC2_BROM_BASE + 0x1200..1220` — **0x1000 above the real BROM
register window**, in unmapped space that always returns PRI poison.
The post-fail diagnostic in `nvidia_gpu.c` originally peeked
`BROM_BASE + 0x010 / 0x004` (also unmapped) and labelled the result
"SEC2 BROM MOD_SEL = 0xbadf5720" — same bug, same wrong conclusion.

**Source-read of nouveau confirms the real story.** `ga102_flcn_fw_boot`
(`../slmos-reference-cache/nouveau/nouveau-falcon-ga102.c:113-123`) writes the BROM
selectors via plain BAR0 MMIO at exactly the same `0x841180/198/19c/210`
addresses SLM-OS uses in `kernel/gpu/nvidia/bringup.c:710-716`. There
is no DMEMMAPPER fixup; the booter HS blob carries only the
`(fuse_ver, engine_id, ucode_id)` triple in its meta_data block
(`../slmos-reference-cache/nouveau/nouveau-gsp-ga102.c:41-92` `ga102_gsp_booter_ctor`).

**Phase 2 STOPPED root cause located + fixed (PR #289, 2026-04-18).**
The diagnostic added in PR #288 surfaced the actual bug on the next
hardware iteration: BOOTVEC was set to `os_code_offset` (= 0 = the
non-secure preamble at IMEM[0]) when it should be `apps[0].offset`
(= 0x100 = the secure entry point the HS-bootrom jumps to after
signature verify). With BOOTVEC=0, the bootrom verified the
signature successfully but jumped to the non-secure preamble and
immediately STOPPED (CPUCTL=0x20). All three pre-iteration
candidates (Falcon2 select PLM, engine_id mismatch, reset
sequencing) turned out to be false trails — the wrong one-line
field assignment is what only the runtime diagnostic could catch.

After the fix (`b->booter_boot_addr = img.apps[0].offset` in
`gsp_bringup_set_booter_layout`), Falcon executes booter code on
the next post-kexec attempt — CPUCTL drops from 0x20 to 0x00. The
booter then hangs waiting for valid `GspFwWprMeta` data, which
SLM-OS intentionally zeros (per `bringup.c:638-641`); populating
WprMeta correctly is the documented E4 boundary.

**Lessons for future GA10x bringup work:**

- The Falcon-select PLM dance only applies to engines with
  RISC-V cores (GSP). SEC2 has no RISC-V core so the no-op skip
  in `falcon_select_falcon_mode` is correct.
- BROM register reads/writes go to `BROM_BASE + 0x180/198/19C/210`,
  not the `+0x010/004` or `+0x1200..1220` offsets the original
  diagnostic was peeking. PR #288 has the corrected offsets.
- For HS booter blobs (R535 booter_load), BOOTVEC must be
  `apps[0].offset`, not `os_code_offset` — see OGKM
  `../slmos-reference-cache/nvidia/ogkm-kernel_gsp_falcon_ga102.c:278` and nouveau
  v2 `../slmos-reference-cache/nouveau/nouveau-falcon-fw.c:351`. Pinned in
  `host-tools/gsp-harness/test_bringup.c:test_booter_layout_*`.
- When source-reading converging-but-wrong candidates from
  multiple references (Jetson code, nouveau, OGKM), add a runtime
  diagnostic that exposes the parsed values FIRST. Three independent
  reference investigations agreed on a wrong answer here; the
  printed values were what made the actual bug obvious.

---

## x86-64 GA10x — GspFwWprMeta Stage A (April 2026)

After PR #289 unblocked Phase 2 (Falcon executes booter, CPUCTL=0x00),
the next observed failure mode was an infinite hang inside SEC2.
Booter validates `magic`/`revision`, then immediately walks
`sysmemAddrOfRadix3Elf` to find the GSP-RM ELF — and SLM-OS at that
point left the entire 256-byte WprMeta buffer zeroed, so the walk
NULL-deref'd silently (no MAILBOX0 update, no halt, just frozen
Falcon). Stage A populates the bare-minimum fields needed for
booter to walk the chain without faulting, then advance to a
*different* failure that reports a discrete MAILBOX0 status code.

**Layout pinning is load-bearing.** `kernel/gpu/nvidia/gsp_wpr_meta.h`
copies the struct definition verbatim from
`../slmos-reference-cache/nouveau/nouveau-r535-nvrm-gsp.h:417-555` and adds 11
`_Static_assert`s pinning sizeof + every field offset booter or
SEC2 reads directly. If a future maintainer reorders fields or
forgets a `uint64_t` somewhere, the build breaks instead of SEC2
quietly trashing GSP-RM state. Field offsets to remember:
`magic=0x00`, `revision=0x08`, `sysmemAddrOfRadix3Elf=0x10`,
`gspFwWprStart=0x70`, `gspFwWprEnd=0xa8`, `fbSize=0xb0`,
`verified=0xf8`. Total size **must** be exactly 256 bytes.

**Radix3 chain shape** (`gsp_radix3_fill_dummy_chain`): three 4 KB
DMA pages plus one dummy ELF page. L0[0] = L1 IOVA, L1[0] = L2
IOVA, L2[0] = ELF IOVA, every other entry zeroed. Single-entry
shape is the Stage A simplification — production GSP-RM ELF spans
many L2 pages and `sizeOfRadix3Elf` would be the actual ELF byte
length, not 4096. Reference: nouveau `nvkm_gsp_radix3_sg`
(`../slmos-reference-cache/nouveau/nouveau-gsp-r535.c:1656-1713`).

**What Stage A deliberately leaves zero.** Bootloader address +
size + offsets, signature address + size, heap fields, partition
RPC, ELF code/data sections. Booter is *expected* to halt with a
MAILBOX0 status code when it tries to use one of these — that's
the signal Stage A produces. If the post-iteration MAILBOX value
indicates "magic invalid" or "WPR2 mismatch" instead, the bug is
in Stage A's struct/constant values (FB size, WPR2 boundaries are
GA107-specific in `bringup.c`).

---

## Jetson GA10B — Ampere compute dispatch uses PCAS2_B (April 2026)

`kernel/gpu/nvidia/ga10b_bringup.c:ga10b_build_launch_kernel_pushbuffer`
emits `SEND_SIGNALING_PCAS2_B` (method `0x02C0`) with `PCAS_ACTION
= INVALIDATE_COPY_SCHEDULE` (`0xA`) as the dispatch-kick method.
**Do not "unify" this with the Turing-era `SEND_SIGNALING_PCAS_B`
(`0x02BC`) with `{INVALIDATE, SCHEDULE}` bits.** Using PCAS_B on
GA10B is the quietest failure mode in the GPU stack: PBDMA
consumes the pushbuffer, `GP_GET` advances, no dmesg error, no
fault notifier — but the dispatch never reaches the SMs. The
kernel silently does not run.

Source: `../slmos-reference-cache/mesa/mesa-nvk_cmd_dispatch.c:322-340` — NVK
branches on `cls_compute <= TURING_COMPUTE_A`:

```c
if (cls_compute <= TURING_COMPUTE_A)
    P_IMMD(p, NVA0C0, SEND_SIGNALING_PCAS_B,  {INVALIDATE, SCHEDULE});
else
    P_IMMD(p, NVC6C0, SEND_SIGNALING_PCAS2_B, INVALIDATE_COPY_SCHEDULE);
```

Ampere (`AMPERE_COMPUTE_B = 0xC7C0`) falls in the `else` branch.

Regression protection:
`host-tools/gsp-harness/test_ga10b_bringup.c:test_launch_kernel_pb_uses_ampere_pcas2_b`
pins both the method id (`0x02C0 / 4`) and the action value (`0xA`)
at build time. This test also decodes the opcode/subch/data fields
independently so a refactor that changes the HDR macros but keeps
the method/action combination still passes.

---

## Pi 5 SDHCI deferred bring-up + 50 ms settle delay (#414, April 2026)

`sdhci_create_bcm2712()` runs **post-scheduler-init only**, gated by
`boot_media_allow_creates()` (called from `kernel_main` right after
`scheduler_init`). It also busy-waits 50 ms before its first MMIO
touch (`BCM2712_EMMC2_SETTLE_US` in `kernel/drivers/sdhci.c`).

**Why both gates exist:**

- The VC firmware on `pieeprom-2024-09-23.bin` is still finishing the
  EMMC2 unlock at kernel handoff. AON GPIO (`0x107D517C00` —
  `sd_vcc_reg` pin 4, `sd_io_1v8_reg` pin 3), the property mailbox
  (`0x107C013880`), and `SDIO_CFG_*` are all reachable but produce an
  AXI fabric hang on the first read for ~50 ms. Empirically 50 ms
  works; 500 ms also works; 10 ms does not.
- The full bring-up (settle delay + AON regulator program + mailbox
  RPC + `sdhci_brcmstb_cfginit_2712` + CMD0/CMD8/ACMD41/CMD2/CMD3/CMD9)
  takes long enough that running it from the pre-scheduler VFS-init
  path made secondary CPUs miss `scheduler_is_initialized` and hang
  the boot. Deferring it past `scheduler_init` keeps the secondary
  bring-up window clean.

**Keep-alive ref.** `kernel/src/boot_media.c` initializes
`g_boot_media_refs = 2` after the first successful create — one ref
for the caller, one keep-alive ref pinning the device for the
kernel's lifetime. This stops the 50 ms / full-init cost from being
paid per acquire/release pair (e.g. `blob_autoload` walking several
FAT entries back to back). Without it, every release dropped refcount
to 0 and the device was destroyed and recreated.

**Diagnostic shell command.** `emmc-bringup` (RASPI5-only, in
`kernel/src/shell_sys.c`) calls `sdhci_pi5_bringup_now()` from the
shell. Useful for differentiating "boot-time firmware state is wrong"
from "hardware state is wrong" — if the shell-driven bring-up
succeeds while a future regression breaks the boot-time path, the
boot-time gate / delay needs adjusting; if both fail, suspect the
hardware path itself.

Regression coverage:
`kernel/tests/test_boot_media.c:test_suite_boot_media` exercises the
gate (`acquire` returns NULL pre-`allow_creates`), the keep-alive ref
(repeated acquire/release pairs invoke `boot_media_create` exactly
once), and the test-device override path that bypasses both gates so
existing fixtures keep working.

---

## UART Lock on Pi 5 / Jetson

On platforms with `PLATFORM_HAS_NC_MEMORY`, the UART lock uses **IRQ-disable-only** (no cross-CPU lock). Standard `ldaxr`/`stxr` spinlocks deadlock under cross-CPU contention because per-core L2 caches are incoherent (no SMPEN). LSE atomics (`SWPALB`) also operate through L2 and have the same problem. NC memory atomic ops may fault (implementation-defined per ARM ARM).

This is safe because secondary CPUs do not print after boot (the `secondary_init` comment at smp.c:328 documents this). All UART output during normal operation comes from CPU 0 (shell, tests, INFO logs). If secondary CPU printing is needed in the future, an NC-memory-based lock must be implemented.

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

### Firmware-supplied /memreserve/ ranges

`pmm_init` calls `pmm_add_region_split` (not the bare `pmm_add_region`)
for every memory region it adds to the buddy allocator. The wrapper
queries `dtb_get_memreserves()` and carves any reservation out of
the region before adding the surviving subranges.

Two reserve encodings are honored — both produced by the same
`dtb_get_memreserves` call:

1. **FDT header reserve map** (`/memreserve/ <addr> <size>;` directive
   at DTS file scope). Standard format; `dtc` emits 16-byte entries
   (`uint64_t addr`, `uint64_t size`) at `off_mem_rsvmap`.
2. **Root-node `memreserve` property** (Pi 5 firmware convention).
   Property data is a sequence of `(uint32_t addr, uint32_t size)`
   cells — half the width. Pi 5 uses this for its VPU shared-memory
   carveout (typically 4 MB at `0x3fc00000`).

The pure carve helper `pmm_carve_reserves` lives in `kernel/mm/pmm.c`
and is exposed via `kernel/include/pmm_internal.h` for the regression
tests in `kernel/tests/test_pmm.c`. It handles unsorted reservation
lists, zero-size entries, reservations entirely outside the target
region, and reservations that fully swallow the region.

**Re-entry guard (PR #598):** `pmm_add_region_split` uses file-scope
scratch arrays (`pmm_split_*`) shared across calls. The "single-
threaded during boot" precondition is now enforced by an unconditional
`if (in_split) panic(...)` check at the top of the function — a
future caller that re-enters concurrently surfaces immediately rather
than silently corrupting whichever caller's split state is active.
The check is unconditional (not gated by NDEBUG via ASSERT) because
the function is called only a handful of times during boot.
Implicit regression coverage: `pmm_init` calls the function once per
platform region; a broken `in_split = false` reset would panic during
boot and every test in `test_pmm.c` would fail to even start.

### Testing

Tests in `kernel/tests/test_pmm.c` verify:
- Basic alloc/free
- Power-of-two rounding
- Block splitting creates correct buddies
- Coalescing enables larger allocations
- Exhaustion and recovery
- Mixed workload stress
- `pmm_carve_reserves` (memreserve carve helper) — 11 cases including
  Pi 5's actual VPU carveout, multiple reservations, edge cases at
  region/reservation boundaries, zero-size and unsorted entries

`kernel/tests/test_dtb.c` covers the DTB-side parsers (FDT header
reserve map vs root-node `memreserve` property, `/chosen` entropy,
`/chosen/bootloader` metadata).

---

*Last updated: December 2025*
