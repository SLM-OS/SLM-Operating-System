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

#include <stdbool.h>
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
 * #1207: write the GA10B L2's dirty lines back and invalidate it before
 * pmm_init lists any free page.
 *
 * After a kexec from Linux the GPU L2 can still hold dirty lines for
 * buffers nvgpu freed before the kexec. Those pages are free RAM to
 * SLM-OS. The first L2 flush SLM-OS issues after pmm_init (the inherit's
 * UFLUSH sequence, at the first MNIST call in test_suite_gpu) wrote them
 * back over pages the PMM owned by then: zeroed free-list heads at the
 * MNIST pushbuf + 0x100000 / + 0x120000 on jetson-nano-2, and the "count
 * without a list" halt (3 of 16 baseline kexec suite boots). Flushing
 * here, while those pages hold nothing of SLM-OS's, lets the write-back
 * land before any page is listed. Rows on jetson-nano-2: no zeroed free
 * page in 6 of 6 kexec boots with a diagnostic flush this early (E3,
 * 89068471), nor in 27 of 27 kexec suite boots with this function plus a
 * line-hash window around the MNIST pushbuf (d7fb1010). Not addressed
 * here: with the flush in place, full-memory scans still saw a separate
 * non-zero 64-byte write at page offset 0x2c0 (offset 0x102c0 of a 1 MB
 * aligned region) during the first MNIST call, from a writer not yet
 * identified.
 *
 * Sequence (nvgpu gv11b_mm_l2_flush order, by direct BAR0 MMIO: the GSP
 * platform vtable is not installed this early): FB_FLUSH, L2_FLUSH_DIRTY,
 * L2_SYSMEM_INVALIDATE, FB_FLUSH, each written 1 and polled until
 * PENDING_BUSY and OUTSTANDING clear, with nvgpu's retry budgets (100,
 * 2000, 200, 100 polls, 5 us apart). Skipped, without touching the UFLUSH
 * registers, on a UEFI-direct boot and when FECS_CURRENT_CTX reads 0 or a
 * 0xbadfxxxx PRI error (cold or power-gated GPU: no inherited L2, block
 * possibly unpowered). A timeout is printed loudly and the remaining ops
 * still run; it does not panic. Prints one line with the result.
 */
enum ga10b_l2_flush_early_rc {
    GA10B_L2_FLUSH_EARLY_TIMEOUT     = -1, /* an op did not complete */
    GA10B_L2_FLUSH_EARLY_DONE        = 0,  /* all four ops completed */
    GA10B_L2_FLUSH_EARLY_SKIP_POISON = 1,  /* FECS_CURRENT_CTX = 0xbadfxxxx */
    GA10B_L2_FLUSH_EARLY_SKIP_NO_CTX = 2,  /* FECS_CURRENT_CTX = 0 */
    GA10B_L2_FLUSH_EARLY_SKIP_UEFI   = 3,  /* UEFI-direct boot: no inherited GPU */
};

#define GA10B_L2_FLUSH_EARLY_OPS 4u

/* MMIO accessor (BAR0 offsets), so the sequence is host-testable. */
struct ga10b_early_mmio {
    uint32_t (*r32)(uint32_t off);
    void (*w32)(uint32_t off, uint32_t val);
    void (*delay_5us)(void);
};

/* What the last call did; `before_pmm_init` is recorded by the kernel
 * wrapper (pmm_get_total_pages() still 0). */
struct ga10b_l2_flush_early_status {
    bool ran;
    bool before_pmm_init;
    int rc;
    uint32_t fecs_current_ctx;
    uint32_t polls[GA10B_L2_FLUSH_EARLY_OPS];   /* reads until done, per op */
    uint32_t timed_out_mask;                    /* bit i: op i timed out */
};

int ga10b_kexec_l2_flush_early_with(const struct ga10b_early_mmio *io);
int ga10b_kexec_l2_flush_early(void);
const struct ga10b_l2_flush_early_status *ga10b_kexec_l2_flush_early_status(void);

/*
 * Was the early flush right for this boot? 0 when it was, -1 when not:
 * it did not run, ran after pmm_init, timed out, or was skipped although
 * the boot inherited a GPU from Linux (`inherited_gpu`: not UEFI-direct,
 * and a hand-off or a FECS_CURRENT_CTX inst block was found). A skip is
 * right only on a boot with no inherited GPU. Pure; the test-kernel check
 * and the host harness use it.
 */
int ga10b_l2_flush_early_verdict(const struct ga10b_l2_flush_early_status *st,
                                 bool uefi, bool inherited_gpu);

/*
 * The gap a skip leaves: FECS_CURRENT_CTX read 0 or 0xbadfxxxx at boot,
 * so the early flush was skipped, but GR comes up later and the inherit
 * finds a live context. Its own L2 flush then runs after pmm_init, where
 * #1207's write-back lands on PMM pages. ga10b_bringup_inherit calls this
 * at that point with the FECS_CURRENT_CTX it reads then; on a skip (or if
 * the early flush never ran) it prints one loud WARN line and returns
 * true.
 */
bool ga10b_l2_flush_early_warn_late(const struct ga10b_l2_flush_early_status *st,
                                    uint32_t fecs_now);

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

/*
 * Phys of the `kind` handoff that the boot scan in
 * `ga10b_reserve_handoffs_in_range` picked, and whose pages it
 * reserved; 0 if none was found, the scan hasn't run, `kind` is out
 * of range, or on non-Jetson builds. `ga10b_resolve_handoff_of_kind`
 * (ga10b_channel_handoff.h) uses it to avoid re-walking DRAM at
 * dispatch time (#1037).
 */
uint64_t ga10b_boot_handoff_phys(uint32_t kind);

/*
 * Whether that boot scan has run. Once it has, its per-kind answer is
 * final, including "none": handoffs are only created by Linux helpers
 * before kexec, so a later scan could only find a page the boot pass
 * did not reserve, and paid ~50 ms per miss with IRQs masked (#1037).
 * Always false on non-Jetson builds.
 */
bool ga10b_boot_handoff_scan_ran(void);

#ifdef SLM_HOST_HARNESS
/* Forget the boot scan (records and the ran flag). Harness only. */
void ga10b_boot_handoff_reset_for_test(void);
#endif

#endif /* GPU_NVIDIA_GA10B_HANDOFF_RESERVE_H */
