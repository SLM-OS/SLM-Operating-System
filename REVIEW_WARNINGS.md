# Warning Findings — Full Codebase Review

128 warning findings across 8 subsystems. Generated 2026-04-26 from parallel multi-agent review of `kernel/` and `runtime/` (excluding `kernel/lib/` vendored code and `kernel/tests/`).

## Status legend

Each finding is annotated with one of the following after disposition:

- ✅ **Fixed** — defect confirmed, fix applied in this PR.
- ❌ **Not a defect** — investigated and found to be a false positive, with reasoning.
- ☐ **Pending** — confirmed valid; left for follow-up (reason given).
- ⏸️ **Deferred** — out of scope for this PR (reason given).
- 🎫 **Tracked in issue** — filed as a separate GitHub issue (number cited).

---

## kernel/src + kernel/include

### kernel/src/syscall.c
- **line 197** — `sys_log_handler` clamps to 256 bytes silently; either return the partial count or document the cap in `syscall.h`. As-is callers cannot tell their data was dropped.

### kernel/src/elf.c
- **lines 217-218** — `(memsz + PAGE_SIZE - 1) / PAGE_SIZE` can overflow if `memsz > SIZE_MAX - PAGE_SIZE`. Cap `memsz` (e.g. against a sane `ELF_MAX_SEGMENT_SIZE`) before allocation; otherwise an attacker-controlled ELF could request a 1-byte allocation and write past it.
  - ✅ **Fixed** — Added `if (memsz > SIZE_MAX - PAGE_SIZE) { elf_unload(info); return ELF_ERR_TRUNCATED; }` immediately before the round-up. The first-pass overflow guard already rejects `p_vaddr + p_memsz` wrap, but a malicious ELF with `min_vaddr = 0` could still slip a near-`SIZE_MAX` memsz into the round-up zone.

### kernel/src/vfs.c
- **line 364-365** — `vfs_init` dereferences `root_node` without NULL-check after `alloc_node()`. The pool is large (64) so this is unlikely to fail at boot, but a defensive `panic("vfs_init: out of nodes")` matches the pattern in `main.c`.
  - ✅ **Fixed** — Added `if (!root_node) panic(...)` after `alloc_node()`. Added `#include "debug.h"` for `panic`.
- **lines 35-58** — `alloc_node` is a non-atomic bump allocator with no lock. Today every caller (`vfs_init`, `vfs_create_*`, `vfs_mount`, `littlefs_mount_at`) runs on CPU 0 before `scheduler_start`, but if anything later mounts from a task context (a future hot-plug filesystem), `next_node++` races silently. Either document the "boot-only" invariant in the header or add a `pool_lock` spinlock.
  - ✅ **Fixed** — Added a doc-comment on `alloc_node` recording the "boot-only" invariant: all callers must run on the primary CPU before `scheduler_start`. A future caller from task context must add a `pool_lock` spinlock first.

### kernel/src/component_runtime.c
- **lines 121-128** — `echo_mailbox` is `volatile` plus mixed `__atomic_*` access on `ready`/`ack`. The `data[64]` field is written with plain stores (line 622) before the atomic release on `ready` — comment says the explicit thread fence handles ordering, which is correct, but the `volatile` keyword on the struct is misleading and not load-bearing.
  - ✅ **Fixed** — Removed the outer `volatile` qualifier on the struct (the inner `volatile uint32_t ready/ack` are kept as visual cues for cross-context fields). Updated the doc-comment to record that the `__ATOMIC_RELEASE` / `__ATOMIC_ACQUIRE` accesses are what carry ordering, not the `volatile` keyword.

### kernel/src/main.c
- **lines 213-219** — Pi 5 DTB scan reads from `0x2FFF0000` down to `0x2E000000` in 4 KB steps with no bounds check that those addresses are mapped. Pre-VMM init this is the firmware identity map; if the firmware ever hands off a smaller RAM window the scan faults instead of "DTB not found". Add a guard against `RAM_BASE + RAM_SIZE`.
  - ✅ **Fixed** — Clamped the scan top against `RAM_BASE + RAM_SIZE` and the scan bottom against `RAM_BASE`. A future smaller RAM window now produces "DTB not found" instead of a pre-VMM data abort.
- **lines 184-192** — Pi 5 LED blink writes to `0x107D517C04ULL` directly (no `volatile *` cast on first read of `i`, then writes the cast pointer). Extract the address into a named platform constant; literal magic addresses pre-MMU are easy to copy-paste wrong (compare with the WDT_BASE/WDT_UNLOCK use a few lines below which uses a constant).
  - ✅ **Fixed** — Hoisted the literal into a local `BCM2712_GPIO2_DATA_REG` define inside the block (kept scoped because vmm hasn't mapped this yet — the literal physical address is required pre-MMU). Comment cross-references the WDT_BASE pattern.

### kernel/src/kprintf.c
- **lines 537-541** — `uart_vsnprintf`: `if (out.pos > 0) { *out.buf = '\0'; }` writes NUL at the current write position, but the `else if (size > 0)` branch is unreachable (size==0 was already short-circuited at line 529). Dead code; either remove it or document why it's defensive.
  - ✅ **Fixed** — Simplified to `else { buf[size - 1] = '\0'; }` — `size > 0` is unconditionally true at that point because the `size == 0` early return already fired. Comment records why.

### kernel/include/spinlock.h
- **lines 79-81** — `spinlock_t` is a single `uint32_t`; on Pi 5 the lock + DC CIVAC dance assumes the lock occupies its own cacheline to avoid false sharing with adjacent data. Adjacent `spinlock_t` instances (e.g. `direct_channels[]` if it ever gets locks) share a 64-byte line. Consider `alignas(64)` for spinlocks declared in arrays.
  - ✅ **Fixed** — Added a doc-comment on the `spinlock_t` typedef recording the array-alignment convention. Existing arrays (`rq_lock`, `steal_deque_lock` in `kernel/sched/sched.c`) already use `__attribute__((aligned(CACHE_LINE_SIZE)))`. Doc-comment makes the convention discoverable for new array sites.

### kernel/src/tcp_telemetry_server.c
- **lines 392-410** — `session_feed_input` writes the `'\0'` terminator at `min(s->rx_len, RX_LINE_MAX-1)` only when `\n` is seen. If the last byte before `\n` was a `\r` and `rx_len == RX_LINE_MAX-1`, the NUL overwrites that `\r` and the CR-strip below (line 350) sees nothing. Minor; results in a slightly truncated command echo.
  - ❌ **Not a defect** — Re-traced the boundary case. `rx_len + 1 >= RX_LINE_MAX` triggers `rx_overflow = true` and drops the byte, so `rx_len` never exceeds `RX_LINE_MAX - 1`. When `\n` arrives with `rx_len == RX_LINE_MAX - 1`, the NUL goes to index `RX_LINE_MAX - 1` (one past the last data byte at `RX_LINE_MAX - 2`), so the `\r` at `rx_len - 1 = RX_LINE_MAX - 2` is preserved and the CR-strip works correctly. If overflow did fire (e.g. `RX_LINE_MAX-1` chars followed by `\r` then `\n`), `session_handle_command` returns early on `rx_overflow` without running the CR-strip path — but the buffer was already truncated upstream, so the silent loss of `\r` is a non-issue at that point. No code change.

---

## kernel/arch (arm64 + x86_64)

### kernel/arch/arm64/smp_boot.S
- **lines 99-130** — `dc ivac` on `secondary_mmu_*` invalidates only the cache line at the symbol's PC-relative literal pool entry, *not* the cache line containing the global the literal points to. The intent in the comment ("invalidate the cache line covering each global") is not what the code does — `ldr x0, =secondary_mmu_ttbr` resolves x0 to the *address of* the global; `dc ivac, x0` then invalidates the global's line. That part is correct, but doing it before reading the literal pool itself doesn't help — the literal pool is in `.text`, and IC is what matters there. Acceptable, but the comment is misleading.
  - ❌ **Not a defect** — Re-read the comment in the source. It explicitly says "invalidate the cache line covering each global before the load so the reissued access hits whatever has actually been written back to DRAM" — and the code does exactly that. `ldr x0, =secondary_mmu_ttbr` materialises the *address* of the global into x0, then `dc ivac, x0` invalidates the cacheline holding the global itself (not the literal-pool entry). The reviewer's "what the code does" parenthetical actually agrees with the comment ("dc ivac, x0 then invalidates the global's line. That part is correct"). The IC concern is irrelevant — invalidating the global's line, not the literal pool, is exactly what cross-CPU coherency requires here. No code change.

### kernel/arch/arm64/boot.S
- **lines 220-222** — EFI signature probe dereferences `x1` blindly. If kexec hands you `x1 = some non-NULL non-EFI pointer` (e.g. a stale value left by Linux), `ldr x11, [x1]` faults before VBAR is installed. The `cbz x1, .Lnot_efi` at line 219 only catches NULL. Concrete fix: range-check x1 against the kernel's own load address window before the load, or accept that this is a known kexec-vs-EFI handshake risk and mask SError just before the load (DAIF.A is set on entry so a sync fault here would still be fatal, but at least won't be confused with an SError storm).
  - ✅ **Fixed (doc)** — Accepted the kexec-vs-EFI handshake risk and documented it inline. A range-check against the load window is brittle (the kernel doesn't know its load extent here without an extra `adr` + literal pool fetch) and would still leave the same fatal-fault behaviour. The contract is now: kexec callers MUST pass `x1 = 0` if they don't intend the EFI signature handshake. UEFI launches always pass a valid EFI_SYSTEM_TABLE.
- **lines 410-426** — Pi 5 GIC GICD/GICC writes from EL2 happen unconditionally even when entering at EL1. The `b.ne .Lpi5_in_el1` at line 320 skips the EL2 block when `CurrentEL != 8`, so this is actually guarded, but the surrounding code has a `cmp x10, #8` test that depends on x10 being CurrentEL. If a future edit moves any instruction between the `mrs x10, CurrentEL` (line 293) and the `cmp` (line 319), the gate breaks silently. Add an `isb` and a refreshed `mrs` immediately before the `cmp`, or use a fresh register.
  - ✅ **Fixed** — Added a refreshed `mrs x10, CurrentEL` immediately before the `cmp x10, #8` gate. Two-instruction cost; makes the gate robust to any future edits (additional diag blocks, EL2 setup, etc.) between the original `mrs` and the test.

### kernel/arch/arm64/vectors.S
- **lines 200-215, 226-241, 263-292** — `el1_irq`, `el1_fiq`, and the EL0 handlers don't save x18 anywhere accessible to the `bl` callee. The `save_regs` macro stores x18 at offset 144 in the trap frame, so the C handler can read it via the trap-frame pointer, but the macro does NOT respect the AAPCS rule that x18 is "platform register". On bare metal this is a non-issue (x18 is unused), but if the kernel ever links against compiler-rt or LLVM-built objects that reserve x18 as TLS, the IRQ entry corrupts it because `restore_regs` reloads from the saved slot. Acceptable today; document it.
  - ✅ **Fixed (doc)** — Added a doc-comment on the `save_regs` macro recording the AAPCS x18 contract. The current save/restore preserves x18 by happy accident — any future maintainer who hoists a kernel-side write to x18 above the stp at offset 144 would corrupt task-side x18 silently. The doc-comment makes the constraint explicit so a future port to compiler-rt-linked code will see the warning.

### kernel/arch/arm64/context.S
- **lines 64-65** — `CTX_DAIF` is at offset `0x280`, immediately after FPSR at `0x278`. The struct comment in the header says context size includes DAIF; verify `kernel/include/task.h` matches. If `struct cpu_context` ends at `0x280` (DAIF as final 8 bytes) the offsets work; if a later field is appended without updating these defines, restore-DAIF reads garbage. Add a `_Static_assert(offsetof(struct cpu_context, daif) == 0x280)` in C to pin this.
  - ✅ **Fixed** — Added `_Static_assert(offsetof(struct cpu_context, daif) == 0x280, "CTX_DAIF in arm64/context.S expects daif at offset 0x280")` in `kernel/include/task.h`. Build verified — assertion passes (offset is indeed 0x280 today).

### kernel/arch/arm64/mmu.S
- **lines 39-49, 92-94, 129-131** — Multiple TLBI/IC sequences use `dsb nsh` (Non-shareable). That's correct pre-SMP (only this core's walker matters), but the comment at line 86-90 explicitly says "we are still pre-SMP here". If `mmu_enable` is ever reused post-SMP for a different purpose (NC region remap, kexec teardown, etc.) the `dsb nsh` would not synchronize against secondaries. Document the precondition or change to `dsb ish` to be safe in either ordering.
  - ✅ **Fixed (doc)** — Added a function-level PRECONDITION block to `mmu_enable` recording that `dsb nsh` is correct only because the function runs pre-SMP from `vmm_init`. A future reuse path (NC remap, kexec teardown) must upgrade to `dsb ish` after quiescing secondaries. Chose documentation over swapping to `dsb ish` because the upgrade pessimises the pre-SMP single-core path that runs here today.

### kernel/arch/arm64/exceptions.c
- **lines 289-294** — `static volatile uint32_t timer_irq_entered` is per-translation-unit, not per-CPU. With SMP every CPU's timer ISR increments the same counter without atomics — increment is RMW so counts will be lost on contention. If this is just a debug aid, document it; otherwise use `__atomic_fetch_add` or per-CPU storage.
  - ✅ **Fixed** — Switched to `__atomic_fetch_add(&timer_irq_entered, 1, __ATOMIC_RELAXED)` and documented the counter as a diagnostic-only aggregate. Per-CPU storage is overkill — the counter exists only to confirm timer ISRs are firing, and a single global atomic is the simplest correct form.
- **lines 209-216, 257-265** — NC trace stores write to `NC_MEM_BASE + NC_MEM_SIZE - 256 + cpu * 4` with bounds check `_cpu < 4`, but `MAX_CPUS` may be 6 on Jetson. Update to `_cpu < MAX_CPUS` or use a defined `NC_TRACE_SLOTS` symbol.
  - ✅ **Fixed** — Replaced both `_cpu < 4` checks with `_cpu < MAX_CPUS`. NC trace region reserves 256 bytes (= 64 slots × 4 bytes), so MAX_CPUS = 6 on Jetson fits with room to spare.

### kernel/arch/arm64/efi_stub.c
- **lines 138-145** — `map_size += desc_size * 8` is set BEFORE the first `get_memory_map` returns `desc_size` (line 125 was the size-query call). The first call returns `EFI_BUFFER_TOO_SMALL` and *populates* `desc_size` and `map_size`. Logic is correct because UEFI does set `desc_size` even on the size-query call (it always knows its descriptor layout), but defensively check `desc_size != 0` before computing slack — some less-conforming firmwares might leave it zero on the size-only call.
  - ✅ **Fixed** — Added a defensive `if (desc_size == 0) { desc_size = sizeof(efi_memory_desc_t); }` before computing slack. UEFI conformant firmware always populates desc_size, but a bad implementation would otherwise allocate exactly the queried map_size with no churn slack and then fail under any allocator activity between the two calls.

### kernel/arch/x86_64/platform_x86.c
- **lines 188-199** — `struct ap_boot_params` has overlapping comments — `_pad` and `stack` both annotated as offset `+0x1C`. With `__attribute__((packed))` and `gdt_base` at offset 0x0A as a `uint64_t`, then `idt_limit` at 0x12 (uint16_t), `idt_base` at 0x14 (uint64_t = 8 bytes → ends at 0x1C), then `_pad` (uint32_t, 4 bytes) → ends at 0x20, then `stack` at 0x20 not 0x1C. The comments contradict each other and the C is inconsistent with `ap_trampoline.S` (which expects PARAM_STACK at +0x1C). On the asm side `PARAMS_OFF + 0x1C` is the stack — but C `boot_ap` writes `*(volatile uint64_t *)(params_base + 0x1C) = (uint64_t)stack_top` (line 311) DIRECTLY rather than through the struct, so it works by accident. The struct definition is dead/misleading. Concrete fix: drop `_pad`, document that `stack` is at +0x1C, remove the duplicate offset annotation.
  - ✅ **Fixed** — Dropped the `_pad` field so under `__packed` the struct now matches the asm offsets exactly (`stack` at +0x1C). Added a doc-comment explaining that the struct is descriptive only (boot_ap writes via direct offset casts), and pinned every field with `_Static_assert(offsetof(...))` so a future field reorder fails the build instead of silently desyncing the asm side.
- **line 332** — `uint8_t sipi_vector = AP_TRAMPOLINE_BASE >> 12` = 8. Intel SDM Vol 3A §10.6.2 requires SIPI vector to encode a page in the first 1 MB and aligned to 4 KB; 0x8000 / 4096 = 8 fits, but no bounds check anywhere. If `AP_TRAMPOLINE_BASE` is ever raised above 1 MB, the truncation to `uint8_t` silently masks it. Add a `_Static_assert(AP_TRAMPOLINE_BASE <= 0xFF000)`.
  - ✅ **Fixed** — Added `_Static_assert(AP_TRAMPOLINE_BASE <= 0xFF000UL, ...)` immediately above the `sipi_vector = AP_TRAMPOLINE_BASE >> 12` line. A future move of AP_TRAMPOLINE_BASE above 1 MB now fails the build instead of silently truncating to a `0xFF` mask.
- **lines 543-550** — `vmm_init` extends page tables for RAM above 4 GB without invalidating the CPU's TLB before reloading CR3 — actually CR3 reload at lines 557-559 does flush TLB on x86. But the writes to `pdpt[i]` and `pd_page[j]` are made through the cacheable identity map; on a multi-socket system a peer CPU could observe stale entries. Single-socket, this is fine; flag for future SMP work above 4 GB.
  - ✅ **Fixed (doc)** — Added a single-socket-assumption block above the loop explaining that the local CR3 reload only flushes THIS CPU's TLB and that a future multi-socket SMP path needs a TLB shoot-down IPI here. Cache visibility (writes through cacheable identity map) is handled by MESI; the gap is purely TLB-side.

### kernel/arch/x86_64/idt.c
- **line 263** — `gic_end_interrupt(vec)` is called for vector >= 32 *before* the handler runs. For LAPIC timer vector 48 the handler may `schedule()` and never return, so EOI-before-handler is correct; but it relies on every IRQ handler tolerating EOI-first semantics. Document this contract or move EOI to the handler tail for non-timer vectors.
  - ❌ **Not a defect** — Re-read the source. The existing comment at lines 265-267 already documents the contract: "Send EOI before handler — timer_handler may context switch via schedule() and never return here." That's exactly the contract the reviewer asked to be documented. The alternative (move EOI to tail for non-timer vectors) would either require split-path code based on vector identity or break the timer-context-switch path. Keep current code.

### kernel/arch/x86_64/lapic.c
- **lines 287-297** — `lapic_send_ipi` polls `Send Pending` (bit 12) without a bound. A wedged LAPIC (e.g., after a #MC) hangs the kernel here. Bound the poll with TSC similar to `lapic_timer_calibrate`.
  - ✅ **Fixed** — Bounded the poll on a TSC deadline (~100 ms at 3 GHz). On exit via timeout, return silently — the caller has no useful recovery path for a wedged LAPIC and adding a panic would itself try to send IPIs and recurse. 100 ms is several orders of magnitude over worst-case IPI delivery, so any timeout is a real LAPIC hang, not a transient.

### kernel/arch/x86_64/trampoline32.S
- **lines 144-146** — `cpuid` long-mode check assumes leaf `0x80000001` exists; if `cmp $0x80000001, %eax; jb halt32` succeeds but a misbehaving virtualizer returns garbage in EDX, you boot a non-LM CPU into LM and crash post-CR0.PG. Acceptable on real hardware; add a comment.
  - ✅ **Fixed (doc)** — Added a comment block explaining the pathological-VM case (returning a bogus LM bit in EDX) and that there is no further defense possible from this stage. Real hardware reports the LM bit truthfully; QEMU/KVM/Hyper-V do too.

### kernel/arch/x86_64/ap_trampoline.S
- **lines 91-94, 102-105** — AP enables CR4.PAE + OSFXSR + OSXMMEXCPT and CR0.PG before checking that the BSP's CR3 (read from PARAM_CR3) actually maps the AP's current execution range. Since BSP's PML4 identity-maps the trampoline at 0x8000 (sub-1MB is in `pd[0]`), it works, but if VMM is ever rebuilt with a non-identity low region, APs hang at the CR0.PG write. Document the identity-map dependency.
  - ✅ **Fixed (doc)** — Added a comment block above the `mov PARAM_CR3, %eax / mov %eax, %cr3` sequence explaining that long-mode entry depends on the CR3 covering the trampoline's TRAMP_BASE PC. trampoline32.S builds PD[0] with 512 × 2MB pages covering 0–1GB, which includes TRAMP_BASE. A future low-region remap that breaks this identity invariant would manifest as an AP hang at the CR0.PG write — the doc-comment now warns about that path.

### kernel/arch/x86_64/context.S
- **line 89-91** — `mov; push; popfq` to restore RFLAGS re-enables IF if the new task had IF=1. If the new task is the idle task or any task that started with IF=0 (e.g. created with interrupts disabled), restoring IF=0 here and then immediately `jmp *CTX_RIP` lands in code that expects IF=1. This is correct *if* `task_create` always initializes RFLAGS to `0x202` (IF set). Verify that task creation always sets RFLAGS.IF=1; otherwise an idle task will spin without preemption.
  - ✅ **Fixed (doc)** — Verified the contract: `kernel/sched/task.c:326` initializes `context.rflags = 0` (IF=0), but `task_entry_wrapper` does `sti` as its very first instruction before tail-calling task_entry_trampoline, so a brand-new task always reaches its first instruction with IRQs enabled. For resuming tasks, the saved rflags came from `pushfq` inside `switch_to`, called from voluntary deschedule paths that always run with IRQs enabled. Documented the full contract inline at the rflags-restore site so a future "spawn task with preconfigured rflags" path knows to set IF=1 explicitly or sti before the first yield.

---

## kernel/sched

### kernel/sched/sched.c
- **lines 1517-1535** — `coop_preempt_maybe_tick` reads `coop_last_tick_cntpct[cpu]` and writes back, but uses `dmb ish` only after publishing the initial timestamp. The `now - last < period` comparison reads two values that might be torn under interrupt nesting (timer ISR could fire mid-update if it ever lands on Pi 5/Jetson, which is the goal of #134). Add an `ISB` between `mrs CNTPCT_EL0` and the comparison if you want monotonic ordering, and use `__atomic_store_n(..., __ATOMIC_RELEASE)` for the per-CPU slot.
  - ❌ **Not a defect** — `coop_last_tick_cntpct[cpu]` is per-CPU and only the calling CPU writes its slot (the array is indexed by `cpu_id()`). The 64-byte alignment in the declaration prevents false sharing with neighbours. Interrupt nesting is not a concern in the COOP_PREEMPT regime — the whole reason this function exists is that timer IRQs are not delivered to EL1 on Pi 5/Jetson. If #134 ever restores hardware IRQ delivery, the caller of this path (`schedule`) runs with `preempt_disabled[cpu]=1`, and the timer ISR's only effect on this slot is via `scheduler_tick` → `schedule` re-entry, which `preempt_disabled` blocks. The existing `dmb ish` after the timestamp update is the right barrier for cross-CPU reads of `pit_ticks` / `timer_handler_count`. No code change.
- **lines 1411-1413** — `sched_try_steal` early-exits when `this_cpu` is isolated, but the increment `sched_diag_steal_attempts[this_cpu]++` already ran above. That's fine but the comment says "we just choose not to probe victims" — the function also doesn't increment `empty_victim`. Consider a separate counter `steal_isolated_skip` so diagnostics can distinguish "isolated CPU intentionally didn't steal" from "no victims had work".
  - ⏸️ **Deferred** — Pure diagnostics enhancement, not a behavioural defect. Adding a new `sched_diag_steal_isolated_skip` array touches the diag emit path, the shell `cpu` dump format, and any test that scrapes the existing per-CPU counters. Not justified in a warnings-batch PR. File as a separate enhancement when the existing counter readings prove ambiguous in practice.
- **lines 1675-1677** — `sched_rebalance_tick` removes a candidate from `busy_cpu`'s queue under its lock, then **outside** the lock sets `candidate->state = TASK_READY; candidate->assigned_cpu = idle_cpu;` and calls `scheduler_add_task_to_cpu`. A concurrent thief could try to steal `candidate` from its (now-empty) deque entry between the lock release and the re-add. The deque entry from when `candidate` was pushed still exists; thief acquires `rq_lock[busy_cpu]`, fails the `assigned_cpu == victim` check, treats as stale. Safe — but document this in the comment block.
  - ✅ **Fixed (doc)** — Documented the safety reasoning inline at the post-unlock site: a thief popping the candidate's stale deque entry after the unlock will hit `remove_from_cpu_queue_locked == 0` (we already removed it) and discard as stale. The window before `assigned_cpu = idle_cpu` is closed by the queue-membership re-check, not by the `assigned_cpu` field check.

### kernel/sched/steal_deque.c
- **lines 43, 102** — `bottom - top` arithmetic on `uint32_t` is sound only as long as the **logical** `bottom >= top`. The contract says external lock around every call so concurrent push/steal can't briefly invert that; OK in practice. However after roughly 2^32 push-steal cycles the indices wrap and `(bottom - top) >= STEAL_DEQUE_CAPACITY` may falsely trigger or pass on the wrap boundary. At realistic rates this is decades, but document the wrap behavior or widen to `uint64_t`.
  - ✅ **Fixed (doc)** — Documented the wrap behaviour at the `bottom - top` site in `steal_deque_push`. Unsigned subtraction wraps cleanly modulo 2^32; the `>=` capacity check is a small bound so wrap is benign. Decades to reach 2^32 push cycles per deque at realistic rates. Doc-comment recorded so a future maintainer who widens to int64 doesn't think the original code was broken.
- **lines 67-78** — `steal_deque_pop` decrements `bottom` first, then NULL-checks the slot. If every slot in the live range is stale (NULL'd by `steal_deque_remove`), the loop drains the whole range — bounded by capacity, fine. But each iteration emits `d->bottom--` writes to NC memory — verify the Pi 5/Jetson cost is acceptable on the hot path (called from `schedule()` indirectly). Add a benchmark or short-circuit fast-path when first slot is non-NULL.
  - ⏸️ **Deferred** — Performance concern, not a defect. The loop is bounded by `STEAL_DEQUE_CAPACITY` (typically 16-32) and only takes the slow path when consecutive entries are stale, which only happens under heavy concurrent removal. Adding a benchmark + fast-path is its own performance project. The existing single-CPU bound makes the worst case acceptable for the current workload.

### kernel/sched/task.c
- **lines 568-628 (task_destroy)** — runs entirely under `task_lock`, and after the slot zero/`task->generation++` releases the lock, calls `cleanup(cleanup_arg)` and then `pmm_free_pages` (line 619) at line 620. The cleanup callback could itself reschedule, but that's fine outside the lock. However, the `task->generation++` at line 608 is done with `task_lock` held and *not* cache_clean'd on non-NC platforms — the bumped generation may be stale to a thief on another CPU, defeating ABA detection. Add `cache_clean(&task->generation)` on non-NC builds, mirroring the pattern used elsewhere.
  - ✅ **Fixed** — Added `#if !defined(PLATFORM_HAS_NC_MEMORY) cache_clean(&task->generation); #endif` immediately after the increment. NC platforms (Pi 5, Jetson) bypass cache so the write is visible without maintenance; non-NC platforms now flush to PoC so a thief on another CPU validating against the bumped generation reads the live value, preserving the ABA-detection guarantee.
- **lines 76-86** — `current_task` is allocated from NC memory, but the fallback `current_task_fallback` is BSS. When the fallback is used (NC exhausted), the `task_set_current` code path elides `cache_clean` (because `PLATFORM_HAS_NC_MEMORY` is defined, even though we fell back to cacheable BSS). The WARN at line 82 acknowledges this, but it's a silent correctness bug, not just degraded coherency: cross-CPU `task_current()` reads will see stale data. Either panic on fallback, or make the fallback path use `cache_clean`/`cache_invalidate` despite the `PLATFORM_HAS_NC_MEMORY` define.
  - ✅ **Fixed** — Converted the WARN to `panic("current_task: NC arena exhausted — bump NC_MEM_SIZE (platform header)")`. The runtime-flag alternative would require threading a `current_task_in_fallback` boolean through every `task_set_current`/`task_current_on_cpu` call, complicating the hot path. NC exhaustion at boot is an operator-fixable problem (raise NC_MEM_SIZE in the platform header). Removed the now-unused `current_task_fallback[MAX_CPUS]` BSS array.

### kernel/sched/ai/sched_ai.c
- **lines 516-527 (quantize_fp32_to_int8)** — if `src[i]` is NaN (possible if `ai_extract_state`'s `Newton's-iteration sqrt` produces a degenerate result), the cast `(int32_t)(q + 0.5f)` is undefined per C99/C11 (out-of-range float→int). Add an `if (q != q)` (NaN test) → clamp to 0 before the cast. Same risk if `state[i]` is ±inf.
  - ✅ **Fixed** — Added a non-finite check (`v != v || (v - v) != 0.0f`) that catches both NaN and ±Inf and substitutes the zero-point `zp` (a neutral feature value the downstream argmax can't interpret as advice). Casting NaN/Inf to `int32_t` is UB per C11 §6.3.1.4; this prevents the policy path from triggering UB on any feature-extraction degeneracy.
- **lines 638-644** — `sched_ai_get_stats` discriminates policy by `policy_name[3]` without checking the string length. Caller could pass `"ai\0"` (length 2) and `policy_name[3]` reads past the NUL — out-of-bounds read. Add `strlen(policy_name) >= 4` or use `strcmp`. Same pattern at lines 679-684.
  - ✅ **Fixed** — Added length validation (`policy_name[0..3]` non-NUL) before the discriminating reads in both `sched_ai_get_stats` and `sched_ai_get_rate_stats`. A short policy name now selects no stats and falls through to the empty-output path instead of reading past the buffer.
- **lines 614-618** — `ai_hailo_assign_cpu` calls `ai_assign_cpu_common` which calls `ai_schedule_mlp_via_hailo`, which on the no-model path returns -1 → falls back to heuristic. Fine. But on the active path it makes a synchronous Hailo NPU call from inside scheduler hotpath. Worst-case Hailo inference latency must be bounded — if the Hailo interface ever blocks (e.g. waiting on FW response per the umbrella #253 work), this stalls every `scheduler_add_task` system-wide. Add a bounded timeout/escape-hatch in the inference_run path called from scheduler context.
  - ⏸️ **Deferred** — `kernel/inference/inference_device_hailo.c` already enforces a 500 ms `timeout_us` on the inference run() path (also flagged in the kernel/inference section below). The bound exists; the warning is about the bound being too coarse for scheduler hotpath. A finer-grained timeout / escape-hatch is the right long-term fix but couples this work to the umbrella #253 NPU stability investigation. Not in scope for the warnings-batch.

### kernel/sched/ai/ai_state.c
- **lines 99-103** — `f[5] = (float)rq->head->effective_priority / 7.0f` reads `rq->head` and `head->effective_priority` for **other CPUs** with no lock. On Pi 5 without SMPEN, can read torn or stale data; can also dereference a NULL/freed pointer if the head is being concurrently mutated. Acquire that CPU's rq_lock briefly, or accept the race and add a NULL guard before deref (`struct task *h = rq->head; if (h) f[5] = ...`).
  - ✅ **Fixed** — Snapshot-the-pointer pattern: `struct task *h = rq->head; if (rq->idle_task && h) f[5] = (float)h->effective_priority / 7.0f;`. The NULL/use-after-free gap is closed; `effective_priority` may still read a torn or stale value, but that's acceptable noise on an AI feature input that already tolerates approximate readings.

### kernel/sched/ai/inference_cpu.c
- **lines 84-86** — `cpu_mlp_run` does `FP_CONTEXT_SAVE/RESTORE`, but its caller (`ai_schedule_mlp_via_device`) is itself called from `ai_assign_cpu_common` which already did `FP_CONTEXT_SAVE`. Two nested saves on a 4 KB stack frame each (struct fp_state ≈ 520 B + alignment) for one inference. Either drop the inner save (the comment correctly notes the policy already saved), or document that the CPU device can be called from non-policy paths. Prefer the former.
  - ❌ **Not a defect** — The existing comment explicitly documents the design: "FP_CONTEXT_SAVE / _RESTORE is mandatory because ai_schedule_mlp is currently called from IRQ context via the scheduler policy path. The inference_device layer doesn't know which context its callers run in, so save/restore unconditionally — the same defense-in-depth approach sched_ai.c takes today." Dropping the inner save would couple inference_device to the policy path's save discipline; the redundant 520 B per call is acceptable on a 16 KB kernel stack and the inference path is not deeply nested. Existing rationale stands. No code change.

### kernel/sched/smp.c
- **lines 451-465** — `boot_secondary` polls `cpu_boot_flag[cpu]` 5000 times with 100us between checks (~500 ms). The accept-on-PSCI-success fallback at lines 470-474 returns `PSCI_SUCCESS` even when the flag never appeared, and the variable `ret` is `PSCI_SUCCESS` from line 435 (the only path that reaches here). The `if (ret == PSCI_SUCCESS)` is always true here; the fallback below is unreachable. Simplify.
  - ✅ **Fixed** — Removed the unreachable `if (ret == PSCI_SUCCESS)` test and the dead `WARN("CPU %u: boot timeout ...")` branch. The path to the fallback is unconditionally `ret == PSCI_SUCCESS` (the failure branch returns early). Inlined the WARN + cpu_data update at the original site with a comment recording why the post-fail branch was removed.

### kernel/sched/preempt.c
- **lines 121-151** — `maybe_arm_resched_trampoline` reads `tf->spsr` and writes `tf->elr` and `tf->spsr` without a barrier. On real ARM64, the trap frame is on this CPU's stack so no cross-CPU visibility issue, but document that writes to `orig_elr` / `orig_spsr` must be visible to `resched_trampoline` (asm) before the eret. Since this all happens on the same CPU, OK — but the asm path may rely on architectural ordering that should be commented.
  - ❌ **Not a defect** — All operations happen on the same CPU. The `orig_elr` / `orig_spsr` writes precede the `tf->elr = &resched_trampoline` redirect; `eret` itself is a context-synchronizing event that orders prior stores against the post-eret instruction stream. The trampoline reads `orig_elr` from cacheable per-CPU memory after the eret, which is naturally ordered by program order on this same CPU. No barrier needed; existing comment block at lines 132-148 already covers the per-CPU semantics. No code change.

### kernel/sched/sched_heuristic.c
- **lines 72, 93** — `static uint32_t rr_next` is a global without atomicity. Multiple CPUs concurrently calling `find_target_cpu` race on read-modify-write. Read/write tearing is unlikely on aligned uint32_t but not guaranteed. Use `__atomic_load_n`/`__atomic_store_n` with relaxed ordering, or accept the race and document.
  - ✅ **Fixed** — Switched both reads to `__atomic_load_n(&rr_next, __ATOMIC_RELAXED)` and the write to `__atomic_store_n(..., __ATOMIC_RELAXED)`. The "tiebreaker" semantic is approximate by design (a lost update means two CPUs pick the same starting cpu for one decision, then drift), so RELAXED ordering is sufficient — we just need to avoid torn reads / lost writes on the RMW.

### kernel/sched/task_sleep.c
- **lines 41-42** — `sleep_queue_head` is a single global with one lock. With many sleeping tasks, `task_wake_sleepers` is O(n) under the lock and called from every scheduler_tick. For MAX_TASKS=32-64 it's bounded; if MAX_TASKS grows, refactor to a sorted queue or per-CPU queue.
  - ⏸️ **Deferred** — Existing comment at line 38-40 already records the bound: "Unsorted — we walk the whole list each tick (cost is bounded by MAX_TASKS and sleeps are rare in bare-metal workloads)." Refactoring to a sorted/per-CPU queue is a meaningful design change tied to a future MAX_TASKS expansion. Not justified now.

---

## kernel/mm + kernel/ipc + kernel/fs + kernel/net + kernel/inference + kernel/usb

### kernel/mm/pmm.c
- **lines 92-93** — `MAX_BLOCKS = RAM_SIZE / PAGE_SIZE` for the `block_state[]` array. On Jetson `RAM_SIZE` is configured for ~990 MB (region 1) but `pmm_init` registers regions extending up to `0x240000000` (~9 GB). `addr_to_block_index` does an `ASSERT(idx < MAX_BLOCKS)` (line 140) — first allocation in regions 2/3 will hit the assert. Either grow `MAX_BLOCKS` to cover all registered regions or refuse to register higher regions. (Today the bug is masked because `block_state` is sized from the platform's full RAM_SIZE; double-check `platform.h` reflects 9 GB before relying on this.)
- **lines 651-714** — `pmm_alloc_pages_low` walks every free list with `pmm_lock` held and IRQs disabled. Comment acknowledges this is "per-load only", but there is no contract enforcement — a future caller in a hot path would silently stall the system. Add a `WARN_ONCE` if call frequency exceeds a budget, or rename to `pmm_alloc_pages_low_slow` to make the contract obvious.

### kernel/mm/vmm.c
- **lines 1395-1428** — `vmm_test_*` helpers compiled unconditionally (no `#ifdef CONFIG_TESTS`). They expose raw L2 entry pokes to any caller. Gate with `#ifdef ENABLE_BOOT_TESTS` (or whatever the existing test guard is) so production kernels can't accidentally call them.

### kernel/ipc/ipc.c
- **lines 106-125** — `block_on_queue` is a fragile lock-handoff API: caller does `irq_restore(flags)` and passes the still-held lock by pointer, the function does `spin_unlock(lock)` (without IRQ-restore — caller already restored), `yield()`, then `spin_lock(lock)`. After return the caller does `flags = irq_save()` to rebuild a token — but the lock is now held with IRQs *enabled* between `spin_lock(lock)` (in block_on_queue) and the caller's `irq_save()`. A timer ISR firing in this window that touches `queue->lock` deadlocks. Fix: have `block_on_queue` accept and return the saved IRQ flags so the lock+IRQ state is contiguous.
- **lines 220-233 and 657-664** — slot allocation uses `for (i = 0; i < MAX; i++)` to find a free slot, silently dropping the queue if the table is full (no error returned to the caller). The queue is leaked. Fix: return NULL if no slot found, after freeing the buffer + queue page.

### kernel/ipc/pi_mutex.c
- **lines 168-198** — selecting highest-priority waiter is O(n) with the lock held + IRQs disabled. Fine for "small queues" but unbounded if a misbehaving workload piles up. Cap waiter count or convert to a priority-indexed list.

### kernel/fs/littlefs_slm.c
- **lines 83-91** — `lfs_strcpy(... size_t max)` does `while (i < max - 1 ...)`; if `max == 0` the unsigned underflow loops `SIZE_MAX-1` times and writes past the buffer. Add `if (max == 0) return;` guard. Currently called only with `sizeof(info->name)` which is non-zero, but the bug is latent.
- **lines 351-383** — `littlefs_unmount` calls `lfs_file_close` / `lfs_dir_close` while holding `mnt->lock` with IRQs disabled. LittleFS read/prog/erase callbacks can be slow (block I/O); holding a spinlock + IRQs off across a multi-millisecond block erase blocks all timer ticks on that CPU. Use a sleeping mutex or drop the spinlock for the I/O phase.
- **lines 396-398** — `static uint8_t read_buf/prog_buf/lookahead_buf` inside `littlefs_format` are file-scope statics shared across all callers. Concurrent callers race; even sequential callers race against a still-mounted filesystem if `littlefs_format` is called against a different device while a mount uses its own buffers. Document as single-caller or move to a per-call stack/heap allocation.

### kernel/net/lwip_slm.c
- **lines 397-407** — `slm_netif_output` allocates `tx_buf[1518]` on the stack. With deep call chains (lwIP → ARP → driver) and a 16 KB kernel stack, this is borderline. Consider moving to a per-CPU static buffer (lwIP is pinned to CPU 0 per the sys_arch.c comment).
- **lines 296-347** — `net_watchdog_get` reads multiple fields without locking. The comment at 285-294 says this is intentional and consumers tolerate intermediate states. Document explicitly that `lwip_stats.memp[i]->used` reads are also racy with lwIP mutating from the same CPU's protect/unprotect sections.

### kernel/inference/inference_device.c
- **lines 21-50** — `device_count++` in `inference_device_register` is not protected by any lock. If two CPUs ever call this concurrently (unlikely today but no enforcement), they'd both write `device_count` and stomp `devices[]`. Either document "single-CPU init only" with a CPU-id assert, or add a registration spinlock.

### kernel/inference/inference_device_hailo.c
- **lines 1665-1666** — `.timeout_us = 500000` (500 ms) for inference run() called from "scheduler-policy paths that may have IRQs disabled". 500 ms with IRQs disabled is a system stall (timer ticks lost, IPI delivery delayed). The comment acknowledges the issue. Either ban IRQ-disabled callers (assert on entry) or split the polling loop with periodic `irq_restore_local`.
- **lines 92-126** — `hailo_fw_dump_log` uses a 4 KB `static uint32_t log_buf[]` (line 101). Re-entrant call from another CPU corrupts the dump; mark single-CPU or move to per-CPU.

### kernel/usb/core/usb_core.c
- **lines 33-37** — `active_hcd` declared without lock but written by `usb_core_register_hcd`. Comment claims primary-CPU-only registration before secondaries boot. Add a runtime assert (`ASSERT(get_cpu_id() == 0)`) at the head of `usb_core_register_hcd` to catch violations.
- **lines 280-303** — `usb_hub_reset_port` busy-waits on `timer_get_count()` for 500 ms with IRQs in unknown state (caller-dependent). If called from an IRQ-disabled context, this stalls the CPU. Document caller contract or yield periodically.

### kernel/usb/class/cdc_ecm.c
- **lines 458-530** — `cdc_notify_handle_event` reads `cdc.notif.urb.actual_length` and `cdc.notif.urb.status` without ACQUIRE-pairing with the HCD's RELEASE on completion. The completion callback (line 402-407) does set `completed` with RELEASE, and `cdc_notify_handle_event` (line 460) loads `completed` with ACQUIRE — so the URB fields are visible. Good. (No issue.) But `cdc.upstream_bps` / `downstream_bps` (lines 506-509) are written here without atomic but read elsewhere (lines 970-973 use RELAXED). Either make them `_Atomic` or note "snapshot is best-effort".
- **lines 354-364** — `generate_fallback_mac` uses a non-atomic static `counter`. Two CPUs concurrently probing different devices (Phase 4 hot-plug) would race on counter and could produce identical MACs. Use `__atomic_fetch_add(&counter, 1, RELAXED)`.

---

## kernel/drivers

### kernel/drivers/bpmp/hsp.c
- **lines 161-183** — `hsp_wait_bpmp_doorbell_enabled` polls every `timer_busy_wait_us(1)`; on a stuck BPMP this is `timeout_us` MMIO reads — fine. But the function is callable concurrently with `mrq_send` (which also polls the doorbell) on different CPUs without locking; the underlying state is read-only here so it's correct, but document the "init-only" expectation.

### kernel/drivers/bpmp/ivc.c
- **lines 234-235** — In `ivc_tx_commit`, `tx.count` is read fresh, +1, written back. Single-master IVC so non-atomic RMW is fine — but the read uses `ivc_read32` which is not barrier-paired with the preceding payload `dmb()`. `ivc_read32` does no DSB; the volatile read may see a stale BPMP-owned `rx.count` (via `ivc_tx_is_empty` earlier). Consider read-side `dmb(ld)` before the count read so the empty-check and the count snapshot agree.

### kernel/drivers/bpmp/mrq.c
- **lines 89-198** — `mrq_send` holds `g_mrq_lock` for up to 100 ms (`MRQ_POLL_TIMEOUT_US`) with IRQs disabled (`spin_lock_irqsave`). On a stuck BPMP this stalls *all* IRQs on every CPU that long. Consider re-enabling IRQs during the poll loop (still hold the lock) or splitting commit/poll so only the commit/consume edges are serialized.

### kernel/drivers/camrtc/camrtc.c
- **lines 605-665** — `CAMRTC_CTRL_REGION_PHYS = 0xA0000000` is a hardcoded physical address. The PMM carve-out is documented in the comment but there is no compile-time link to `pmm.c`'s carveout constant — if one moves, the other silently breaks. Define the address in `platform.h` or a shared header and `_Static_assert` agreement.
- **lines 653-656** — Volatile `uint64_t *region_words` zero-loop assumes the region is identity-mapped & cacheable. Comment at 693-699 asserts no cache maintenance needed because RCE is "coherent fabric"; verify via TRM. If RCE actually goes through SMMU bypass and reads from DRAM, you need `cache_clean_range(region_phys, CAMRTC_CTRL_REGION_BYTES)` after the TLV writes — `dsb sy` alone doesn't push dirty cachelines to DRAM.

### kernel/drivers/gpio/gpio_tegra.c
- **lines 74-101, 118-127** — No locking. Two CPUs touching the same pin's `OUTPUT_VALUE`/`ENABLE_CONFIG` will RMW-race. Pinmux writes (`gpio_tegra_pinmux_set_gpio`) likewise unprotected. Today only IMX219 bring-up uses these (single CPU) but the `tegra_i2c_bus` has a per-bus `spinlock_t`; mirror the pattern with a per-pin or controller-wide lock before adding a second consumer.

### kernel/drivers/i2c/i2c_tegra.c
- **lines 215-273** — `tegra_i2c_init` is 59 lines and holds `bus->lock` across BPMP MRQs (≤100 ms each × 3). Acceptable per comment but worth flagging if init becomes hotter.

### kernel/drivers/pcie/pcie_core.c
- **lines 233-330** — `probe_one_bar` does write-0xFFFFFFFF, read-back, restore — but the restore is the *original* `raw_lo`, which on Pi 5 RC comes up as 0. After probing, the BAR is left at 0 (effectively unmapped); the resource-allocation pass at 468-543 only programs BARs where `d->bar[b] != 0` was reset to 0 — wait, it programs where `d->bar[b] == 0`. So the path is correct, but only by accident: the probe destroys any pre-firmware-assigned address and then the bump allocator re-assigns from window. On firmware-pre-assigned topologies (x86 SeaBIOS) this would relocate BARs. On QEMU/Pi 5 it works because `raw_lo == 0` already. Document the assumption or save the original value across `restore + reallocate`.
- **line 565** — `pcie_find_class` returns first hit; sufficient for current single-device topologies but masks duplicate devices.

### kernel/drivers/pcie/pcie_tegra194.c
- **lines 164, 249-595** — `g_host_inited` and the controller's APPL/DBI/iATU MMIO are unprotected. Multi-caller would corrupt iATU programming. Single-init expectation acceptable but should be enforced (assert/return from second `pcie_tegra_host_init`).

### kernel/drivers/pcie/pcie_bcm2712.c
- **lines 267-285** — `pcie1_r32`/`pcie1_w32` use no barriers at all (unlike the Tegra side). For BCM2712 Device-nGnRnE this is mostly OK (strongly-ordered per location), but `bcm2712_phy_bringup` line 527 demonstrates the project knows the cross-region ordering issue (explicit `dsb sy`). Consider a single helper or document the convention — easy to add a new register window and forget the manual DSB.
- **lines 933-962** — `cfg_read32_locked` for bus 0/dev 0 uses `pcie1_regs + (offset & 0xFFCu)`. The mask aligns to 4 but doesn't mask `bus`/`dev` — fine because the early-return covers (0,0,0). However, `cfg_write32_locked` lacks the read-back barrier between the EXT_CFG_INDEX write and the DATA write that the read path has at 947 (`(void)pcie1_r32(PCIE1_EXT_CFG_INDEX);`). Per the comment at 940-946, back-to-back accesses can return stale data without that read-back. Apply the same readback in the write path before the DATA store.

### kernel/drivers/camera/nvcsi.c
- **lines 220-246** — No barriers in `nvcsi_phy_write`/`nvcsi_stream_write`; relies on read-back for flush. Same convention as i2c/gpio, but Tegra-MMIO speculative-read concern applies to the verify read at 356-364.

### kernel/drivers/usb/xhci/xhci.c
- **lines 1369-1379, 1404-1409** — DCBAAP/CRCR/ERSTBA written as two `w32` calls (lo then hi). Comment at 1370 says "HC latches on the high-dword write" — correct. But there's no DSB *between* lo and hi. ARM Device-nGnRE preserves order per location; these are different addresses (`+0` and `+4`), so reordering is permitted. Add `dsb(sy)` between lo and hi or use a 64-bit single-store path.
- **line 1377** — `cmd_ring_ptr = phys | 1`. `phys` must be 64-byte aligned per xHCI spec. No assert. If a future allocator returns a 32-byte-aligned ring, the `| 1` collides with address bits silently.

---

## kernel/ai_accel/hailo + kernel/gpu/nvidia

### kernel/ai_accel/hailo/hailo_cs_builder.c
- **line 22** — `hailo_cs_builder_append` returns `HAILO_ERR_INVAL` for `b == NULL` but does not validate `body_len` against any per-action max. A caller passing `body_len = SIZE_MAX` triggers the overflow above. Add a sanity ceiling (e.g. `HAILO_CS_CONTEXT_CHUNK_MAX_BYTES`).

### kernel/ai_accel/hailo/hailo_control.c
- **lines 1383-1399** — `hailo_control_set_network_group_header` truncates user-supplied `header->batch_size`, `csm_buffer_size`, `external_action_list_address` to native-LE u16/u32 fields without endian conversion (per the v4.23 contract — that part is correct), but the validator only checks `config_channels_count > HAILO_CS_MAX_CFG_CHANNELS`. `header->networks_count` and `header->dynamic_contexts_count` are copied as raw `uint8_t`/`uint16_t` from caller fields that may be wider; if the caller's struct uses larger types, silent truncation produces a wire-valid header that disagrees with what the caller intended. Add explicit range checks for these fields.
- **lines 322, 304, 305** — `control_msi_registered`, `control_irq_masks_armed`, `control_post_boot_init_done` are plain `bool` flags with the comment acknowledging they need promotion to atomic if any future caller races init. `hailo_control_register_msi_for_boot` is exposed publicly — if any code path off CPU 0 calls it before `hailo_boot` finishes, two CPUs both observe the flag false and double-register. Make the flags `_Atomic bool` or gate behind a single-acquire spinlock now while the surface is small.

### kernel/ai_accel/hailo/hailo_cs_translator.c
- **lines 411-439** — `hef_matches_mnist_template` reads `info->sdk_version[i]` against fixed `"3.33"` prefix without bounds-checking `info->sdk_version`'s length. If `sdk_version` is shorter than 4 bytes (or non-NUL-terminated), the for loop may read past the buffer. The check trusts the parser to guarantee a NUL terminator within the array — verify this contract is enforced upstream in `hef_parser`.
- **lines 1044-1052** — `translate_trigger_sequencer` narrows `cluster_index` (u32→u8), `initial_l3_cut` (u32→u8), `initial_l3_offset` (u32→u16) silently, then *afterwards* logs a WARN if the original values exceeded the narrowed range. The cast happens at struct init time before the comparison — by then truncation has occurred and the wire body already carries the wrong value. Move the range check before the struct initialization and return `HAILO_ERR_INVAL` rather than warn-and-emit.

### kernel/ai_accel/hailo/hailo_vdma.c
- **lines 415-471** — `hailo_vdma_channel_start` does `mb()` only at the very end; the four MMIO writes (ALIGNED_ADDR_L RMW, ADDR_H, BASE_DWORD depth/id, BASE_DWORD start) rely on the MMIO accesses being strongly-ordered (ARM64 Device-nGnRnE) but the read-modify-write at line 449-454 reads through `pi5_read32` which doesn't issue a barrier between read and write. On Pi 5 PCIe with a posted-write window this is fine, but call out the assumption.
- **line 421** — `if (list->iova & 0xFFFFu) return HAILO_ERR_INVAL` enforces 64 KB alignment. Good. But `list->iova` should also be bounded to 40-bit (BCM2712 PCIe inbound window) — line 446 `addr_l = list->iova >> 16 & 0xFFFFu` and line 447 `addr_h = list->iova >> 32`. If iova ≥ 2^48, addr_h truncates silently. Add a check.

### kernel/ai_accel/hailo/hailo_pi5.c
- **lines 198-204** — `pi5_shutdown` zeros `bar_map[]` but does NOT unregister the MSI handler. If shutdown is ever called with the MSI line still enabled, `hailo_msi_trampoline` fires, calls `user_irq_handler` (which will dereference `hailo_platform->read32(HAILO_BAR_CONFIG, ...)` and read through a now-NULL `bar_map[HAILO_BAR_CONFIG]` returning 0xFFFFFFFF — but the IRQ stays asserted because we never W1C). Either tear down the MSI binding in shutdown, or document the precondition.
- **lines 255-269** — `pi5_bar4_read` performs MMIO reads in a loop with no explicit `dsb` or compiler barrier between iterations. ARM64 Device-nGnRnE memory enforces program order for a single CPU, but the kernel CLAUDE.md notes Tegra needs DSB SY before LSR reads. Pi 5 may not strictly need this, but adding a leading `dsb ld` (or matching the post-`bar4_write` barrier with a leading one in `bar4_read`) keeps the contract symmetric and safe across platforms.

### kernel/gpu/nvidia/ga10b_bringup.c
- **lines 1382-1538** — `ga10b_submit_and_poll` is ~157 lines, mixing pushbuffer copy, GPFIFO entry placement, GP_PUT update, doorbell ring, polling, and bookkeeping. The 2-second polling loop comment (line 1470) is misleading: `for (uint32_t us = 0; us < 2000000; us++)` is iteration-count, not a real wall-clock cap; with cache_invalidate+busy-spin per iteration the loop can run 4-10 seconds on Jetson. Use CNTPCT for an actual time-based bound (the helper exists in `kernel/drivers/timer.c`).
- **line 988-991** — `g_handoff` is global with no lock. The file-scope comment (line 975-980) acknowledges single-channel only and serializes via shell-driven flow, but `ga10b_bringup_set_input` (line 1806) memcpys into `g_handoff.input_buf_phys` and is callable from any context. If a second caller arrives, the v6 input swap can race with `ga10b_submit_and_poll`'s read. Add a `static spinlock_t handoff_lock` now.
- **lines 1396-1397** — `volatile uint32_t *poll = (volatile uint32_t *)(uintptr_t)poll_phys; *poll = 0;` then `cache_clean(poll, sizeof(uint32_t))`. The `volatile` qualifier doesn't make the store a release barrier; the subsequent `cache_clean` followed by `mb()` (line 1401) does the right thing, but the ordering is fragile. Document that the `volatile *poll = 0` is a CPU-visible store that must be CMO'd to PoC before doorbell.
- **line 1481** — Inner busy-wait `for (volatile int i = 0; i < 1500; i++) { }` is timing-dependent and adds nothing on top of the cache-invalidate. Use `gsp_platform->udelay(1)` or its Jetson equivalent.

### kernel/gpu/nvidia/falcon.c
- **lines 51-56** — `us_to_iters(us)` converts microseconds to a fixed iteration budget assuming "10 iters/µs", but BCM2712/Tegra MMIO read latency is hardware-dependent. On a slow PCIe RC the timeout gets stretched silently. Same recommendation as ga10b: switch to CNTPCT-based wall-clock budgeting, especially for `falcon_wait_halted` whose timeout is caller-supplied.

### kernel/gpu/nvidia/bringup.c
- **lines 175-237** — `gsp_bringup_patch_dmemmapper` does bounds checks (line 180, 189, 199, 214) but `cnt` is read from device-controlled DMEM (line 185). A malicious or corrupted DMEM with `cnt = 255` and `interface_off + 4 + 255*8 = interface_off + 2044` could pass the bounds check yet have the loop iterate past the intended interface table. The bounds check covers the full extent; this is fine, but worth noting that `dmem` content is firmware-supplied and any field within is untrusted.

---

## runtime/src/mm

### runtime/src/mm/model_mem.rs
- **lines 685-796** — `evict_and_retry` releases the lock between candidate snapshot (line 689-697), eviction selection (line 725), and the actual `pool.free` (line 768). A concurrent `touch()`/`free()` between snapshot and free is handled (`StaleHandle` ignored) but the retry path is single-shot — under contention the caller can spuriously see `OutOfMemory`. Add a bounded retry (say 4 attempts) or document the single-shot semantics.
- **line 769** — `pool.evictions_total = pool.evictions_total.saturating_add(1)` is incremented even when the prior `pool.free(h)` returned `StaleHandle` (line 768 discards the result). Counter overcounts on benign races. Check the `free` result before bumping.
- **line 1109** — `snapshot_evictable_blocks` allocates a `Vec` while holding the spinlock. The `alloc::vec::Vec::push` can call into `LockedHeap`, which is a third lock domain — if the heap is contended with another CPU mid-allocation while this CPU holds the pool spinlock, latency spikes. Pre-size with `Vec::with_capacity(2 * MAX_BLOCKS_PER_POOL)` before taking the guard.
- **line 1117** — `for (pool_id, pool_ptr) in [(POOL_WEIGHT, addr_of_mut!(WEIGHT_POOL)), ...]` — creating the tuple-array allocates a small array on the stack, but the inner `&*pool_ptr` is still a transient `&MemoryPool` while another thread cannot acquire LOCK. Fine, but the unsafe block is large; split per-pool to narrow the SAFETY scope.

### runtime/src/mm/eviction/store.rs
- **line 89** — `static mut STORES: [KindStore; 3]` lacks an explicit `unsafe impl Sync`. Compiler accepts because access is via `addr_of_mut!`, but the soundness contract should be spelled out as in `model_mem.rs`.
- **lines 131-139** — `reset()` uses `&mut` indexing on the static via `(*stores)[0] = ...`. Same pattern as above — works only because the lock is held, but the `&mut` materialisation is not visible to readers.

### runtime/src/mm/eviction/registry.rs
- **lines 215-236** — `set_eviction_policy` quietly installs the caller's policy into pool 0 only and forces pool 1 to the default. The doc-comment admits this is a workaround for the unclonable `Box<dyn>`. Callers reading the function name reasonably expect "set both". Rename to `set_weight_eviction_policy` or take two `Box<dyn>` arguments.
- **lines 312-334** — `select_victim` dereferences `candidates[0].pool_type` to dispatch but the docs claim "callers pre-filter by pool". If a caller ever mixes pools, blocks of the wrong pool are scored against the wrong policy. Add a `debug_assert!(candidates.iter().all(|c| c.pool_type == pool))` guard.
- **line 81-94** — The `slm_get_time_ns` extern is also declared in `kernel_ffi.rs` and `cacheus.rs`. Triple-declared FFI signatures will silently diverge. Funnel through `kernel_ffi::get_time_ns()`.

### runtime/src/mm/eviction/cacheus.rs
- **line 113** — `1.0 / n as f32` — when `experts.is_empty()`, the `debug_assert!` on line 111 only fires in debug. In release, `n = 0` makes weights `[inf; 0]` (vec is empty so no value) — actually Vec is empty so safe, but there is no defensive path. Promote to `assert!`.
- **line 281** — `let mut best_score = f32::MIN;` — if a single expert returns NaN for every candidate, no `s > best_score` succeeds and `best_idx` stays at 0. Fine for safety, but add a comment so future maintainers don't change it to `f32::NEG_INFINITY` and break the NaN-safe degenerate path.

### runtime/src/mm/eviction/runtime_xgboost.rs
- **lines 102-110** — `roots` and `nodes` are heap-allocated even though the maximum tree count is unbounded by a `u16`. Add a cap (say 1024 trees and 16K nodes) so a malicious blob cannot OOM the kernel via a 65535-tree header.

### runtime/src/mm/eviction/runtime_mlp.rs
- **lines 71-102** — `parse_payload` reads 4-byte floats via 4-byte reads; OK. But `parse_vec` doesn't bounds-check the cursor — the outer length check at line 85 covers it, but if `PAYLOAD_LEN_V1` is ever changed without `FLOAT_COUNT`, an out-of-bounds index panics in kernel context. Add `debug_assert!(*cursor + count*4 <= bytes.len())` at the top of `parse_vec`.

### runtime/src/mm/eviction/blob.rs
- **lines 68-79** — `read_u16_le`/`read_u32_le` index without bounds check; safe only because `parse_blob` checks `bytes.len() < HEADER_LEN` first. Add `#[inline]` and a debug-bound check or take a fixed-size array.

### runtime/src/mm/eviction/features.rs
- **line 47** — `AI_HORIZON_NS = 1_000_000_000` is a magic constant — already a `pub const` with comment, so this is fine.

### runtime/src/mm/eviction/slm_heuristic.rs
- **lines 65-73** — `bump_active_global` uses `compare_exchange_weak` with `Relaxed`/`Relaxed`. Cross-CPU readers via `get_active` (line 79) use `Relaxed`. Acceptable per the comment ("observational"), but document that this is intentionally inconsistent vs the `model_mem.rs` `Acquire/Release`.

### runtime/src/mm/model_loader.rs
- **line 130** — `From<AllocError> for LoadError` collapses every `AllocError` into `LoadError::AllocFailed` — losing the `StaleHandle`/`PmmFailed` distinction. Map per-variant.

### runtime/src/mm/eviction/tracker.rs
- **line 132** — `self.entries.iter().rposition` then `entries.remove(pos)` is O(n) on `VecDeque` because `remove` shifts. Capacity is 256 — fine, but document.

---

## runtime/src (sched, component, loader, inference, top-level)

### runtime/src/lib.rs
- **lines 4480-4489** — `inference::run_inference` is called inside a single `unsafe { }` even though only the buffer accesses are unsafe; the call itself is safe Rust. This blurs the safety surface and bypasses `ENGINE_LOCK`'s observation that the engine is reentrant-safe at the FFI level. Tighten `unsafe` to only the static-mut deref.
- **lines 64-98 (panic handler)** — uses a 64-byte stack buffer for the file path (good, no allocation), but if `kernel_ffi::uart_puts`/`uart_printf` themselves panic (e.g., null pointer in `slm_print`'s C side, or recursion via the `slm_panic` path), the panic is unbounded. Consider a `static AtomicBool PANICKING` guard that hard-loops on second entry. Also `kernel_ffi::panic` is `extern "C" fn panic(...) -> !` — confirm the C side never returns; if it does, this is UB. The handler does not currently disable IRQs before printing — a timer interrupt during the printf could re-enter the runtime.
- **line 135** — `rust_test_panic` is callable from FFI and reachable in production builds. Gate behind a debug feature flag.

### runtime/src/kernel_ffi.rs
- **lines 491-498** — `print(s: &[u8])` silently no-ops if the slice is not null-terminated. A caller that forgets the `\0` gets no output and no error indication. Either `assert!(s.last() == Some(&0))` (panic = kernel hang, but at least visible) or return `Result<(), KernelError>`.
- **line 417** — `extern "C" { pub fn panic(msg: *const u8) -> !; }` — the C-side `panic` symbol is exported with C linkage; using the unqualified name `panic` shadows Rust's `panic!` macro lexically in any module that does `use kernel_ffi::*`. Rename to `slm_panic` (matches the SLM_ prefix used elsewhere) and adjust callers.

### runtime/src/log.rs
- **lines 88-93** — `uart_print` silently no-ops on missing NUL. Same problem as `kernel_ffi::print`. At minimum, log to a side channel.
- **lines 192-197** — `format_u64` decrements `idx` past 0 in the `idx -= 1` after writing the digit; the loop guard is `v > 0 && idx > 0`, but on the iteration where `idx == 1`, after writing it does `idx -= 1` → 0, then guard fails. Returns `idx + 1 = 1`. OK. But if `value == u64::MAX` (20 digits) and `NUM_BUF_SIZE = 24`, idx starts at 22 (after `idx -= 1` for NUL), 20 digits fits in idx 22..3 — fine. Pin via test against `u64::MAX`.

### runtime/src/msg_router.rs
- **lines 663-679** — The ack-wait loop calls `sched_yield()` (which is `yield`, an arm-friendly cooperative yield), but if all subscribers are non-responsive the loop only exits via `timeout_cycles` from the ARM generic timer. On x86_64 this code path doesn't compile-error, but `timer_get_count` / `timer_get_frequency` are ARM-specific symbols. Confirm the C side stubs them on x86 or this links broken on PLATFORM=X86_64.
- **lines 310-334** — `str_eq_cstr`'s loop exits via `i >= buf.len()` returning false, but the precondition comment says callers guarantee `cstr` has `buf.len() + 1` readable bytes. `cstr_len_bounded` is now called by `subscribe`/`publish` but not before `is_wildcard_pattern` (line 365) — `is_wildcard_pattern` reads up to `max_len` bytes and is called BEFORE `cstr_len_bounded`. Reorder: validate length first, then call `is_wildcard_pattern`.
- **line 298** — `slm_irq_save`/`slm_irq_restore` declared in `extern "C"` but used inside `SpinGuard::new()` without acquiring the lock first. The IRQ save happens before the lock, so a higher-priority IRQ could fire between save and lock acquisition — this is the correct order (mask before lock to prevent priority inversion), but document why the order matters.

### runtime/src/sched/heterogeneous.rs
- **lines 162-164** — `pub fn big_little(big_cores: u8, little_cores: u8) -> Self { let total = big_cores + little_cores; ... }` — `big_cores + little_cores` can overflow `u8` (>255). Use `saturating_add` or accept `usize`.
- **lines 465, 496** — `self.topology.get_core(i).unwrap()` — the loop bound is `0..self.num_cores()` and `get_core(i)` returns `Some` for `i < num_cores`, so this can't fail today, but a refactor could break the invariant. Use `if let Some(core) = ...` or document why infallible.

### runtime/src/sched/inference.rs
- **lines 195-299** — entire file is a skeleton; `submit`/`cancel`/`get_result` return `NotImplemented`. Public API shipped in lib.rs without test coverage. Either gate behind `#[cfg(feature = "wip")]` or write tests that document expected vs current behavior.

### runtime/src/component/registry.rs
- **lines 60-73** — `lock()`/`unlock()` are bare functions, not RAII guards. Every accessor in this file is a `lock(); ... ; unlock(); result` pattern with manual rollback. Switch to `SpinGuard` for consistency with `loader::registry::SpinGuard` and `msg_router::SpinGuard`. Hand-rolled unlocks have already caused real bugs in this codebase.
- **lines 243-263** — `iter()` snapshots into a 16-element stack array and returns an iterator. With `MAX_COMPONENTS=16` and `ComponentInfo` size of ~120 bytes, that's a ~2 KB stack copy per call — acceptable but document.

### runtime/src/component/state.rs
- **lines 217-227** — `name_str()` / `version_str()` use `from_utf8_unchecked` "We control the name bytes and ensure they're valid ASCII." But `set_name` accepts arbitrary `&[u8]` (line 196) — input could be from `parse_manifest` reading file bytes, or from FFI. Use `from_utf8(...).unwrap_or("<bad utf8>")` to make this safe.

### runtime/src/loader/protobuf.rs
- **line 211** — `self.pos = value_start + consumed;` can overflow `usize` if the file is exotically crafted; use `checked_add` and return `LengthOverflow`.

### runtime/src/loader/onnx_parser.rs
- **lines 207-219** — `num_elements()` uses `saturating_mul`, which silently caps at `usize::MAX`. The caller `data_size()` on line 236 uses `saturating_mul(element_size())` — also saturates. A malformed model with shape `[i64::MAX, ...]` returns `usize::MAX` as data size, which is then compared against allocator capacity. Fine in practice (allocator rejects), but log/error explicitly: introduce `EngineError::ShapeOverflow` (already exists in engine) and return `Result` from `num_elements()`.
- **lines 359-423** — every `parse_*` function recurses into nested protobuf via `parse_node`/`parse_tensor_proto`/`parse_value_info`/`parse_type_proto`/`parse_tensor_type`/`parse_tensor_shape`/`parse_shape_dim` with no depth limit. Protobuf allows unbounded nesting; a malicious ONNX with deeply nested shape dims (e.g., DimValue containing another nested message) could cause stack overflow. Add a `depth: u8` parameter and `if depth > MAX_DEPTH { return Err(...) }`.
- **line 484** — `tensor.shape.dims[ndim] = v as i64` — `v: u64`, cast to `i64` silently flips sign for `v > i64::MAX`. Use `i64::try_from(v)` and reject negative-via-overflow shapes.

### runtime/src/loader/registry.rs
- **lines 333-339** — FP16→FP32 conversion loop reads `src[e*2]` and `src[e*2+1]` without checking `src.len() >= 2 * n_elements`. If the ONNX file has truncated `raw_data`, indexing panics. Use `src.get(e*2..e*2+2)` and break on shortage.
- **lines 21-52** — `fp16_to_f32` uses unchecked shift `(half >> 15)` etc. — these are fine for `u16`. But the subnormal path at line 32-41 has `let mut e: i32 = -14; while (m & 0x400) == 0 { m <<= 1; e -= 1; }` — if `mant == 0` we already returned, but if `m & 0x400 == 0` AND m≠0, we loop until we find the bit. Bounded by 10 (mantissa width). OK but document the bound.
- **line 263** — `static mut I64_DECODE_BUF: [u8; 128]` truncates to 16 int64 values silently (line 273 `max_items = 128/8 = 16`). Reshape ops with > 16 dims are dropped. The reshape op uses this as the target shape — corrupted output. Fail loudly: bump to `MAX_DIMS * 8 = 64` is enough for the engine's `MAX_DIMS=8`, but record an error if `i64_packed` had more values than fit.

### runtime/src/loader/graph.rs
- **lines 209-217** — `num_elements()` uses `saturating_mul` (good). `size_bytes()` (line 222) does `num_elements() * elem_type.size()` without saturating — overflow-wraps when `num_elements()` is near `usize::MAX`. Use `saturating_mul`.

### runtime/src/inference/ops.rs
- **lines 78-98** — `matmul_int8` uses `i32` accumulator (`acc += a_val * b_val`); for large K, `a_val` and `b_val` are in `[-256, 255]` (after subtracting zero point), so `a_val * b_val` is in roughly `[-65536, 65536]`. With `K = 32768`, sum reaches `2^31`, overflowing `i32`. Use `i64` accumulator.
- **lines 850, 1182** — `libm::expf` and `super::mathf::tanhf` are used, but no fallback if input is `NaN` / `Inf`. `softmax` will produce `NaN` outputs that propagate silently. Add `if !v.is_finite() { ... }` guards or document.
- **lines 700-710** — `matmul_inner` chooses tiled vs. non-tiled at `TILE = 32`. No test that the two paths produce identical output (within FP rounding).

### runtime/src/inference/engine.rs
- **lines 357-373** — `exec_reshape` reads `*i64_ptr.add(i)` from `shape_tensor.data` (cast to `*const i64`). The data was written by the loader as raw bytes; alignment of `*const i64` is 8, but `weight_ptr.add(offset)` may not be 8-aligned. Force a sentinel that `WeightEntry.offset` is `% 8 == 0` for int64 tensors, or use `read_unaligned`.
- **line 370** — `let dim = *i64_ptr.add(i);` then `dim as u32` (line 361, 369) silently truncates negative values to large `u32`. Should reject negative dims explicitly (already partial — handles `-1` and `0` — but other negatives silently corrupt).
- **line 632** — `gnode.input_count = in_count as u8;` — no overflow check; `in_count` is bounded by `MAX_NODE_INPUTS_PARSE = 3` and `MAX_NODE_INPUTS = 4`, so it fits, but document.

### runtime/src/inference/tensor.rs
- **lines 106-115** — `num_elements()` uses plain `total *= self.shape[i] as usize`, overflows silently. All call sites trust this. Use `saturating_mul` like `loader::graph::TensorShape`. (`workspace.rs::alloc_tensor` does its own checked-mul, so it's safe — but `Tensor::num_elements` is called from many places that don't.)
- **lines 41-45** — `unsafe impl Send/Sync for Tensor` — Tensor holds a raw `*const f32` pointer. Send is fine; Sync claims `&Tensor` is safe to share across threads, which is true only if the pointed-to data is not mutated. `Tensor::data_mut()` casts to `*mut f32`, so a shared `&Tensor` can produce a mutable pointer. Two threads both calling `tensor.data_mut()` race. Sync is unsound as currently used; restrict to Send only, or hide `data_mut` behind `&mut self`.

### runtime/src/inference/gpu.rs
- **lines 194-199** — `core::slice::from_raw_parts(input as *const u8, MNIST_INPUT_FLOATS * 4)` aliases an `*const f32` as `*const u8` for the FFI. Sound (any type is valid as bytes), but document that `input` must be `MNIST_INPUT_FLOATS` floats (already in the precondition comment, just elevate to `// SAFETY:`).
