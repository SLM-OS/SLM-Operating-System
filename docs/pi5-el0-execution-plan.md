# Pi 5 — Real EL0 User-Mode Execution Plan

**Status:** Plan stage. Not started.

**Issue:** [#697](https://github.com/SLM-OS/SLM-Operating-System/issues/697) — real EL0 user-mode execution: VMM_FLAG_USER on Pi 5 RAM, per-task TTBR0, smoke EL0 task, hardware verification.

**Predecessor:** [#683](https://github.com/SLM-OS/SLM-Operating-System/issues/683) — closed; moved Pi 5 to EL2/VHE so EL0 SVC reaches `VBAR_EL2 + 0x400`. Vector path verified by `test_lower_el_sync_vector_dispatches_to_el0_sync` (PR-5 / #698).

**Predecessor:** Phase 5 M4 (commit `91c73f91`, April 2026) — added the syscall ABI, EL0 vectors, `task_create_user`, `user_entry.S`, and `VMM_FLAG_USER` PTE-bit handling. Deferred actually flagging any pages with `VMM_FLAG_USER` because it broke QEMU. That defer is what this issue closes.

**Predecessor:** [#706](https://github.com/SLM-OS/SLM-Operating-System/pull/706) — closed step 1 of #697 (audit the QEMU MMU hang). Re-confirmed the hang reproduces on QEMU 8.2.2 / cortex-a76; ruled out PAN/EPAN/PAN3, WXN, SPAN. Concluded the production design (per-task TTBR0) avoids the hang by construction.

---

## Why this exists

Phase 5 M4 left a working SVC dispatch + EL0 vector + `task_create_user` API but no actually-runnable EL0 task. The blocker is purely VMM: every kernel mapping uses `AP=00` (kernel-only); no page is `VMM_FLAG_USER`. An EL0 task ERETing into its first instruction would prefetch-abort.

The natural fix would be to flag user-task pages (text + stack) with `AP=01` while leaving kernel pages at `AP=00`. The complication: today, **TTBR0 and TTBR1 share one L1 table** (kernel/mm/vmm.c:1287). User mappings would have to live in that same L1 alongside kernel mappings. That means:

- All user tasks share an address space — task A can read task B's pages (no per-task isolation).
- The page-table ownership model is muddled: which subsystem allocates user pages, who frees them, what does `task_destroy` reclaim?
- Future extensions (mmap, fork-style separate ASes, ASIDs) become awkward.

Per-task TTBR0 — the standard kernel-OS design — gives clean separation. Each EL0 task owns its own L1 table for the lower VA half. The kernel keeps `TTBR1_EL1` with kernel-only mappings. Context switch into a user task swaps `TTBR0_EL1` and invalidates the TLB.

---

## Goal

After this work lands:

1. Each EL0 task has its own L1 table at `TTBR0_EL1` populated with mappings for its text + stack (`AP=01`, `UXN=0`, `PXN=0`, `MAIR=NORMAL_WB`).
2. The kernel runs at high VA (`TTBR1_EL1` region) with `AP=00` mappings — never `VMM_FLAG_USER`. The QEMU hang trigger condition is structurally avoided.
3. `switch_to` writes `TTBR0_EL1` and invalidates the TLB when transitioning into a user task.
4. A boot-time smoke test creates an EL0 task that issues `SVC #0 SYS_LOG`, then `SVC #0 SYS_EXIT`, and verifies the round-trip works on both QEMU virt and Pi 5 hardware at EL2/VHE.

---

## Out of scope

- **ASID-based TLB management.** A full TLB flush on every user-task switch is fine for capstone scope; ASID tagging is a follow-up.
- **Copy-on-write / page faults.** No demand-paging.
- **`mmap` / `munmap` syscalls.** User memory is set up at task creation and freed at task destroy — no dynamic mapping.
- **ELF user-mode execution.** `cmd_run` keeps loading ELFs as kernel-mode tasks. Switching `run` to EL0 is a separate effort once the smoke task lands.
- **x86-64 Ring 3.** ARM64 only; `task_create_user` is already gated by `#if !defined(PLATFORM_X86_64)`.
- **Stage-2 translation / KVM-style nested guests.** `HCR_EL2.VM=0` stays.
- **Re-investigating the QEMU TCG hang on user-flagged kernel pages.** Already closed by #706 — design avoids the trigger.

---

## Considered alternative — single-L1 with per-page USER flags

A cheaper design exists: keep `TTBR0=TTBR1` sharing today's L1 table, leave kernel pages at `AP=00`, and flag only user-allocated pages with `AP=01`. The QEMU hang still wouldn't fire because we never set `VMM_FLAG_USER` on kernel pages. This delivers EL0/EL1 isolation but **not** isolation between user tasks (shared address space).

This is a defensible scope for a capstone OS — the EL0 SVC round-trip works, and components running at EL0 can't crash the kernel. But it forecloses on per-task isolation forever (or requires reverting later).

**Per-task TTBR0 is the path chosen** for #697. Documented here so a future maintainer who wants to revisit the trade-off knows there's a cheaper option.

---

## PR breakdown

### PR 1 — Plan doc + audit + groundwork

This PR. Sets up the planning structure, audits files that change, and lands minimal scaffolding (a `task->user_l1_pa` field, unused, so PR 3 can populate it without an API churn).

Touches: `docs/pi5-el0-execution-plan.md` (new), `kernel/include/task.h` (new field).

Acceptance: plan doc reviewed; kernel still builds clean on all 3 ARM64 platforms; `make test` passes.

### PR 2 — Per-task L1 plumbing

Goal: prepare a `vmm_create_user_l1` / `vmm_destroy_user_l1` API so PR-3 can populate per-task L1 tables, and decide concretely how the kernel mappings remain reachable when a per-task L1 is loaded into `TTBR0_EL1`.

Two designs are viable; **PR 2 picks one before writing code**:

#### Option A — L1-clone (kernel stays at low VA)

- Each per-task L1 (allocated from PMM) starts as a copy of the boot L1's entries. L1 entries point to **shared** L2 tables for kernel mappings (so a future kernel-side mapping change propagates to all per-task L1s without per-task fix-up). The user-region L1 entries (covering whatever VA range a user task gets) are zeroed initially; PR 3/4 populate them with per-task L2 tables.
- TTBR0 swap on context switch loads `task->user_l1_pa`. Kernel mappings stay reachable at the same low VA they're at today because the per-task L1 mirrors them via shared L2s.
- Kernel doesn't move. Linker.ld unchanged. Boot.S unchanged.
- Touches: `kernel/mm/vmm.c` (refactor `vmm_init` to keep kernel L2 tables addressable; new `vmm_create_user_l1` / `vmm_destroy_user_l1`), `kernel/include/vmm.h` (new declarations).
- Risk: low (additive).

#### Option B — Kernel at high VA (TTBR1) — original plan

- Linker `.text`/`.data`/`.bss` relocated to `KERNEL_VA_BASE = 0xFFFFFF8000000000`. Boot.S adds a post-`mmu_enable` trampoline that branches from the identity-mapped low VA to the linked high VA. SP, VBAR, and any in-flight literal pointers reload to high VA.
- After PR 2, kernel runs at high VA via `TTBR1_EL1`. `TTBR0_EL1` is freed; per-task L1s are pure user mappings (no kernel content).
- Real KPTI-style separation. Future-proof for ASID tagging and stage-2 translation. Mirrors what every production kernel-OS does.
- Touches: `kernel/arch/arm64/linker.ld`, `kernel/arch/arm64/boot.S`, `kernel/arch/arm64/mmu.S` (separate TTBR0/TTBR1 args), `kernel/mm/vmm.c` (L1 split), `kernel/include/vmm.h` (new constants), every platform's boot path.
- Risk: high (every absolute-address pointer in the kernel; all 3 ARM64 boot paths affected).

#### Decision

| Concern | Option A (L1-clone) | Option B (high-VA) |
|---|---|---|
| Per-task EL0 isolation | ✓ | ✓ |
| Kernel/user VA separation | ✗ (shared low-VA AS) | ✓ (separate halves) |
| Boot path / linker changes | None | Major |
| ASID future-proofing | Adequate | Better |
| Estimated PR 2 size | ~150 lines | 200–500 lines, multi-platform |
| Risk of regression | Low | High |

For #697's scope (run a smoke EL0 task end-to-end), **Option A is sufficient and gets us there faster**. Option B becomes worthwhile once the kernel needs real KPTI-style separation (e.g. for security or for nested guests) — neither is in #697's scope.

**PR 2 implements Option A.** A future "kernel at high VA" effort can layer on top without re-doing PR-3/4.

#### Mechanism (Option A)

- `kernel/mm/vmm.c`:
  - Refactor `vmm_init` so `l1_table` (the boot L1) is built from explicit references to L2 tables that live at known PAs. The L2 tables (`l2_kernel`, `l2_mmio`, `l2_ram_*`, `l2_pcie_bar_win`) stay file-scoped statics today; PR-2 makes their PAs reachable from the new helpers.
  - New `int vmm_create_user_l1(uint64_t *out_pa)`: allocates a 4 KB L1 page from PMM; copies the boot L1's L1 entries (which all happen to be in the lower VA range today since the kernel runs there) into the new L1; the user-region entries (TBD which range) are left zeroed. Returns 0 on success.
  - New `void vmm_destroy_user_l1(uint64_t l1_pa)`: walks the per-task L1, frees any per-task L2/L3 sub-tables, frees the L1 page itself. Doesn't free shared kernel L2s.
- `kernel/include/vmm.h`: declarations + a `USER_VA_BASE` / `USER_VA_LIMIT` pair defining which L1 entries are per-task vs shared (probably `0..256 GB` for user, the rest for kernel today since kernel image is in the low couple of GB).

Touches: `kernel/mm/vmm.c`, `kernel/include/vmm.h`. No assembly changes, no linker-script changes, no boot.S changes.

Acceptance:

- `vmm_create_user_l1` returns a fresh L1 PA. Walking the L1 shows kernel L1 entries are mirrored from the boot L1 (same L2 PAs), and user-region L1 entries are zero.
- `vmm_destroy_user_l1` frees the L1 cleanly.
- New unit tests in `kernel/tests/test_vmm.c` cover both helpers.
- `make test` (QEMU virt) passes.
- `kernel PLATFORM=RASPI5` and `PLATFORM=JETSON_ORIN_NANO` build clean.
- No hardware re-verification needed (no behavior change to running kernels — only new helpers).

### PR 3 — Per-task TTBR0 allocation + context-switch swap

Goal: allocate a fresh L1 table per user task; swap `TTBR0_EL1` on context switch into / out of a user task; flush TLB on transitions.

Mechanism:

- `task_create_user`: allocate a 4 KB L1 page from PMM (kernel-only access on the page itself), populate it with mappings for the task's text page(s) and stack page(s) using `VMM_FLAG_USER`. Store the PA in `task->user_l1_pa`.
- `task_destroy`: walk the per-task L1, free any L2/L3 sub-tables, free the L1 page itself.
- `switch_to` (`kernel/arch/arm64/context.S`): on entry, if the next task is `is_user`, write its `user_l1_pa` to `TTBR0_EL1` and `tlbi vmalle1`. If the previous task was `is_user` and the next task is kernel-mode, optionally clear TTBR0 (or leave it — kernel-mode tasks never access TTBR0 region after PR 2).
- New `kernel/mm/user_vmm.c` for per-task user-mapping helpers (`user_vmm_map`, `user_vmm_unmap`, `user_vmm_destroy`).

Touches: `kernel/sched/task.c`, `kernel/include/task.h` (use the field added in PR 1), `kernel/arch/arm64/context.S` (TTBR0 swap), `kernel/mm/vmm.c` and new `user_vmm.c`, `kernel/include/vmm.h`.

Acceptance:

- `task_create_user` allocates a per-task L1, returns a non-NULL task with `user_l1_pa != 0`.
- A new test in `kernel/tests/test_syscall.c` walks a freshly-created user task's L1 and asserts the expected entries are present with `AP=01`.
- `make test` passes.
- Pi 5 boots to shell at EL2/VHE, no regression.

### PR 4 — Smoke EL0 task + hardware verification

Goal: actually run an EL0 task end-to-end on QEMU virt and Pi 5 hardware.

Mechanism:

- New `kernel/src/user_smoke.c` compiled with `-fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables` to avoid GOT references / canary checks. Single function `user_smoke_main()` that calls `sys_log("[USERTEST] hello\n", 17)` and `sys_exit(0)` via the inline-asm stubs in `user_syscall.h`.
- New section `.text.user` in the linker script — pages that the user-task L1 needs to map with `VMM_FLAG_USER`. The kernel L1 maps `.text.user` with `AP=00` like any other kernel section (for relocation / debug visibility). Per-task L1 maps it with `AP=01`.
- `task_create_user` populates the per-task L1 with `.text.user` and a freshly-allocated stack page mapped `AP=01` `UXN=0`.
- New `usertest` shell command in `kernel/src/shell_sys.c` that creates the task and waits up to 1 s for it to terminate.

Touches: `kernel/src/user_smoke.c` (new), `kernel/arch/arm64/linker.ld` (new section), `kernel/sched/task.c` (extend `task_create_user`), `kernel/src/shell_sys.c` (new command), `kernel/tests/test_syscall.c` (round-trip test).

Acceptance:

- `make test` (QEMU virt) — `usertest` round-trip prints `[USERTEST] hello`, task exits cleanly, kernel stays alive.
- Pi 5 (`pi-5-2`, EL2/VHE, `SECONDARY_PREEMPT=ON`): `usertest` from the shell prints `[USERTEST] hello`, returns to shell prompt.
- 10/10 `boot_test` reliability with `usertest` invoked from boot probe.

### Follow-up (not part of #697)

- **ASID tagging.** Trade `tlbi vmalle1` on every TTBR0 swap for ASID-tagged TLB entries.
- **`mmap` / `munmap` syscalls.** Dynamic user memory beyond what `task_create_user` sets up.
- **EL0 ELF execution.** Switch `cmd_run` to load ELF programs as EL0 tasks instead of kernel-mode tasks.
- **x86-64 Ring 3.** Equivalent design with CR3 swap and IDT for syscalls.

---

## Test plan

Each PR must pass:

- `make test` (QEMU virt at EL1) — confirms QEMU path unaffected (until PR 4 adds the EL0 round-trip).
- `make kernel PLATFORM=RASPI5` — clean build.
- `make kernel PLATFORM=JETSON_ORIN_NANO` — clean build as a cross-platform regression check (Jetson is also EL2/VHE).
- Pi 5 `pi-5-2` boot to shell with `SECONDARY_PREEMPT=ON` — no regression.

PR 2 specific (Option A — L1-clone):

- `vmm_create_user_l1` returns a fresh L1 PA; walking it shows mirrored kernel L1 entries + zeroed user-region entries.
- `vmm_destroy_user_l1` frees the L1 cleanly without leaking sub-tables.
- New unit tests in `kernel/tests/test_vmm.c` cover both helpers.
- A kernel-context syscall_dispatch test still passes (no regression on the existing fabricated-trap-frame tests in `test_syscall.c`).

PR 3 specific:

- `kernel/tests/test_syscall.c::test_user_task_l1_walks_correctly` — creates a user task, walks its L1, asserts `AP=01` on text + stack entries, `AP=00` (or invalid) elsewhere.

PR 4 specific:

- `usertest` shell command on QEMU virt prints `[USERTEST] hello` and returns control to the shell.
- Pi 5 hardware: same.
- 10/10 `boot_test --count 10`.

---

## Risks

1. **Per-task L1 mirroring drift.** Under Option A (L1-clone), each per-task L1 has its own copy of the boot L1's L1 entries. If the kernel later changes a kernel mapping by writing the boot L1 directly (rather than the underlying L2), the change won't propagate to existing per-task L1s. Mitigation: kernel-side mapping changes go through L2-level edits (which ARE shared). Document the invariant in `vmm.h` next to `vmm_create_user_l1`. Optionally, walk the per-task L1 table and re-clone if needed; deferred unless this surfaces.
2. **TLB flush semantics on PR 3's TTBR0 swap.** `tlbi vmalle1` (non-shareable) is sufficient pre-SMP; for SMP we need `tlbi vmalle1is`. Today's scheduler can run a user task on any CPU. Mitigation: use the `is` variant from the start; pin the task to one CPU as a defensive option.
3. **Linker-script section ordering for `.text.user` (PR 4).** Putting it adjacent to `.text` keeps PMM-free; putting it elsewhere requires extra mapping logic. Mitigation: standard linker-script practice.
4. **Pi 5 cache discipline.** PR 3 changes the page tables at runtime under SMP. Existing `cache_clean_range` helpers cover this; new code must use them.

---

## References

- `docs/component-isolation.md` — Phase 5 M4 syscall infrastructure.
- `docs/pi5-el2-vhe-plan.md` — #683 EL2/VHE refactor (predecessor).
- `kernel/arch/arm64/user_entry.S` — EL0 ERET trampoline.
- `kernel/include/syscall.h` / `kernel/src/syscall.c` — syscall ABI + dispatch.
- ARM ARM D8.2 (Translation table descriptor formats) — AP/XN bit definitions.
- ARM ARM D8.3 (Translation lookaside buffer maintenance) — `tlbi vmalle1` vs `tlbi vmalle1is`.

---

*Last updated: 8 May 2026 — PR 2 design split into Option A (L1-clone) and Option B (kernel at high VA); PR 2 chosen to implement Option A.*
