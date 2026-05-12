/*
 * ga10b_handoff_reserve.h — register GA10B kexec-handoff state with
 * the PMM as user-supplied memory reservations.
 *
 * Problem statement (#788): when SLM-OS kexecs over Linux while
 * `gpu-channel-helper` is holding an nvgpu channel open, the
 * channel's inst block + GMMU page tables + GR ctxsw save buffer
 * survive kexec in DRAM — Linux doesn't actively free them, the
 * helper kept the fds open. But SLM-OS's PMM has no knowledge of
 * those allocations and happily hands the same physical pages out
 * to its own kernel code, model preload buffers, ramdisks, etc.
 * By the time SLM-OS issues its first GPU submit, the channel
 * state has been clobbered byte-for-byte — proven by the
 * `nvgpu instdump` shell command (PR #797).
 *
 * Fix: read FECS_CURRENT_CTX before PMM init publishes pages, decode
 * the channel's inst block phys, validate it's inside DRAM, and
 * register it as a user reserve via `pmm_user_reserve_add`. PMM
 * carves the registered range out of every region it adds to the
 * buddy allocator, so kernel allocations naturally steer around the
 * inherited channel's pages.
 *
 * **Scope of this MVP:** reserves the channel inst block only — one
 * 4 KB page identified via the FECS_CURRENT_CTX MMIO register. The
 * inst block carries the PDB pointer; without protecting it, any
 * GMMU walk reads garbage and faults at the first dispatch. Future
 * work will extend to walking the PDB tree and reserving every page
 * table page, plus the helper-allocated dmabufs (USERD / GPFIFO /
 * pushbuffer / semaphore / SASS pool) listed in the v9 handoff. The
 * MVP picks the highest-value single reservation first so the
 * hardware verification gives a clean before/after signal on whether
 * inst-block-only protection is enough to keep the wedge from firing.
 *
 * Hooked from `kernel_main` between `vmm_init` (which sets up the
 * identity mappings the function needs to read GPU MMIO and DRAM)
 * and `pmm_init` (the consumer of `pmm_user_reserve_add`). Safe to
 * call from non-kexec boots — the function early-returns when
 * `FECS_CURRENT_CTX` reads zero / target=0 / a poisoned-MMIO
 * pattern, all of which characterise fresh boots where no channel
 * was ever live.
 */

#ifndef GPU_NVIDIA_GA10B_HANDOFF_RESERVE_H
#define GPU_NVIDIA_GA10B_HANDOFF_RESERVE_H

/*
 * Discover the kexec'd channel's inst block via FECS_CURRENT_CTX and
 * register its physical page with the PMM as a user-supplied reserve.
 *
 * No return value: the function logs every decision (skipped because
 * no current ctx / outside DRAM / reserve table full / success) so
 * the UART log carries the diagnostic trail, but the caller has
 * nothing it could meaningfully act on — if the reservation fails,
 * the channel will subsequently wedge during inheritance and the
 * existing wedge handler will dump enough state to triage.
 *
 * Safe to call on non-Jetson platforms only via the platform-guarded
 * call site; the implementation reads Jetson-specific MMIO (BAR0 at
 * 0x17000000) directly. Building the function for other targets is
 * a no-op via the file-level platform guard in `ga10b_handoff_reserve.c`.
 */
void ga10b_kexec_handoff_register_reserves(void);

#endif /* GPU_NVIDIA_GA10B_HANDOFF_RESERVE_H */
