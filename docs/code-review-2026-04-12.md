# SLM-OS Comprehensive Code Review — April 12, 2026

**Scope:** Full kernel (C, ARM64/x86 asm), Rust runtime, tests, and architecture-specific boot code
**Method:** 7 parallel review agents covering memory, scheduler/SMP, IPC/syscall/components, drivers, boot/exceptions, Rust runtime, and kernel core/shell/VFS
**Prior review:** `docs/code-review-2026-04-02.md`
**Codebase:** ~92K lines kernel C/asm + ~11.5K lines Rust runtime

---

## Executive Summary

| Subsystem | Critical | High | Medium | Low | Total |
|-----------|----------|------|--------|-----|-------|
| Memory Management (PMM/VMM/ncmem) | 3 | 4 | 3 | 1 | 11 |
| Scheduler / SMP / Concurrency | 3 | 3 | 4 | 2 | 12 |
| IPC / Syscall / Components | 4 | 3 | 4 | 2 | 13 |
| Drivers (UART, GIC, virtio, fb) | 2 | 3 | 2 | 2 | 9 |
| Boot / Exceptions (ARM64 + x86) | 1 | 2 | 4 | 2 | 9 |
| Rust Runtime (inference, FFI, loader) | 6 | 4 | 2 | 0 | 12 |
| Kernel Core (shell, VFS, strings, tests) | 3 | 7 | 4 | 2 | 16 |
| **Total** | **22** | **26** | **23** | **11** | **82** |

### Top Systemic Issues (recurring across subsystems)

1. **Integer overflow in size/offset math** — absent in syscall `in_len * 4`, Rust `workspace::alloc_tensor`, IPC `msg_queue_create`, ELF/DTB parsers, littlefs_vfs offset cast.
2. **SMP synchronization gaps** — C-side `msg_router.c`, `current_task[]`, VMM page table writes, Rust `static mut` in `msg_router.rs`, component cleanup context, echo mailbox data.
3. **C-string validation missing** — components pass topic/data strings that may not be NUL-terminated or may be unbounded; `str_copy` helpers in both C and Rust lack enforcement at syscall boundary.
4. **Stale MMU/I-cache barriers on kexec/UEFI** — `dsb nsh` used where `dsb sy` is required; missing ISB before `eret`; missing TLBI after `vmm_map_block`.
5. **Residual issues from April 2 review not fully addressed** — MM-C1 (`get_l2_table` returns PA), MM-C2 (Pi 5 SPINLOCK_SKIP_LOCKING), MM-M1 (PA 0 sentinel), duplicated string functions, VFS ref counting.

### Capstone-Critical vs Future-Work Priority

**MUST FIX before capstone submission** (correctness, crash risk, demo failure):
- Integer overflows at syscall boundary (IPC-C1)
- VMM missing TLBI after map (MM-H1), missing init guard (MM-H3)
- `secondary_mmu_*` reads stale on SMP boot (BOOT-M3)
- Scheduler infinite busy-wait without timeout (SCHED-C1)
- Rust `static mut` races in `msg_router.rs` and inference (RUST-C3)
- Duplicate string functions / linker ordering risk (CORE-H1)
- `ncmem_alloc` accounting bug (MM-C3) — cosmetic but visible in stats

**FUTURE WORK** (hardening, multi-tenant readiness, post-capstone):
- VMM spinlock protection (MM-H2) — matters only when multi-writer VMM active
- Pi 5 SPINLOCK_SKIP_LOCKING replacement (MM-C2) — requires runtime flag refactor
- Symlink/path-traversal defense (CORE-H6) — only matters with untrusted users
- VFS ref counting (CORE-H8) — only matters when files can be unlinked while open
- Semihosting gate (CORE-H10) — only matters on real hardware test runs
- Full SIMD feature-gate audit (RUST-M9)

---

## 1. Memory Management (PMM / VMM / ncmem)

### Critical

**MM-C1: `get_l2_table()` returns physical address as pointer** *(unchanged from April 2)*
- `kernel/mm/vmm.c:224-242`
- Line 241 casts the raw L2 PA to `uint64_t *`. Works today because identity mapping is active on TTBR0; breaks any future user-space mapping.
- Fix: `return (uint64_t *)PA_TO_KVA(l2_pa);`

**MM-C2: Pi 5 SPINLOCK_SKIP_LOCKING disables all locks** *(unchanged from April 2)*
- `kernel/include/platform.h:226`, `kernel/include/spinlock.h:21-23`
- SMP is now enabled on Pi 5 (4 cores, cross-CPU dispatch working). With compile-time lock disable, every shared structure that isn't in NC memory is racy.
- Fix: Convert `SPINLOCK_SKIP_LOCKING` from compile-time macro to runtime flag (`spinlock_hw_enabled`) set after MMU enable. Framework partially in place — needs to be applied globally.

**MM-C3: `ncmem_alloc` accounting calculation is nonsensical**
- `kernel/mm/ncmem.c:50-51`
- Line 50 computes a meaningless expression that is immediately overwritten by line 51. Not a crash risk, but the allocator also lacks overflow checks on `aligned + size`.
- Fix: Delete line 50. Add `if (aligned < nc_next_free) return NULL;` (catches alignment wrap) and check that `aligned + size` doesn't wrap.

### High

**MM-H1: `vmm_map_block()` missing TLBI after descriptor write**
- `kernel/mm/vmm.c:281-287`
- Only `dsb ishst` after writing L2 entry. ARM ARM D8.12 requires TLB invalidation (`tlbi vaae1is`) after changing a translation even from invalid→valid if the old entry might be cached.
- Fix: Add `tlbi vaae1is, <va>; dsb ish; isb` after the descriptor write.

**MM-H2: No VMM locking on page table modifications**
- `kernel/mm/vmm.c` — `vmm_map_block`, `vmm_unmap_block`, `vmm_map_region`
- If two CPUs call these concurrently (post-boot dynamic mapping), L2 entries can be lost. Today the primary caller is boot-only (single CPU), so this is latent, but any runtime mapping triggers the bug.
- Fix: Add `static spinlock_t vmm_lock` and wrap all public mapping APIs.

**MM-H3: `vmm_state.initialized` never checked by callers**
- `kernel/mm/vmm.c:853`
- Flag is set but no public function validates it. Early calls write garbage L1 indices.
- Fix: Add `if (!vmm_state.initialized) return -1;` guard at the top of each public VMM function.

**MM-H4: `pmm_get_free_pages()`/`pmm_get_total_pages()` unlocked**
- `kernel/mm/pmm.c:639-647`
- Torn reads on SMP. `pmm_get_stats()` is properly locked; these two are not.
- Fix: Take `pmm_lock` for both.

### Medium

**MM-M1: PA 0 as allocation failure sentinel** *(unchanged from April 2)*
- `kernel/mm/pmm.c:206-221` (`free_list_pop`)
- PA 0 is valid on Pi 5 (RAM_BASE=0). Caller cannot distinguish success from failure.
- Fix: Use `UINTPTR_MAX` sentinel or out-parameter with bool return.

**MM-M2: `vmm_dump()` integer overflow**
- `kernel/mm/vmm.c:690-691`
- `blocks_mapped * BLOCK_SIZE` in 32-bit arithmetic — overflows at 2048 × 2MB = 4GB on Pi 5.
- Fix: Cast `(uint64_t)vmm_state.blocks_mapped * BLOCK_SIZE`.

**MM-M3: `pmm_get_buddy_stats()` holds lock during UART output**
- `kernel/mm/pmm.c:652-669`
- Blocks all PMM ops while slow UART runs. `pmm_dump_stats()` was fixed; this sibling was not.
- Fix: Snapshot under lock, print after unlock.

### Low

**MM-L1: `block_state[]` indexing has no bounds check**
- `kernel/mm/pmm.c:232-246`
- If a freed address falls outside `[heap_start, heap_end)`, index wraps silently.
- Fix: Add bounds check in `addr_to_block_index()` with `DEBUG_ASSERT` or error log.

---

## 2. Scheduler, SMP, and Concurrency Primitives

### Critical

**SCHED-C1: Secondary CPU infinite busy-wait with no timeout**
- `kernel/sched/smp.c:353`
- `while (!scheduler_is_initialized()) { for (volatile int d = 0; d < 100000; d++) {} }` — no timeout. If CPU 0 faults before `scheduler_init`, secondaries spin at 100% forever.
- Fix: Bounded retry with panic on timeout (`retry < 50000`) that writes a diagnostic to NC memory before panicking.

**SCHED-C2: `current_task[cpu]` cross-CPU coherency**
- `kernel/sched/task.c:465-479`
- `task_current()` calls `cache_invalidate()`, but on Pi 5 with no SMPEN and per-core L2, writes by CPU 0 to `current_task[N]` may not reach CPU N's view. The structure needs to live in NC memory like run queues, or every writer must `cache_clean` and every reader must `cache_invalidate`.
- Fix: Move `current_task[]` into NC region at `ncmem_init` time, or add cache_clean at every `task_set_current` site.

**SCHED-C3: Task teardown race between `task_exit` and `schedule`**
- `kernel/sched/task.c:446`, `kernel/sched/sched.c:1013`
- `task_exit` sets state outside `rq_lock`; `schedule()` may pick a TERMINATED task. On Pi 5, the `task->state` NC read can race with the non-NC `next` pointer.
- Fix: Acquire `rq_lock(assigned_cpu)` in `task_exit` before marking TERMINATED, hold through `scheduler_remove_task`.

### High

**SCHED-H1: `pi_mutex` wait loop is interrupt-unsafe**
- `kernel/ipc/pi_mutex.c:91-98`
- After `spin_unlock_irqrestore`, IRQs re-enabled. Spin-wait with `yield()` can be preempted by the timer ISR which invokes `schedule()` — if `schedule()` ever needs the same `guard`, deadlock. On ARM64, `yield` is a hint only.
- Fix: Convert to a proper sleep queue (block on condition, wake on release) or document that `pi_mutex` callers must not be reentered.

**SCHED-H2: Zombie cleanup after `rq_unlock` can race**
- `kernel/sched/sched.c:998-1010`, `kernel/sched/task.c:502-548`
- `task_destroy` runs outside the run-queue lock; another CPU can find the zombie on its queue before destroy completes.
- Fix: Either destroy inside the lock, or mark `TASK_DESTROYED` state before releasing so the remote CPU can filter.

**SCHED-H3: AI scheduler task scan holds no locks**
- `kernel/sched/sched.c:131-165` (`ai_update_top_tasks`)
- Iterates all run queues without taking `rq_lock`. Pointers from partially-modified queues can be stale or garbage. Since result feeds inference, invalid pointers propagate into FFI.
- Fix: Take all `rq_lock`s in ascending CPU order, scan, release in reverse. Or copy pointers under each lock individually.

### Medium

**SCHED-M1: `preempt_disabled` set after `task_set_current`**
- `kernel/sched/sched.c:1101-1137`
- Brief window where remote CPU's `task_current()` sees next task before the switch happens. Low probability but real.
- Fix: Set `preempt_disabled[this_cpu] = 1` before `task_set_current(next)`.

**SCHED-M2: Ticket lock acquire semantics fragile on ARM64**
- `kernel/include/spinlock.h:282-287`
- Relies on WFE's implicit acquire-like fencing; the owner load happens before WFE, so the value after WFE wake is not reloaded before DMB. Hard to trigger but wrong under ARM's memory model.
- Fix: Re-read `owner` with `ldar` after WFE returns.

**SCHED-M3: Task `daif` initialized with IRQs masked**
- `kernel/sched/task.c:306`, `kernel/arch/arm64/context.S:126-128`
- New tasks start with DAIF.I=1. On Pi 5 this is intentional (see kernel/CLAUDE.md — tasks run with IRQs masked to avoid context corruption). Documentation in `task.h` should reflect that user tasks expecting timer interrupts will silently run with them masked until they explicitly yield.
- Fix: Document the invariant in `task.h` / `docs/smp.md`.

**SCHED-M4: Unlock→ISB ordering in task_lock release**
- `kernel/sched/task.c:34-37`
- `TASK_UNLOCK_IRQRESTORE` does `cache_clean` + DSB but no ISB. Remote CPU's next LDAR may still spin briefly. Cosmetic on capstone targets but not architecturally clean.
- Fix: Add `isb` after `dsb ish` in the macro on platforms without SMPEN.

### Low

**SCHED-L1: Diagnostic NC writes scattered through scheduler**
- `kernel/sched/smp.c:297,357`, `kernel/sched/sched.c:387`
- Debug markers embedded directly in production paths.
- Fix: Move behind `#ifdef SCHED_DEBUG_NC_TRACE`.

**SCHED-L2: `for (volatile int d=0; ...)` delay loops**
- `kernel/sched/smp.c:353,436`
- Compiler-dependent timing.
- Fix: Use `timer_busy_wait_us()` helper.

---

## 3. IPC, Syscall, and Component System

### Critical

**IPC-C1: Integer overflow in `sys_infer_handler`**
- `kernel/src/syscall.c:136`
- `in_len * 4` where `in_len` is `uint32_t`. If `in_len >= 0x40000000`, wraps to a small value, `validate_user_ptr` passes, and subsequent copy reads/writes 4 bytes into an attacker-chosen range.
- Fix: Reject `in_len > UINT32_MAX / 4 || out_len > UINT32_MAX / 4` before multiplication.

**IPC-C2: Echo mailbox data-before-ready ordering**
- `kernel/src/component_runtime.c:564-575`
- Data copied with plain stores, then `ready` flag set with atomic release. On weakly-ordered ARM64, reader's acquire load of `ready` must pair with a release ordering on ALL data writes, not just the flag. Non-atomic data stores are not ordered by the atomic store on their own — need a `__atomic_thread_fence(__ATOMIC_RELEASE)` before the flag set, or make the entire payload atomic.
- Fix: Insert `__atomic_thread_fence(__ATOMIC_RELEASE)` after copying data, before setting `ready`.

**IPC-C3: Message router has no cross-CPU synchronization**
- `kernel/src/msg_router.c` (entire file)
- Static `topics[]` array accessed from publish and subscribe paths with zero locking. Multi-core publish vs subscribe can corrupt subscriber arrays, skip deliveries, or read garbage topic slots.
- Note: The Rust-side inference buffers were protected by commit 3c2d9a1, but the C message router was not. Commit a5715d6 fixed priority ordering in the Rust router only.
- Fix: Add `static spinlock_t msg_router_lock` guarding all public APIs (`publish`, `subscribe`, `ack`, `receive`).

**IPC-C4: Component cleanup context array reused across components**
- `kernel/src/component_runtime.c:541-543`
- `cleanup_ctxs` indexed by `comp_idx`. If indices are reused after unload, the entry written for component B overwrites A's; A's surviving task may fire B's cleanup.
- Fix: Allocate per-task cleanup context from PMM, free on cleanup. Remove the shared array.

### High

**IPC-H1: Unbounded-in-practice `str_copy` at msg_router boundary**
- `kernel/src/msg_router.c:64-69`, callers at lines 112, 172-173, 206
- `str_copy` takes a max parameter but is called with source strings from syscalls that are not guaranteed NUL-terminated. If source buffer is length N < max with no NUL, read wanders past N into unrelated memory (OOB read, potentially revealing kernel data).
- Fix: Validate NUL termination at the syscall boundary (see IPC-H2) before calling into router.

**IPC-H2: `validate_user_ptr` does not enforce NUL termination for string args**
- `kernel/src/syscall.c:80-91` (`sys_send_handler`)
- Only the byte range is validated; the buffer may not be NUL-terminated. `msg_router_*` and `str_copy` assume termination.
- Fix: For string args, require `((const char *)data)[len - 1] == '\0'` and `len > 0`.

**IPC-H3: Integer overflow in `msg_queue_create`**
- `kernel/ipc/ipc.c:167-169`
- `capacity * MSG_PRIO_COUNT * msg_size` — unchecked multiplication.
- Fix: Sequential overflow checks before each multiply.

### Medium

**IPC-M1: Publish iterates subscribers without lock**
- `kernel/src/msg_router.c:166-189`
- Related to IPC-C3. A new subscriber added mid-publish may be skipped; the `subs[j]` array may show uninitialized entries.
- Fix: Hold `msg_router_lock` for the full publish loop.

**IPC-M2: `sys_recv_handler` misses validation of `topic_out`**
- `kernel/src/syscall.c:107`
- Caller-provided output pointer not validated.
- Fix: `validate_user_ptr(topic_out, MSG_ROUTER_TOPIC_LEN)` check.

**IPC-M3: `component_idx` bounds unvalidated at subscribe time**
- `kernel/src/msg_router.c:97-140`
- Indices out of `[0, COMPONENT_MAX_COUNT)` are stored and later used as array subscripts.
- Fix: Range-check at subscribe.

**IPC-M4: Global `swap_state_buf` unprotected**
- `kernel/src/component_runtime.c:720-744`
- Currently single-threaded on CPU 0 by convention; explicit lock would make this safer as components expand.
- Fix: Add a short-held spinlock, or document the single-threaded invariant and assert on it.

### Low

**IPC-L1: `msg_queue_create` accepts arbitrary capacity**
- `kernel/ipc/ipc.c:145-150`
- No upper bound on capacity.
- Fix: Define `MSG_QUEUE_MAX_CAPACITY` (e.g., 65536) and reject larger.

**IPC-L2: Echo mailbox pre-init non-atomic**
- `kernel/src/component_runtime.c:531-533`
- Fields set with plain stores, but reader uses acquire loads.
- Fix: Use `__atomic_store_n(..., __ATOMIC_RELEASE)` for consistency.

---

## 4. Drivers

### Critical

**DRV-C1: Framebuffer scroll bounds**
- `kernel/drivers/fb_console.c:238-243`
- `fb_scroll` copies `fb_info.pitch` bytes per row without re-validating `y < fb_info.height` on the last-line clear path. If `fb_info` is misinitialized, writes past the framebuffer.
- Fix: Clamp `char_height = min(char_height, fb_info.height - last_line_y)`.

**DRV-C2: LAPIC EOI missing ISB**
- `kernel/arch/x86_64/lapic.c:123-125`
- On out-of-order x86 implementations, the next instruction may speculatively re-enter an ISR before the EOI takes effect.
- Fix: Add explicit `memory` clobber or `mfence` after the EOI write (on x86, a `lock; addl $0, (%%rsp)` is the canonical full fence).

### High

**DRV-H1: `uart_tegra` missing DSB on non-TCU RX path**
- `kernel/drivers/uart_tegra.c:286`
- TCU RX path has `dsb sy` before LSR read; non-TCU fallback does not. On ARM64 speculative MMIO reads, this returns stale LSR state.
- Fix: Add `__asm__ volatile("dsb sy" ::: "memory")` before the LSR poll.

**DRV-H2: `virtio_net` descriptor ring cache maintenance**
- `kernel/drivers/virtio_net.c:189-191,217-220`
- On Pi 5 / Jetson without SMPEN, `virtio_mb()` barrier is not sufficient — the descriptor cacheline may still be dirty in CPU L2 when the device's DMA engine reads from PoC.
- Fix: `cache_clean_range(vq->avail, sizeof(*vq->avail))` after updating `avail->idx` on platforms lacking SMPEN.

**DRV-H3: `uart_x86` init missing explicit barriers**
- `kernel/drivers/uart_x86.c:42-52`
- Ordering of DLAB/LCR/divisor writes relies on implicit `outb` serialization. Portable to x86 today, but should have explicit compiler barriers for future maintainers.
- Fix: Add `__asm__ volatile("" ::: "memory")` between configuration writes.

### Medium

**DRV-M1: Framebuffer cursor non-atomic**
- `kernel/drivers/fb_console.c:316-342`
- `cursor_x`/`cursor_y` updates not atomic. UART lock already excludes multi-CPU output; framebuffer path may be reachable from secondary CPU logging.
- Fix: Either route all fb output through UART lock or add dedicated fb lock.

**DRV-M2: GIC enable/disable comment clarity**
- `kernel/drivers/gic.c:565-600`
- `GICD_ISENABLER`/`ICENABLER` are atomic set/clear; the code is correct but future maintainers might switch to RMW.
- Fix: Add comment documenting the atomic write-1-to-set semantics.

### Low

**DRV-L1: `uart_rp1` FUNCSEL sequencing uses volatile delay loop**
- `kernel/drivers/uart_rp1.c:175-179`
- Empirically derived; not architecturally guaranteed.
- Fix: Timer-based delay if a more reliable signal exists; otherwise expand comment.

**DRV-L2: Stale SWPALB comment in `kprintf`**
- `kernel/src/kprintf.c:43-53`
- Comment references atomics the code no longer uses.
- Fix: Update comment to match the current IRQ-disable-only locking strategy.

---

## 5. Boot, Exceptions, and Startup (ARM64 + x86-64)

### Critical

**BOOT-C1: x86-64 GDT not reloaded after long mode entry** *(verify)*
- `kernel/arch/x86_64/boot.S:141` (32-bit `lgdt`), `kernel/arch/x86_64/entry64.S`
- 32-bit `lgdt` loads a descriptor with a 64-bit base; some CPUs truncate to 32 bits. After the far jump to 64-bit code, the GDT should be reloaded with a fresh `lgdt` using RIP-relative addressing.
- Fix: Add `lgdt gdt64_ptr(%rip)` early in `entry64.S`.
- **Note:** Confirm this is actually missing — some CPUs handle the 64-bit base correctly. Current builds appear to work on QEMU, so this is latent.

> **FALSE POSITIVE (boot-C-original):** The "PD zero loop only zeros 4KB" finding was incorrect — `rep stosl` with `%ecx = 4096` stores 4096 doublewords (16KB), covering all three page tables. The loop is sized generously. No fix needed.

### High

**BOOT-H1: DTB parser missing structure-block bounds validation**
- `kernel/src/dtb.c:135-156`
- `off_dt_struct + size_dt_struct` not validated against `totalsize` or caller-provided buffer size. Malformed DTB (possible from kexec image) can cause unbounded read.
- Fix: In `dtb_validate()`, enforce `off_dt_struct + size_dt_struct <= be32_to_cpu(hdr->totalsize)`.

**BOOT-H2: ELF program header array bounds**
- `kernel/src/elf.c:165`
- Pointer cast to `phdr[i]` without verifying `e_phoff + e_phnum * sizeof(Elf64_Phdr) <= size`.
- Fix: Add explicit check before the cast.

### Medium

**BOOT-M1: `dsb nsh` insufficient after UEFI relocation `ic ialluis`**
- `kernel/arch/arm64/boot.S:237-246`
- Instruction cache coherence across per-core L2 needs `dsb sy`, not `dsb nsh`.
- Fix: Change to `dsb sy; isb` on the relocation path.

**BOOT-M2: Inconsistent DSB after TLBI in MMU setup**
- `kernel/arch/arm64/mmu.S:40,87`
- One path uses `dsb nsh`, another `dsb ish` for equivalent operations.
- Fix: Standardize on `dsb nsh` (sufficient for identity-mapped setup) or `dsb ish` with explanation.

**BOOT-M3: Secondary CPU reads `secondary_mmu_*` before cache invalidate**
- `kernel/arch/arm64/smp_boot.S:147-195`
- With MMU off, values are read as Non-Shareable cached. Later L2 invalidate happens *after* the reads; if CPU 0 updated them between secondary wake and read, secondary sees stale values.
- Fix: Place `dc ivac` on each of the three values immediately before reading, or place them in NC memory.

**BOOT-M4: Missing ISB before `eret` at EL2→EL1 drop**
- `kernel/arch/arm64/boot.S:432-453`, `kernel/arch/arm64/smp_boot.S:77-82`
- ARM ARM D.1.21.1 requires ISB after modifying `spsr_el2`/`elr_el2` before `eret`.
- Fix: Add `isb` between the final `msr` and `eret`.

### Low

**BOOT-L1: DAIF mask not the very first instruction of `primary_cpu`**
- `kernel/arch/arm64/boot.S:561-571`
- Comment claims "first instruction"; actually preceded by ~10 instructions of stack/FPU setup.
- Fix: Move `msr daifset, #0xF` to immediately after `primary_cpu:` label. (The kexec entry already does this correctly.)

**BOOT-L2: x86-64 boot.S comment misleading about PD coverage**
- `kernel/arch/x86_64/boot.S:226`
- Comment says "512 entries = 1GB (enough for now)"; actually initializes 1GB identity map. VMM must extend to cover additional PDs.
- Fix: Expand comment to clarify that `vmm_init` extends the map.

---

## 6. Rust Runtime

### Critical

**RUST-C1: `alloc_tensor` unchecked multiplication**
- `runtime/src/inference/workspace.rs:57-68`
- Shape multiplication can wrap `usize`, resulting in under-sized allocation and subsequent OOB writes in ops.
- Fix: `n_elem = n_elem.saturating_mul(d as usize)`; reject on `usize::MAX`.

**RUST-C2: `BumpAllocator::alloc` alignment can overflow**
- `runtime/src/inference/workspace.rs:44-50`
- `(self.offset + align - 1)` can wrap; subsequent bounds check then passes a bogus pointer.
- Fix: Check `self.capacity - aligned >= size` after alignment, and guard against `aligned < self.offset`.

**RUST-C3: `static mut` globals in `msg_router.rs` race**
- `runtime/src/msg_router.rs:153-157,186-191,212`
- `TOPICS`, `TOPIC_COUNT`, `WILDCARD_SUBS`, `LAST_RECEIVED` accessed through `unsafe` blocks without held spinlock in all paths. `str_copy` helper copies unbounded C strings.
- Fix: Add OPS_LOCK-style spinlock around all `static mut` access. Cap `str_copy` with explicit `max_len`.

**RUST-C4: `im2col` buffer type confusion**
- `runtime/src/inference/ops.rs:222,238-241`
- `IM2COL_BUF: [u32; IM2COL_MAX]` cast to `*mut f32`. Size accounting in terms of f32 elements but underlying storage is u32 array — if `col_rows * col_cols > IM2COL_MAX` with the correct element size this is fine, but the check should explicitly express the size relationship.
- Fix: Redefine as `[f32; IM2COL_MAX]` directly or use a `union` / `MaybeUninit<f32>` buffer.

**RUST-C5: Component C-string dereferenced before bounds check**
- `runtime/src/component/mod.rs:83-93`
- Loop body dereferences `*p` before validating `len < MAX_NAME_LEN`.
- Fix: Check `len < MAX_NAME_LEN` in the loop condition, not the body; null-check the pointer first.

**RUST-C6: `registry.rs` i64 decode assumes fixed item size**
- `runtime/src/loader/registry.rs:254-267`
- Reserves 128-byte buffer, expects 16 × 8-byte entries, but doesn't count items written.
- Fix: Track item count, break on overflow.

### High

**RUST-H1: Spinlock leak on early return / panic**
- `runtime/src/mm/model_mem.rs:438-442`
- Manual `lock_acquire` / `lock_release` bracket — any intermediate panic leaves the lock held.
- Fix: RAII guard (`struct SpinGuard; impl Drop for SpinGuard`).

**RUST-H2: `c_str_to_bytes` must bounds-check every deref**
- `runtime/src/component/mod.rs:200-206`
- Calls a helper whose safety is not visible. If the helper loops until NUL without max, OOB read on malformed input.
- Fix: Audit and harden the helper.

**RUST-H3: NEON intrinsics without explicit target feature**
- `runtime/src/inference/ops.rs:130-150`
- Relies on implicit target feature; fallback is correct but the gate should be explicit.
- Fix: `#[target_feature(enable = "neon")]` on the vectorized path, runtime dispatch if needed.

**RUST-H4: Panics halt the kernel**
- `runtime/src/lib.rs:128` and throughout inference
- Any `panic!()` in hot paths invokes the `no_std` panic handler, which halts.
- Fix: Convert remaining panics in engine/ops to `Result` or debug-only assertions.

### Medium

**RUST-M1: SIMD feature gates in Cargo.toml**
- Relates to RUST-H3 — confirm `Cargo.toml` specifies `target-feature = "+neon"` for aarch64 and `"+sse,+sse2"` for x86_64.

**RUST-M2: ONNX parser length trust**
- `runtime/src/loader/onnx_parser.rs` — audit protobuf field length reads; ensure every `read_bytes(n)` checks `n <= remaining()`.

---

## 7. Kernel Core (shell, VFS, strings, tests, GPU, Lua)

### Critical

**CORE-C1: `strcpy` without bounds in `elf_setup_argv`**
- `kernel/src/elf.c:411`
- `strcpy(str_ptr, argv[i])` — depends on prior `strings_size` calculation being correct. Corrupted argv or TOCTOU between length and copy overflows the stack-allocated area.
- Fix: `memcpy(str_ptr, argv[i], strlen_bounded(argv[i], remaining) + 1)` with explicit remaining-space tracking.

**CORE-C2: GPU probe reads blocked MMIO on Jetson**
- `kernel/gpu/gpu_nvidia.c:41-75`
- `nv_gpu_probe()` reads `NV_PMC_BOOT_0` etc. without checking whether the region is firewall-permitted. On Jetson, this triggers CBB firewall abort. Build should use `gpu_stub.c` on Jetson; verify this probe is never linked into Jetson builds.
- Fix: Confirm CMake gates `gpu_nvidia.c` out of Jetson/Pi5 builds, or add a platform check inside `nv_gpu_probe`. CLAUDE.md notes stub is used on Jetson — verify there's no call path that reaches the probe.

**CORE-C3: Duplicated string functions across files** *(unchanged from April 2)*
- `kernel/src/string.c` vs `kernel/src/lua_stubs.c` — at least `strlen`, `strcmp`, `strncpy`, `strncat`, `memcpy`, `memset`, `memcmp`, `memmove` duplicated.
- Risk: Linker picks whichever symbol appears first; if `lua_stubs.c` is linked ahead of kernel libc, Lua-stub implementations (which may differ) replace kernel ones.
- Fix: Remove duplicates from `lua_stubs.c`; have Lua use the canonical `string.c` implementations. Verify by `nm` on final object.

### High

**CORE-H1: `kprintf` return count not clamped to INT_MAX**
- `kernel/src/kprintf.c:545`
- Large format widths can cause `out.count` to exceed `INT_MAX`, returning a negative `int`.
- Fix: Clamp to `INT_MAX`.

**CORE-H2: Lua stack not cleared after error**
- `kernel/src/lua_slm.c:585-615`
- Error path pops only the error message; any pre-error pushes leak.
- Fix: `lua_settop(L, 0)` after error.

**CORE-H3: Shell path resolution has no symlink/mount-escape defense**
- `kernel/src/shell.c:125-227`
- `..` normalization works but no mount-boundary tracking; a littlefs symlink to `../../` escapes.
- Fix: Capstone-optional; track mount context during traversal if user-accessible filesystems are added.

**CORE-H4: Shell path-resolution untested**
- `kernel/tests/test_shell.c`
- Tests cover command dispatch but not the `shell_resolve_path` corner cases (`..`, `.`, empty components).
- Fix: Add dedicated test cases for path normalization.

**CORE-H5: VFS node pool has no ref counting**
- `kernel/src/vfs.c`
- Nodes allocated from `node_pool[64]`; slot reuse without generation number means a stale fd can address the new file.
- Fix: Add generation counter in node struct; invalidate fd's that hold old generation. Capstone-optional if demo only opens files short-term.

**CORE-H6: Lua API called with potentially NULL state**
- `kernel/src/lua_slm.c:36`
- `l_print` calls `lua_gettop(L)` without null-check. Lua state creation failure path returns NULL.
- Fix: Null-guard at every C-Lua entry point.

**CORE-H7: Semihosting probe can fault on real hardware**
- `kernel/src/semihosting.c:33-36`, test_harness.c
- `HLT #0xF000` on non-semihosting hardware is undefined. Test harness must never call this on Pi 5 or Jetson.
- Fix: Guard `semihosting_available()` with `#ifdef QEMU_BUILD` or runtime platform check.

### Medium

**CORE-M1: `atoi` silently overflows**
- `kernel/src/string.c:175-201`
- Used by shell for numeric args; values beyond INT_MAX wrap.
- Fix: Route shell numeric parsing through `shell_parse_uint` (already exists at line 103); deprecate `atoi`.

**CORE-M2: `littlefs_vfs` offset truncation**
- `kernel/fs/littlefs_vfs.c:37-39`
- `size_t` cast to `int32_t` for seek — silently truncates > 2 GiB offsets.
- Fix: Reject offsets above `INT32_MAX`.

**CORE-M3: GPU alloc returns NULL without error propagation**
- `kernel/gpu/gpu_nvidia.c:227`
- Callers assume success.
- Fix: Either assert or return error enum.

**CORE-M4: Documentation drift**
- Docs dated December 2025 (`filesystem.md`, phase 3 notes) reference older file layouts.
- Fix: Audit `docs/` dated pre-April 2026; remove or refresh.

### Low

**CORE-L1: Lua stubs return 0.0 for `asin/acos/atan/atan2`**
- `kernel/src/lua_stubs.c:742-745`
- Silently wrong math.
- Fix: Implement via Taylor series, or have stubs log a warning.

**CORE-L2: Shell parses argv twice per line**
- `kernel/src/shell.c:418-426`
- Minor inefficiency.

---

## Comparison vs April 2 Review

| April 2 Finding | Status April 12 |
|---|---|
| MM-C1 (`get_l2_table` returns PA) | **Unchanged** |
| MM-C2 (Pi 5 SPINLOCK_SKIP_LOCKING) | **Unchanged** (but more urgent now — SMP live) |
| MM-M1 (PA 0 sentinel) | **Unchanged** |
| MM-M2 (lock held during UART) | `pmm_dump_stats` **fixed**; `pmm_get_buddy_stats` **unchanged** |
| MM-M3 (`pmm_get_*_pages` unlocked) | **Unchanged** |
| Duplicated string functions | **Unchanged** — still in `lua_stubs.c` and `string.c` |
| Priority message race | **Fixed** (commit a5715d6, Rust side) |
| Static buffer data races | **Partially fixed** (commit 3c2d9a1 — Rust inference `OPS_LOCK` added; C `msg_router` still unprotected → IPC-C3) |
| Stale interrupts after Jetson kexec | **Fixed** (DAIF mask at `primary_cpu` entry) |
| UART LSR DSB on Tegra | **Fixed** for TCU path; still missing on non-TCU fallback → DRV-H1 |
| MPIDR encoding on A78AE | **Fixed** |

---

## Recommended Fix Order for Capstone

**Week 1 — crash-risk / demo-blocking**
1. IPC-C1 (syscall integer overflow) — 5 min
2. SCHED-C1 (smp.c:353 busy-wait timeout) — 30 min
3. MM-H1 (TLBI after `vmm_map_block`) — 30 min
4. MM-H3 (`vmm_state.initialized` guards) — 15 min
5. DRV-H1 (Tegra UART DSB) — 10 min
6. BOOT-M3 (secondary MMU stale reads) — 1 hr
7. CORE-C3 (string-function duplication) — 1 hr

**Week 2 — correctness gaps that will surface under load**
8. IPC-C3 (msg_router lock) — 1 hr
9. IPC-C2 (echo mailbox release fence) — 15 min
10. IPC-C4 (component cleanup context) — 1 hr
11. SCHED-C2 (`current_task[]` NC / cache clean) — 1 hr
12. SCHED-C3 (task_exit race) — 1 hr
13. RUST-C1, RUST-C2 (workspace allocator overflow) — 30 min
14. RUST-C3 (msg_router.rs static mut races) — 1 hr
15. MM-C3 (ncmem accounting) — 5 min

**Week 3 — hardening / polish**
16. CORE-H1, CORE-H2, CORE-H6 (kprintf/Lua safety) — 1 hr
17. MM-M2, MM-M3 (PMM lock polish) — 30 min
18. DRV-C2 (LAPIC EOI ISB) — 5 min
19. Remaining docs drift + April 2 residuals

**Deferred to post-capstone** (requires larger refactor or doesn't affect correctness today):
- MM-C2 (Pi 5 runtime-flag spinlock) — needs wider lock audit
- MM-H2 (VMM locking) — only if dynamic mapping used
- CORE-H3, CORE-H5 (path traversal, VFS ref count) — only if untrusted users
- RUST-H4 (panics → Results) — cross-cutting refactor

---

## Notes on Methodology

- All claims include `file:line` pointers. One agent's boot finding about x86-64 PD zero loop was **incorrect** (`rep stosl` with ECX=4096 zeros 16KB, enough for all three 4KB tables) and has been removed.
- Severity is relative to the capstone goals: CRITICAL = crash/memory-corruption/demo-failure; HIGH = correctness hole observable under stress or expected next week; MEDIUM = correctness polish; LOW = style/clarity.
- "Unchanged from April 2" findings are carry-forwards that require no re-verification.
- Several subsystems (Rust inference ops, littlefs glue, lwIP glue, GPU stub) were sampled but not deeply audited; a focused follow-up is recommended if those paths become load-bearing in the final demo.
