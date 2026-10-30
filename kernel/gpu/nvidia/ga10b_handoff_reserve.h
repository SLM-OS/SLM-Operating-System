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
 * **Scope.** Reserves the inst block of the channel resident on GR at
 * kexec (FECS_CURRENT_CTX), the GR runlist, and — for the published
 * handoff of every pipeline kind (MNIST, SCHED_MLP, EVICTION_QNET;
 * #1029, previously MNIST only) — the handoff page, that channel's
 * own inst block, the helper dmabufs (USERD / GPFIFO / pushbuffer /
 * semaphore / SASS / cbuf / QMD pool), per-op shaders (v7+), GR
 * context extents and v9 weights extents. Each of those channels'
 * VMs — and the resident channel's — is then enumerated from its
 * inst block's PDB (`ga10b_gmmu_enum_pages`) and every page-table
 * page and every mapped page is reserved too (#1029): v6 dispatches
 * relaunch the helper's own QMDs, whose shaders, cbufs and buffers no
 * handoff field names, and `pmm_init`'s free-list links landing on an
 * unreserved PDE table or SASS page wedged the dispatch.
 *
 * Hooked from `kernel_main` between `vmm_init` (which sets up the
 * identity mappings the function needs to read GPU MMIO and DRAM)
 * and `pmm_init` (the consumer of `pmm_user_reserve_add`). Safe to
 * call from non-kexec boots: when `FECS_CURRENT_CTX` reads zero /
 * target=0 / a poisoned-MMIO pattern the inst-block and runlist
 * reserves are skipped, and the per-kind handoff scan finds nothing
 * (one ~50 ms DRAM walk) when no helper was ever run.
 */

#ifndef GPU_NVIDIA_GA10B_HANDOFF_RESERVE_H
#define GPU_NVIDIA_GA10B_HANDOFF_RESERVE_H

#include <stdint.h>

/*
 * Register the kexec-inherited GPU state with the PMM as user-supplied
 * reserves: the inst block of the channel resident at kexec (read from
 * FECS_CURRENT_CTX), the GR runlist, and everything referenced by the
 * published handoff of each pipeline kind via
 * `ga10b_reserve_handoffs_in_range` (see the file-level "Scope"
 * paragraph). The runlist reserve needs a valid resident inst block
 * from the FECS_CURRENT_CTX read; the per-kind pass always runs.
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

/*
 * Return the inst-block phys captured by `ga10b_kexec_handoff_-
 * register_reserves` at boot from FECS_CURRENT_CTX.
 *
 * Returns 0 if the boot capture was skipped (fresh boot / no current
 * ctx / poisoned read) or if the function hasn't run yet. Non-zero
 * means the kernel saved a valid inst-block phys from the very first
 * FECS read after kexec — before any subsequent SLM-OS GPU MMIO
 * could trigger a FECS context switch.
 *
 * Use case (#788 Stage 9): `nvgpu oplib stage`'s auto-discovery
 * reads FECS_CURRENT_CTX at command time, but by then FECS may
 * have ctx-switched to a different channel (observed empirically:
 * boot read 0x13e647000, but post-`nvgpu channel` FECS reads
 * 0x127ebb000 — both valid inst blocks, but only the boot capture
 * matches the inherited channel SLM-OS wants to drive). Prefer the
 * boot capture over live FECS reads for that workflow.
 */
uint64_t ga10b_kexec_inherited_inst_block_phys(void);

/*
 * Reserve every page referenced by the published handoff of each
 * pipeline kind (MNIST, SCHED_MLP, EVICTION_QNET) found in
 * `[start, end)` at `stride`. One scan picks, per kind, the first
 * handoff that passes `ga10b_validate_handoff` — the same choice as
 * `ga10b_find_handoff_of_kind_in_range`. `resident_inst_phys` may be
 * 0 (no resident channel captured). Per
 * handoff: the handoff page, the channel's own inst block (skipped
 * when it equals `resident_inst_phys`, which the FECS_CURRENT_CTX
 * pass already reserved), the helper buffers, per-op shaders, GR
 * ctx extents, weights extents and the channel's whole VM (page
 * tables + mapped pages; the v9 weights pool's VA range is skipped
 * since its extents are already reserved). Finally the resident
 * channel's VM, unless its PDB was already covered.
 *
 * Returns a bitmask with bit `k` set for each kind whose handoff
 * was found.
 *
 * #1029: before this, only the MNIST handoff was reserved, so the
 * sched-MLP and eviction channels' buffers (including their GR
 * context save buffers) were free for PMM to hand out. Called from
 * `ga10b_kexec_handoff_register_reserves`; exposed so the host
 * harness can exercise it against in-memory handoffs.
 */
uint32_t ga10b_reserve_handoffs_in_range(uint64_t start, uint64_t end,
                                         uint64_t stride,
                                         uint64_t resident_inst_phys);

#endif /* GPU_NVIDIA_GA10B_HANDOFF_RESERVE_H */
