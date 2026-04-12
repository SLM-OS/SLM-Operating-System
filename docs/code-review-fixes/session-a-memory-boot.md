# Session A — Memory Management + Boot Sequence

**Source:** `docs/code-review-2026-04-12.md` §1 (Memory Management) and §5 (Boot)
**Scope:** 20 issues (3 CRITICAL, 6 HIGH, 7 MEDIUM, 4 LOW)
**Files touched:** `kernel/mm/*.c`, `kernel/arch/arm64/mmu.S`, `kernel/arch/arm64/boot.S`, `kernel/arch/arm64/smp_boot.S`, `kernel/arch/x86_64/boot.S`, `kernel/arch/x86_64/entry64.S`, `kernel/src/dtb.c`, `kernel/src/elf.c`

## Mission

This session hardens the lowest layer of the kernel: physical/virtual memory
management, non-cacheable (NC) allocator accounting, MMU enable and TLB
coherency, and the boot trampolines on both ARM64 and x86-64. Several issues
are carry-forwards from the April 2 review.

This session runs first because downstream sessions — particularly B
(Scheduler) — may depend on NC memory layout changes or new VMM invariants
introduced here.

## Working rules

- Read `CLAUDE.md` (root) and `kernel/CLAUDE.md` — note especially:
  - **Critical struct layout rules** (don't reorder fields ahead of `context`)
  - **NC memory layout table** (don't break `cpu_runqueue` / `task_table` offsets)
  - **Pi 5 cache coherency rules** (DC CIVAC before reader-side, DC CVAC on writer-side)
- All memory-map and MMU changes need testing on **at least QEMU + Pi 5** — these
  are the two most different memory topologies.
- Jetson builds should at minimum compile (`make kernel PLATFORM=JETSON_ORIN_NANO`).
- If you change NC memory layout, grep for `NC_MEM_BASE`, `cpu_rq`, and any
  hardcoded offset in both C and assembly.
- Work in a worktree: `git worktree add ../slm-os-session-a -b fix/session-a-mm-boot`.
- Ask John before committing.

## Issues

### CRITICAL

#### MM-C1 — `get_l2_table()` returns physical address as pointer
- **File:** `kernel/mm/vmm.c:224-242` (specifically line 241)
- **Problem:** Line 241 returns `(uint64_t *)l2_pa` — raw PA cast to pointer.
  Works today only because the identity map on TTBR0 is active.
- **Fix:** Convert PA to kernel VA via `PA_TO_KVA(l2_pa)` before returning.
- **Verification:** Add a VMM test that maps a block, unmaps, and re-maps it
  (exercises `get_l2_table`). Ensure QEMU + Pi 5 boot to shell.
- **Note:** Carry-over from April 2. High latent risk; doesn't crash today.

#### MM-C2 — Pi 5 SPINLOCK_SKIP_LOCKING disables all locks
- **File:** `kernel/include/platform.h:226`, `kernel/include/spinlock.h:21-23`
- **Problem:** Compile-time macro disables every spinlock on Pi 5. With SMP
  live (4 CPUs, cross-CPU dispatch), every shared non-NC structure races.
- **Fix approach:** Replace `SPINLOCK_SKIP_LOCKING` compile-time gate with
  a runtime flag `spinlock_hw_enabled` set to `true` after MMU enable.
  Before MMU enable, spinlocks are barrier-only (as today); after, they use
  real atomic operations. Audit all `spin_lock`/`spin_unlock` call sites.
- **Verification:** QEMU `make test` suite; Pi 5 `labctl boot_test --count 10`
  to verify no regressions; stress test with SMP-enabled workload.
- **Scope warning:** This is **invasive** — if time is tight, defer to a
  GitHub issue and document as post-capstone. Coordinate with John before
  starting.

#### MM-C3 — `ncmem_alloc` accounting calculation is bogus
- **File:** `kernel/mm/ncmem.c:50-51`
- **Problem:** Line 50 is a complex nonsense expression that is immediately
  overwritten by line 51's correct computation. Also no overflow check on
  `aligned + size` (mild risk, usually caller-bounded).
- **Fix:**
  ```c
  nc_next_free = aligned + size;
  nc_total_allocated = nc_next_free - NC_MEM_BASE;
  ```
  Delete line 50 entirely. Add `if (aligned < nc_next_free) return NULL;`
  above the capacity check for overflow defense.
- **Verification:** Unit test in `kernel/tests/` covering alignment edge cases.

### HIGH

#### MM-H1 — `vmm_map_block()` missing TLB invalidation
- **File:** `kernel/mm/vmm.c:281-287`
- **Problem:** Writing a valid L2 descriptor without invalidating stale TLB.
- **Fix:** After the DSB, insert:
  ```c
  __asm__ volatile("tlbi vaae1is, %0" :: "r" (virt >> 12));
  __asm__ volatile("dsb ish; isb");
  ```
  Consider adding a `vmm_invalidate_tlb(va)` helper.
- **Verification:** Remap test — map block, access, unmap, remap with different
  flags, verify access reflects new flags.

#### MM-H2 — No VMM locking on page table modifications
- **File:** `kernel/mm/vmm.c` — `vmm_map_block`, `vmm_unmap_block`, `vmm_map_region`
- **Problem:** Multi-CPU callers race on L2 entries.
- **Fix:** Add `static spinlock_t vmm_lock = SPINLOCK_INIT;` at file scope;
  wrap each public mapping API with `spin_lock_irqsave` / `spin_unlock_irqrestore`.
  **Only useful if MM-C2 is fixed** or if you accept the Pi 5 gap.
- **Verification:** QEMU SMP stress test.

#### MM-H3 — `vmm_state.initialized` guard never checked
- **File:** `kernel/mm/vmm.c:853` (set); no guard in public APIs
- **Fix:** Add `if (!vmm_state.initialized) { ERROR(...); return -1; }` at the
  top of `vmm_map_block`, `vmm_unmap_block`, `vmm_map_region`.
- **Verification:** Call `vmm_map_block` before `vmm_init` in a test; expect
  error return, not corruption.

#### MM-H4 — `pmm_get_free_pages()` / `pmm_get_total_pages()` unlocked
- **File:** `kernel/mm/pmm.c:639-647`
- **Fix:** Wrap each with `spin_lock_irqsave(&pmm_lock)` / `spin_unlock_irqrestore`
  — copy value into a local under the lock, release, return local.
- **Verification:** Existing PMM tests; no behavior change expected single-core.

#### BOOT-H1 — DTB parser missing structure-block bounds validation
- **File:** `kernel/src/dtb.c:135-156`
- **Fix:** In `dtb_validate()` (or wherever `off_dt_struct`/`size_dt_struct`
  are first read), enforce:
  ```c
  uint32_t off = be32_to_cpu(hdr->off_dt_struct);
  uint32_t sz  = be32_to_cpu(hdr->size_dt_struct);
  uint32_t total = be32_to_cpu(hdr->totalsize);
  if (off + sz < off || off + sz > total) return -1;  // overflow or out-of-bounds
  ```
- **Verification:** Craft a malformed DTB in a test and confirm `dtb_validate`
  rejects it.

#### BOOT-H2 — ELF program header array bounds
- **File:** `kernel/src/elf.c:165`
- **Fix:** Before the phdr array cast, add:
  ```c
  if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr) > size)
      return ELF_ERR_TRUNCATED;
  ```
  Use `uint64_t` cast in the multiplication to avoid 32-bit wrap.
- **Verification:** Existing ELF loader tests; add a truncated-ELF test.

### MEDIUM

#### MM-M1 — PA 0 as allocation failure sentinel (carry-over)
- **File:** `kernel/mm/pmm.c:206-221` (`free_list_pop`), `477-505` (`pmm_alloc_pages`)
- **Fix:** Define `#define PMM_ALLOC_FAIL ((uintptr_t)-1)`; change `free_list_pop`
  and `buddy_alloc` to return `PMM_ALLOC_FAIL` instead of 0; update all callers
  to check against the sentinel. Leaves PA 0 as a legitimate allocatable page
  (Pi 5 RAM_BASE).
- **Verification:** PMM test coverage for low-address allocations.

#### MM-M2 — `vmm_dump()` 32-bit multiplication overflow
- **File:** `kernel/mm/vmm.c:690-691`
- **Fix:** Cast to `uint64_t` before multiplying: `((uint64_t)vmm_state.blocks_mapped * BLOCK_SIZE)`.

#### MM-M3 — `pmm_get_buddy_stats()` holds lock during UART output
- **File:** `kernel/mm/pmm.c:652-669`
- **Fix:** Snapshot struct under lock, release, then print (mirror the existing
  `pmm_dump_stats` pattern).

#### BOOT-M1 — `dsb nsh` insufficient after UEFI `ic ialluis`
- **File:** `kernel/arch/arm64/boot.S:237-246`
- **Fix:** Change `dsb nsh` to `dsb sy` after `ic ialluis` on the UEFI
  relocation path. Add a comment explaining per-core L2 coherency on Pi 5/Jetson.

#### BOOT-M2 — Inconsistent DSB after TLBI in MMU setup
- **File:** `kernel/arch/arm64/mmu.S:40,87`
- **Fix:** Use `dsb nsh` consistently for both identity-mapped TLBI sequences
  and document why `nsh` is sufficient pre-SMP.

#### BOOT-M3 — Secondary CPU reads stale `secondary_mmu_*` values
- **File:** `kernel/arch/arm64/smp_boot.S:147-195`
- **Problem:** With MMU off, secondary reads `secondary_mmu_ttbr`,
  `secondary_mmu_mair`, `secondary_mmu_tcr` as Non-Shareable cached; L2 invalidate
  happens after, so stale values can be used.
- **Fix option A (preferred):** Place each with `dc ivac, <addr>; dsb sy`
  before reading.
- **Fix option B:** Move the three values into NC memory at
  `secondary_boot_prepare` time.
- **Verification:** Pi 5 SMP boot reliability — `labctl boot_test --count 10`
  must pass.

#### BOOT-M4 — Missing ISB before `eret` at EL2→EL1 drop
- **File:** `kernel/arch/arm64/boot.S:432-453`, `kernel/arch/arm64/smp_boot.S:77-82`
- **Fix:** Insert `isb` after the final `msr elr_el2, x0` / `msr spsr_el2`
  write, before `eret`. Per ARM ARM D.1.21.1.
- **Verification:** Pi 5 and Jetson boot.

### LOW

#### MM-L1 — `block_state[]` bounds check missing
- **File:** `kernel/mm/pmm.c:232-246`
- **Fix:** In `addr_to_block_index`, add a `DEBUG_ASSERT(idx < MAX_BLOCKS);`
  or silent error return.

#### BOOT-L1 — DAIF mask not the first instruction of `primary_cpu`
- **File:** `kernel/arch/arm64/boot.S:561-571`
- **Fix:** Move `msr daifset, #0xF` to the line immediately after
  `primary_cpu:` label, before stack/FPU/BSS setup. Update comment claim.

#### BOOT-L2 — x86-64 boot.S comment misleading about PD coverage
- **File:** `kernel/arch/x86_64/boot.S:226`
- **Fix:** Clarify comment: "identity-maps 1 GiB; VMM extends to cover additional PDs".
- **Note:** The **"PD zero loop broken"** finding from agent output is a
  **false positive** — `rep stosl` with ECX=4096 zeros 16 KiB (4096 dwords ×
  4 bytes), enough for all three page tables. No code change needed for that.

#### BOOT-C1 — x86-64 GDT not reloaded after long mode
- **File:** `kernel/arch/x86_64/boot.S:141`, `kernel/arch/x86_64/entry64.S`
- **Note:** This was flagged CRITICAL in the review but the severity is
  **uncertain** — builds work on QEMU today. **Investigate first.**
  If the GDT base is truncated on real x86 hardware, add
  `lgdt gdt64_ptr(%rip)` early in `entry64.S`. If current behavior is
  correct, close as not-a-bug and note why in `docs/x86-64-port.md`.

## Suggested work order

1. **MM-C3** (5 min, isolated) — warm up
2. **MM-H4** (15 min, isolated PMM locking)
3. **MM-M3** (15 min, PMM polish)
4. **MM-M2** (5 min, cast fix)
5. **MM-M1** (30 min, PMM sentinel refactor)
6. **MM-H3** (15 min, VMM init guards)
7. **MM-H1** (30 min, TLBI after map)
8. **MM-C1** (30 min, PA_TO_KVA conversion)
9. **BOOT-H1** (20 min, DTB bounds)
10. **BOOT-H2** (15 min, ELF phdr bounds)
11. **BOOT-M4** (5 min, ISB before eret)
12. **BOOT-M1** (5 min, dsb nsh→sy)
13. **BOOT-M2** (10 min, mmu.S consistency)
14. **BOOT-M3** (30 min, secondary MMU stale reads) — **run Pi 5 boot_test after**
15. **BOOT-L1, L2, MM-L1** (15 min total, trivia)
16. **MM-H2** (1 hr) — gated on MM-C2 or accepts Pi 5 gap
17. **MM-C2** (half-day) — **ask John first**, likely out of scope
18. **BOOT-C1** (investigation first; may be no-op) — **ask John before changing x86 boot**

## Testing requirements

- **Every fix:** `make test` (QEMU ARM64). Must pass.
- **MM fixes:** `make test PLATFORM=X86_64`; then `make kernel PLATFORM=RASPI5
  && labctl sdwire_update + boot_test --count 10`.
- **BOOT_M3 specifically:** Pi 5 SMP reliability — boot_test --count 20
  recommended.
- **BOOT-H1 / BOOT-H2:** add negative-input tests in `kernel/tests/test_elf.c`
  (create if needed) and exercise the `dtb_validate` path.
- **MM-H1:** add a test in `kernel/tests/test_vmm.c` that remaps with
  different flags and verifies the new flags take effect.

## Deliverable — PR template

```
## Summary
Session A fixes from code-review-2026-04-12: memory management and boot sequence.

## Issues fixed
- MM-C1, MM-C3, MM-H1, MM-H3, MM-H4 (memory management)
- MM-M1, MM-M2, MM-M3 (memory management polish)
- BOOT-H1, BOOT-H2 (parser bounds checks)
- BOOT-M1..M4 (ARM64 barrier / exception-level polish)
- BOOT-L1, L2, MM-L1 (comments / diagnostics)

## Deferred (filed as issues)
- MM-C2 (runtime spinlock flag refactor)
- BOOT-C1 (x86 GDT reload — investigation inconclusive)

## Test plan
- [ ] `make test` passes on QEMU ARM64
- [ ] `make test PLATFORM=X86_64` passes
- [ ] `make kernel PLATFORM=RASPI5` deploys; `labctl boot_test --count 10` passes
- [ ] `make kernel PLATFORM=JETSON_ORIN_NANO` compiles
- [ ] New VMM remap test passes
- [ ] New DTB bounds test passes
```
