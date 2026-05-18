/*
 * ga10b_handoff_reserve.c — implementation of
 * `ga10b_kexec_handoff_register_reserves`. See the header for the
 * #788 problem statement.
 *
 * The function reads FECS_CURRENT_CTX (BAR0 + 0x409b00), decodes the
 * inst-block physical address per `gr_fecs_current_ctx_*`
 * (bits[27:0] = inst_phys >> 12, bits[29:28] = aperture target),
 * bounds-checks it against the DRAM aperture via `ga10b_phys_in_dram`,
 * and registers a single 4 KB reserve via `pmm_user_reserve_add`.
 *
 * Why read FECS_CURRENT_CTX directly here instead of going through
 * `ga10b_gmmu_discover_inst_block_phys`?
 *
 * The MMU is up at the call site (vmm_init has run, identity-maps
 * cover all DRAM + the GPU BAR0 aperture), but the higher-level
 * GPU bringup infrastructure (`ga10b_bringup` state machine, PMM-
 * dependent allocations inside `ga10b_gmmu_*`) has not been
 * initialised — `pmm_init` itself is the next call after us. Reading
 * the raw MMIO directly keeps the dependency footprint to the
 * identity mapping plus `pmm_user_reserve_add`, both of which are
 * known-safe to use this early.
 *
 * **Limitations of the MVP:**
 *
 *   - Reserves only the inst block. The PDB and page-table pages
 *     pointed to from the inst block also need protection but the
 *     walk requires reading those pages too — adding correctness
 *     gates that aren't worth shipping until the inst-block-only
 *     reservation is hardware-verified to fix the wedge or proves
 *     insufficient.
 *
 *   - Reads `FECS_CURRENT_CTX` from BAR0 directly. The Tegra GA10B
 *     GPU MMIO can return poisoned values (`0xbadfXXXX` family) when
 *     the GPU is power-gated. Those decode to a non-DRAM `inst_phys`
 *     and are filtered out by the `ga10b_phys_in_dram` check; the
 *     function logs the skip and returns cleanly.
 *
 *   - Assumes the helper kept the channel loaded on the GR engine
 *     through kexec — i.e. FECS still has a meaningful current ctx
 *     to report. A `--gpu-suspend` kexec breaks this assumption;
 *     the function early-returns on `target == 0` (and on poisoned
 *     reads) so suspended-GPU boots cost only a single MMIO read.
 *
 * **Patched-`nvgpu.ko` dependency** (#788 v10/v11 fix path).
 * `reserve_gr_ctx_extents` consumes `h->gr_ctx_extents_phys` /
 * `gr_ctx_n_extents` which are populated only when the Linux-side
 * `nvgpu.ko` is built from the patch at
 * `tools/nvgpu-patches/0001-expose-gr-ctx-phys-via-debugfs.patch`.
 * Without the patched module installed, those fields stay zero,
 * the function logs the `gr_ctx reservation skipped (...)` line,
 * and per-attempt PASS regresses from ~60% to the v6-equivalent
 * ~17%. See PR #823 §Dependencies for the build/install steps.
 */

#include "ga10b_handoff_reserve.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "ga10b_channel_handoff.h"
#include "ga10b_gmmu.h"
#include "../../include/dtb.h"
#include "../../include/pmm.h"
#include "../../include/uart.h"

#include <stdbool.h>
#include <stdint.h>

/* MMIO offsets — same as `ga10b_gmmu.c` and `ga10b_bringup.c` use,
 * duplicated here so this TU has no compile-time dependency on the
 * larger bringup module (which itself pulls in `pmm.h` transitively
 * and could create cycles in early-boot ordering). */
#define GA10B_BAR0_BASE                  0x17000000ull
#define GA10B_GR_FECS_CURRENT_CTX_OFFSET 0x00409b00u

/* `gr_fecs_current_ctx_*` field encoding per L4T nvgpu
 * `~/slmos-ref/nvidia/nvgpu-include-nvgpu-hw-gv11b-hw_gr_gv11b.h`:
 *   bits [27:0]  = `ptr` = inst_block_phys >> 12
 *   bits [29:28] = `target` (0=vid_mem, 2=sys_mem_coh, 3=sys_mem_ncoh)
 * target == 0 on Tegra means "no current ctx" (no vidmem) or stale
 * post-kexec garbage; skip the reservation in either case. */
#define GA10B_FECS_CTX_PTR_MASK     0x0FFFFFFFu
#define GA10B_FECS_CTX_TARGET_SHIFT 28u
#define GA10B_FECS_CTX_TARGET_MASK  0x3u

/* GA10B inst block is 4 KB. Hardcoded constant duplicated from
 * `ga10b_bringup.c`'s `GA10B_INST_BLOCK_BYTES` so this TU stays
 * self-contained — the larger bringup header has the
 * compile-time-pinned definition; if the size ever changes on
 * Ampere, both sites need updating in lock-step. */
#define GA10B_INST_BLOCK_BYTES 4096u

/* Boot-time capture of the kexec'd channel's inst-block phys. Read
 * from FECS_CURRENT_CTX in `reserve_inst_block` BEFORE any other
 * SLM-OS GPU MMIO touches FECS. The live FECS_CURRENT_CTX register
 * drifts after kexec — SLM-OS's own subsequent GPU probes
 * (nvgpu inherit / channel / submit) trigger FECS context switches
 * to other channels Linux had loaded, and the helper-published
 * handoff doesn't carry an inst_block_phys field. So this is the
 * single authoritative copy of "which inst block did Linux just
 * leave on the GR engine when we kexec'd."
 *
 * Exposed via `ga10b_kexec_inherited_inst_block_phys()` for the
 * `nvgpu oplib stage` shell verb (and any future caller that needs
 * to dereference the inherited channel's GMMU after FECS has drifted). */
static uint64_t g_boot_inst_block_phys = 0;

/* ---------------------------------------------------------------
 * Sub-functions, one per reservation class. Called in order by
 * `ga10b_kexec_handoff_register_reserves` below; the orchestrator
 * threads the inst-block early-out and the handoff scan.
 * --------------------------------------------------------------- */

/* Read FECS_CURRENT_CTX, decode the inst-block phys, validate it
 * lives in DRAM, and register a 4 KB reserve. Stashes the verified
 * phys in `g_boot_inst_block_phys` for later access.
 *
 * Returns true iff a valid inst block was reserved (caller should
 * continue with downstream reservations). Returns false on any skip
 * path: poison MMIO, target=0 fresh boot, outside-DRAM phys, or
 * PMM table full. */
static bool reserve_inst_block(void)
{
    /* Read FECS_CURRENT_CTX directly via the identity mapping that
     * vmm_init has just established. Volatile so the compiler can't
     * elide or reorder; `bar0_r32` from ga10b_bringup.c would route
     * through the GSP platform vtable which is still NULL this
     * early in boot. */
    uint32_t reg = *(volatile uint32_t *)(uintptr_t)
        (GA10B_BAR0_BASE + GA10B_GR_FECS_CURRENT_CTX_OFFSET);

    /* GA10B GPU MMIO returns the `0xbadfXXXX` family of poison
     * patterns when the GPU is power-gated or off (PRI bad-access
     * response). Filter those out specifically — the `target` check
     * below would catch most by coincidence (poison patterns
     * typically decode to target=2 or 3 with a garbage ptr field,
     * which then fails the DRAM bounds check), but the explicit
     * filter logs the cause clearly instead of "outside DRAM" for
     * the case the operator hits most often (fresh boot, no
     * helper). */
    if ((reg & 0xFFFF0000u) == 0xbadf0000u) {
        uart_printf("[ga10b-reserve] FECS_CURRENT_CTX=0x%08x (priv-bad "
                    "poison — GPU likely power-gated). Skipping "
                    "inst-block reservation.\n",
                    (unsigned)reg);
        return false;
    }

    uint32_t target = (reg >> GA10B_FECS_CTX_TARGET_SHIFT) &
                      GA10B_FECS_CTX_TARGET_MASK;
    if (target == 0u) {
        uart_printf("[ga10b-reserve] FECS_CURRENT_CTX=0x%08x (target=0 "
                    "— no current ctx, or fresh boot). Skipping.\n",
                    (unsigned)reg);
        return false;
    }

    uint64_t inst_phys =
        ((uint64_t)(reg & GA10B_FECS_CTX_PTR_MASK)) << 12;

    if (!ga10b_phys_in_dram(inst_phys, GA10B_INST_BLOCK_BYTES)) {
        uart_printf("[ga10b-reserve] decoded inst_phys=0x%lx outside "
                    "DRAM (FECS_CURRENT_CTX=0x%08x, target=%u). "
                    "Skipping reservation.\n",
                    (unsigned long)inst_phys, (unsigned)reg,
                    (unsigned)target);
        return false;
    }

    int rc = pmm_user_reserve_add(inst_phys,
                                  (uint64_t)GA10B_INST_BLOCK_BYTES);
    if (rc != 0) {
        uart_printf("[ga10b-reserve] pmm_user_reserve_add(0x%lx, %u) "
                    "rc=%d — reservation NOT registered. PMM will hand "
                    "out the inst-block page; expect a #788-style "
                    "wedge on the first GPU submit.\n",
                    (unsigned long)inst_phys,
                    (unsigned)GA10B_INST_BLOCK_BYTES, rc);
        return false;
    }

    /* Save the verified inst_phys for the inherited-inst-block
     * accessor — the only point in boot where FECS_CURRENT_CTX is
     * guaranteed to still hold Linux's last-active channel pointer.
     * Future GPU MMIO touches by SLM-OS can ctx-switch FECS away
     * from this channel; callers that need to drive the inherited
     * channel later (e.g. `nvgpu oplib stage` for SASS upload)
     * must read this stashed value, not the live register. */
    g_boot_inst_block_phys = inst_phys;

    uart_printf("[ga10b-reserve] reserved kexec'd channel inst block "
                "at phys=0x%lx (size=%u, FECS_CURRENT_CTX=0x%08x, "
                "target=%u)\n",
                (unsigned long)inst_phys,
                (unsigned)GA10B_INST_BLOCK_BYTES,
                (unsigned)reg, (unsigned)target);
    return true;
}

/* #788 Stage 7 — read the GR engine's runlist submit_base register
 * and reserve the runlist's pages.
 *
 * The runlist is a per-engine DRAM buffer that PBDMA reads to
 * find which channels to schedule. Linux's nvgpu allocates it
 * via `gk20a_gmmu_alloc_sys` (sys mem, page-aligned) and writes
 * the address into `runlist_submit_base_lo/hi`. The handoff
 * struct doesn't publish the runlist phys, so without this
 * step SLM-OS's PMM may hand out the runlist's pages to its
 * own allocations — Stage 6 trace proves intermediate PT-page
 * allocations during the GMMU rebuild land on physical pages
 * adjacent to / overlapping the runlist when pushbuf or sem
 * is reserved.
 *
 * GA10B Tegra has a single GR runlist with empirically-observed
 * pri_base at BAR0 + 0xC000 (per the PTOP device-info table
 * walk in L4T nvgpu's `ga10b_top_parse_next_dev`). So
 * `runlist_submit_base_lo` is at BAR0 + 0xC080, `_hi` at
 * BAR0 + 0xC084.
 *
 * Register encoding per `~/slmos-ref/nvidia/nvgpu-hw-ga10b-hw_runlist_ga10b.h`:
 *   lo bits[31:10] = runlist_iova >> 10 (1 KB-aligned ptr)
 *   lo bits[1:0]   = target aperture
 *   hi bits[7:0]   = runlist_iova bits[39:32]
 *
 * Runlist size: 32 KiB per buffer × 2 buffers (double-buffered)
 * = 64 KiB total, page-aligned.
 *
 * If the empirical 0xC000 offset is wrong on a future chip
 * revision, the read will return a value outside DRAM and
 * the `ga10b_phys_in_dram` check will skip the reservation
 * cleanly — Stage 4+5 regression behavior worsens but the
 * boot continues. */
static void reserve_runlist(void)
{
    const uint64_t runlist_lo_addr = GA10B_BAR0_BASE + 0xC080u;
    const uint64_t runlist_hi_addr = GA10B_BAR0_BASE + 0xC084u;
    uint32_t rl_lo = *(volatile uint32_t *)(uintptr_t)runlist_lo_addr;
    uint32_t rl_hi = *(volatile uint32_t *)(uintptr_t)runlist_hi_addr;

    if ((rl_lo & 0xFFFF0000u) == 0xbadf0000u ||
        (rl_hi & 0xFFFF0000u) == 0xbadf0000u) {
        uart_printf("[ga10b-reserve] runlist submit_base reads "
                    "poison (lo=0x%08x hi=0x%08x) — wrong "
                    "pri_base offset or GPU power-gated. Skipping.\n",
                    (unsigned)rl_lo, (unsigned)rl_hi);
        return;
    }

    /* Decode: lo bits[31:10] are ptr bits[31:10] (1 KB-aligned).
     * Mask off the low 10 bits, those carry target + flags. */
    uint64_t runlist_phys =
        ((uint64_t)(rl_lo & 0xfffffc00u)) |
        ((uint64_t)(rl_hi & 0xffu) << 32);
    uint32_t aperture = rl_lo & 0x3u;

    /* Double-buffered 32 KiB → 64 KiB total. Page-round
     * the size to ensure we cover both buffers even if
     * the base isn't 64 KiB-aligned. */
    const uint64_t runlist_bytes = 64u * 1024u;

    if (!ga10b_phys_in_dram(runlist_phys, (size_t)runlist_bytes)) {
        uart_printf("[ga10b-reserve] runlist phys=0x%lx (size=%llu, "
                    "lo=0x%08x hi=0x%08x ap=%u) outside DRAM — "
                    "skipping reservation. Either the pri_base "
                    "offset is wrong or Linux's nvgpu hadn't "
                    "programmed the runlist yet.\n",
                    (unsigned long)runlist_phys,
                    (unsigned long long)runlist_bytes,
                    (unsigned)rl_lo, (unsigned)rl_hi,
                    (unsigned)aperture);
        return;
    }

    uint64_t runlist_page_base = runlist_phys & ~4095ull;
    uint64_t runlist_page_end =
        (runlist_phys + runlist_bytes + 4095ull) & ~4095ull;
    uint64_t runlist_reserve_bytes =
        runlist_page_end - runlist_page_base;
    if (pmm_user_reserve_add(runlist_page_base,
                              runlist_reserve_bytes) != 0) {
        uart_printf("[ga10b-reserve] pmm_user_reserve_add "
                    "for runlist @0x%lx failed (table full)\n",
                    (unsigned long)runlist_page_base);
    } else {
        uart_printf("[ga10b-reserve] reserved GR runlist "
                    "@0x%lx (%llu B, lo=0x%08x hi=0x%08x ap=%u)\n",
                    (unsigned long)runlist_page_base,
                    (unsigned long long)runlist_reserve_bytes,
                    (unsigned)rl_lo, (unsigned)rl_hi,
                    (unsigned)aperture);
    }
}

/* Reserve the handoff dmabuf page itself — cheap + small (one 4 KB
 * page), but without it PMM can clobber the handoff between boot
 * and the first reader. */
static void reserve_handoff_dmabuf(uint64_t handoff_phys)
{
    if (pmm_user_reserve_add(handoff_phys & ~4095ull, 4096ull) != 0) {
        uart_printf("[ga10b-reserve]   pmm_user_reserve_add for "
                    "handoff page @0x%lx failed (table full?)\n",
                    (unsigned long)handoff_phys);
    } else {
        uart_printf("[ga10b-reserve]   reserved handoff dmabuf "
                    "@0x%lx (4 KB)\n",
                    (unsigned long)(handoff_phys & ~4095ull));
    }
}

/* Reserve every helper-allocated buffer the channel needs
 * (USERD, GPFIFO, pushbuf, sem, SASS pool, cbuf, QMD pool).
 * Without these, SLM-OS PMM can hand them out to its own
 * allocations (e.g. the GMMU rebuild's page-table pages —
 * Stage 4 maps a 1.5 GB weights pool which requires ~3 MB of
 * PTE tables, way more PMM activity than the Stage 3
 * single-page case), and the channel state gets clobbered.
 * Stage 3 worked without these because the PTE-table footprint
 * was tiny (a few pages); Stage 4's larger PTE footprint
 * surfaced the latent collision.
 *
 * Each buffer is small + finite (max ~1 MB for SASS pool);
 * collectively well under the PMM user-reserve table cap.
 * Read the (volatile) handoff fields into locals first so the
 * struct-initializer fits a static helper table cleanly. */
static void reserve_helper_buffers(const volatile struct ga10b_channel_handoff *h)
{
    uint64_t userd_p   = h->userd_phys;
    uint64_t gpfifo_p  = h->gpfifo_phys;
    uint32_t gpfifo_e  = h->gpfifo_entries;
    uint32_t gpfifo_es = h->gpfifo_entry_size;
    uint64_t pushbuf_p = h->pushbuf_phys;
    uint32_t pushbuf_s = h->pushbuf_size;
    uint64_t sem_p     = h->semaphore_phys;
    uint64_t shader_p  = h->shader_phys;
    uint32_t shader_s  = h->shader_size;
    uint64_t cbuf_p    = h->cbuf_phys;
    uint32_t cbuf_s    = h->cbuf_size;
    uint64_t qmd_p     = h->qmd_pool_phys;
    uint32_t qmd_s     = h->qmd_pool_size_bytes;

    struct {
        const char *tag;
        uint64_t phys;
        uint64_t size;
    } refs[] = {
        { "userd",    userd_p,   4096 },
        { "gpfifo",   gpfifo_p,  (uint64_t)gpfifo_e * (uint64_t)gpfifo_es },
        { "pushbuf",  pushbuf_p, (uint64_t)pushbuf_s },
        { "sem",      sem_p,     4096 },
        { "shader",   shader_p,  (uint64_t)shader_s },
        { "cbuf",     cbuf_p,    (uint64_t)cbuf_s },
        { "qmd_pool", qmd_p,     (uint64_t)qmd_s },
    };
    for (size_t i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
        if (refs[i].phys == 0 || refs[i].size == 0) {
            continue;
        }
        uint64_t base = refs[i].phys & ~4095ull;
        uint64_t end  = (refs[i].phys + refs[i].size + 4095ull) &
                        ~4095ull;
        uint64_t sz   = end - base;
        if (pmm_user_reserve_add(base, sz) != 0) {
            uart_printf("[ga10b-reserve]   pmm_user_reserve_add "
                        "for %s @0x%lx failed (table full?)\n",
                        refs[i].tag, (unsigned long)base);
        } else {
            uart_printf("[ga10b-reserve]   reserved %s @0x%lx "
                        "(%llu B)\n",
                        refs[i].tag, (unsigned long)base,
                        (unsigned long long)sz);
        }
    }
}

/* #788 Mode B fix — per-op shader reservation.
 *
 * In v7+ mode the helper allocates each pipeline op's SASS as a
 * separate nvmap dmabuf and publishes only the GPU VA in
 * `pipeline_ops_phys[i].shader_gpu_va`. The single `h->shader_phys`
 * is either 0 (v6/v7 publish path in gpu-launch-common.c) or
 * covers only one shader buffer, so the existing helper-buf
 * reservation block above doesn't protect the other shader pages.
 *
 * Stress-test Mode B (4/10 fails on post-Mode-A-fix run) is
 * empty-SASS at op[0].shader_gpu_va — SLM-OS PMM clobbered the
 * helper's shader pages before launch-kernel could fetch them.
 *
 * Prefers `ops_v7[i].shader_phys` (helper publishes since b2b6c2dc);
 * falls back to GMMU walk via `h->inst_block_phys` for older helpers.
 *
 * Bounded by GA10B_PIPELINE_V7_MAX_OPS — well under
 * pmm_user_reserve_add table cap of 2048. */
static void reserve_per_op_shaders(const volatile struct ga10b_channel_handoff *h)
{
    if (!(h->pipeline_n_ops > 0u && h->pipeline_ops_phys != 0u &&
        h->inst_block_phys != 0u)) {
        uart_printf("[ga10b-reserve]   per-op shader reservation skipped "
                    "(n_ops=%u ops_phys=0x%lx inst_block_phys=0x%lx)\n",
                    (unsigned)h->pipeline_n_ops,
                    (unsigned long)h->pipeline_ops_phys,
                    (unsigned long)h->inst_block_phys);
        return;
    }

    uint64_t ops_phys = h->pipeline_ops_phys;
    uint32_t n_ops    = h->pipeline_n_ops;
    uint64_t inst_phys_for_walk = h->inst_block_phys;

    /* Bound check: cap at GA10B_PIPELINE_V7_MAX_OPS to match the
     * dispatch path's enforced max. A bigger value in the handoff
     * is a corruption signal, not a real op count. */
    if (n_ops > GA10B_PIPELINE_V7_MAX_OPS) {
        uart_printf("[ga10b-reserve]   pipeline_n_ops=%u exceeds "
                    "cap %u — likely corrupt handoff; skipping "
                    "per-op shader reservation\n",
                    (unsigned)n_ops,
                    (unsigned)GA10B_PIPELINE_V7_MAX_OPS);
        return;
    }
    if (!ga10b_phys_in_dram(ops_phys,
                            (size_t)n_ops *
                            sizeof(struct ga10b_pipeline_op_v7))) {
        uart_printf("[ga10b-reserve]   pipeline_ops_phys=0x%lx "
                    "(n=%u) outside DRAM — skipping per-op "
                    "shader reservation\n",
                    (unsigned long)ops_phys, (unsigned)n_ops);
        return;
    }

    /* Reserve the ops array page itself (cheap + small — single
     * 4 KB page for ≤GA10B_PIPELINE_V7_MAX_OPS v7 ops × 80 B).
     * Without this, SLM-OS PMM could clobber the ops array between
     * boot and the launch-kernel command reading it. */
    uint64_t ops_page_base = ops_phys & ~4095ull;
    uint64_t ops_bytes     = (uint64_t)n_ops *
                             sizeof(struct ga10b_pipeline_op_v7);
    uint64_t ops_page_end  =
        (ops_phys + ops_bytes + 4095ull) & ~4095ull;
    if (pmm_user_reserve_add(ops_page_base,
                              ops_page_end - ops_page_base) == 0) {
        uart_printf("[ga10b-reserve]   reserved pipeline_ops "
                    "@0x%lx (%llu B)\n",
                    (unsigned long)ops_page_base,
                    (unsigned long long)(ops_page_end - ops_page_base));
    }

    /* v7.1 path (commit b2b6c2dc+): the helper now publishes
     * shader_phys + shader_size_bytes per op directly. No
     * GMMU walking needed — read the values and reserve.
     *
     * Falls back to GMMU walk for older helpers (shader_phys=0)
     * if h->inst_block_phys is available. The walk path is
     * less reliable because the helper-side FECS read can
     * catch a wrong channel (Xorg / nvgpu-internal) — but
     * better than no per-op reservation at all.
     *
     * Dedup: mnist reuses 4 shaders across 8 ops, dedup by
     * phys. Simple O(n²) linear scan, n ≤ GA10B_PIPELINE_V7_MAX_OPS. */
    const struct ga10b_pipeline_op_v7 *ops_v7 =
        (const struct ga10b_pipeline_op_v7 *)(uintptr_t)ops_phys;
    uint64_t reserved_phys[GA10B_PIPELINE_V7_MAX_OPS];
    uint32_t n_reserved = 0;
    uint32_t n_direct = 0, n_walked = 0, n_walk_ok = 0;
    for (uint32_t i = 0; i < n_ops; i++) {
        uint64_t phys = 0;
        uint64_t size_b = 0;

        /* Prefer the explicit fields. Falls back to walk if
         * helper didn't populate them. */
        if (ops_v7[i].shader_phys != 0 &&
            ops_v7[i].shader_size_bytes > 0) {
            phys   = ops_v7[i].shader_phys;
            size_b = (uint64_t)ops_v7[i].shader_size_bytes;
            n_direct++;
        } else if (ops_v7[i].shader_gpu_va != 0) {
            n_walked++;
            struct ga10b_gmmu_walk_result wr;
            ga10b_gmmu_walk(inst_phys_for_walk,
                            ops_v7[i].shader_gpu_va, &wr);
            if (wr.status != GA10B_GMMU_WALK_OK) continue;
            n_walk_ok++;
            phys   = wr.leaf_phys;
            size_b = 4096u;  /* walk gives one page */
        } else {
            continue;
        }

        uint64_t page_base = phys & ~4095ull;
        uint64_t page_end  = (phys + size_b + 4095ull) & ~4095ull;
        uint64_t reserve_bytes = page_end - page_base;
        if (!ga10b_phys_in_dram(page_base, (size_t)reserve_bytes)) {
            continue;
        }

        /* Dedup */
        bool seen = false;
        for (uint32_t j = 0; j < n_reserved; j++) {
            if (reserved_phys[j] == page_base) {
                seen = true;
                break;
            }
        }
        if (seen) continue;

        if (pmm_user_reserve_add(page_base,
                                 reserve_bytes) != 0) {
            uart_printf("[ga10b-reserve]   per-op shader: "
                        "pmm_user_reserve_add(0x%lx, %llu) failed — "
                        "table full, op %u/%u uncovered\n",
                        (unsigned long)page_base,
                        (unsigned long long)reserve_bytes,
                        (unsigned)i, (unsigned)n_ops);
            break;
        }
        reserved_phys[n_reserved++] = page_base;
    }
    uart_printf("[ga10b-reserve]   per-op shader (v7.1): "
                "%u direct, %u walked (%u ok), %u unique "
                "reservations\n",
                (unsigned)n_direct, (unsigned)n_walked,
                (unsigned)n_walk_ok, (unsigned)n_reserved);
}

/* v10 (#788 Mode C fix): per-channel GR context buffer reservation.
 *
 * The helper reads /sys/kernel/debug/gpu.0/fifo/slmos_gr_ctx_phys
 * (exposed by the patched nvgpu.ko — see file header) and publishes
 * every active channel's GR context buffer phys ranges in an
 * extents array. Each entry covers one channel-internal GR buffer
 * (main ctx, patch ctx, etc.). Without this, SLM-OS PMM clobbers
 * the buffers and FECS hits mb6=0x5a on context-load. */
static void reserve_gr_ctx_extents(const volatile struct ga10b_channel_handoff *h)
{
    uint64_t gr_ctx_extents_phys = h->gr_ctx_extents_phys;
    uint32_t gr_ctx_n_extents    = h->gr_ctx_n_extents;
    if (gr_ctx_extents_phys == 0 || gr_ctx_n_extents == 0) {
        uart_printf("[ga10b-reserve]   gr_ctx reservation skipped "
                    "(extents_phys=0x%lx n=%u — helper didn't publish, "
                    "older or non-patched nvgpu)\n",
                    (unsigned long)gr_ctx_extents_phys,
                    (unsigned)gr_ctx_n_extents);
        return;
    }

    uint64_t gr_extents_bytes =
        (uint64_t)gr_ctx_n_extents *
        sizeof(struct ga10b_phys_extent);
    if (!ga10b_phys_in_dram(gr_ctx_extents_phys,
                            (size_t)gr_extents_bytes)) {
        uart_printf("[ga10b-reserve]   gr_ctx_extents_phys=0x%lx "
                    "(n=%u) outside DRAM — skipping gr_ctx "
                    "reservation\n",
                    (unsigned long)gr_ctx_extents_phys,
                    (unsigned)gr_ctx_n_extents);
        return;
    }

    /* Reserve the extents-array dmabuf itself */
    uint64_t arr_page_base = gr_ctx_extents_phys & ~4095ull;
    uint64_t arr_page_end  =
        (gr_ctx_extents_phys + gr_extents_bytes + 4095ull) &
        ~4095ull;
    if (pmm_user_reserve_add(arr_page_base,
                              arr_page_end - arr_page_base) == 0) {
        uart_printf("[ga10b-reserve]   reserved gr_ctx_extents "
                    "array @0x%lx\n",
                    (unsigned long)arr_page_base);
    }

    /* Reserve every entry. */
    const struct ga10b_phys_extent *gr_exts =
        (const struct ga10b_phys_extent *)(uintptr_t)
        gr_ctx_extents_phys;
    int gr_reserved = 0, gr_skipped = 0;
    uint64_t gr_total_bytes = 0;
    for (uint32_t i = 0; i < gr_ctx_n_extents; i++) {
        uint64_t ph = gr_exts[i].phys;
        uint32_t np = gr_exts[i].n_pages;
        if (np == 0u) continue;
        uint64_t sz = (uint64_t)np * 4096ull;
        if (!ga10b_phys_in_dram(ph, sz)) {
            gr_skipped++;
            continue;
        }
        if (pmm_user_reserve_add(ph, sz) != 0) {
            uart_printf("[ga10b-reserve]   gr_ctx reserve "
                        "table full at extent %u/%u\n",
                        (unsigned)i, (unsigned)gr_ctx_n_extents);
            break;
        }
        gr_reserved++;
        gr_total_bytes += sz;
    }
    uart_printf("[ga10b-reserve]   gr_ctx: %u extents reserved "
                "(%llu KB total), %u skipped\n",
                (unsigned)gr_reserved,
                (unsigned long long)(gr_total_bytes / 1024ull),
                (unsigned)gr_skipped);
}

/* #788 Stage 4 — reserve every weights-pool extent the handoff lists.
 *
 * The IOVMM-stitched 1.5 GB weights pool is physically scattered
 * across hundreds-to-thousands of contiguous runs; without
 * reserving each run's pages, SLM-OS PMM allocates from those
 * pages and the CE memcpy writes during W3 staging clobber
 * whatever kernel data SLM-OS put there.
 *
 * Also reserves the extents dmabuf itself (can be 100s of KB for
 * a fragmented IOVMM allocation; up to 128 KB at
 * GA10B_WEIGHTS_EXTENTS_MAX × 16 B). Without that, SLM-OS PMM
 * easily hands those pages to ramdisk allocations or model
 * preload buffers, and by rebuild-time the extents array reads
 * as random bytes.
 *
 * `handoff_phys_for_log` is the dmabuf phys carried through only
 * for the trailing log line — no functional dependence. */
static void reserve_weights_extents(const volatile struct ga10b_channel_handoff *h,
                                    uint64_t handoff_phys_for_log)
{
    uint32_t version = h->version;
    uint32_t n_extents = h->weights_n_extents;
    uint64_t extents_phys = h->weights_extents_phys;

    if (version < 9u || n_extents == 0u || extents_phys == 0u) {
        uart_printf("[ga10b-reserve]   handoff@0x%lx v=%u no v9 "
                    "extents (n=%u extents_phys=0x%lx) — skipping\n",
                    (unsigned long)handoff_phys_for_log, (unsigned)version,
                    (unsigned)n_extents, (unsigned long)extents_phys);
        return;
    }
    uint64_t extents_bytes =
        (uint64_t)n_extents * sizeof(struct ga10b_phys_extent);
    if (!ga10b_phys_in_dram(extents_phys, (size_t)extents_bytes)) {
        uart_printf("[ga10b-reserve]   extents_phys=0x%lx (n=%u) "
                    "outside DRAM — skipping\n",
                    (unsigned long)extents_phys, (unsigned)n_extents);
        return;
    }

    /* Reserve the extents dmabuf itself first so the array doesn't
     * get clobbered between this boot stage and the rebuild path
     * that reads it later. */
    uint64_t extents_page_base = extents_phys & ~4095ull;
    uint64_t extents_page_end =
        (extents_phys + extents_bytes + 4095ull) & ~4095ull;
    uint64_t extents_reserve_bytes =
        extents_page_end - extents_page_base;
    if (pmm_user_reserve_add(extents_page_base,
                              extents_reserve_bytes) != 0) {
        uart_printf("[ga10b-reserve]   pmm_user_reserve_add for "
                    "extents dmabuf failed — array will be at "
                    "risk of clobber between boot and rebuild\n");
    } else {
        uart_printf("[ga10b-reserve]   reserved extents dmabuf "
                    "@0x%lx (%llu B)\n",
                    (unsigned long)extents_page_base,
                    (unsigned long long)extents_reserve_bytes);
    }

    const struct ga10b_phys_extent *exts =
        (const struct ga10b_phys_extent *)(uintptr_t)extents_phys;
    int reserved_count = 0;
    int skipped_count = 0;
    uint64_t total_pages = 0;
    for (uint32_t i = 0; i < n_extents; i++) {
        uint64_t ph = exts[i].phys;
        uint32_t np = exts[i].n_pages;
        if (np == 0u) {
            continue;
        }
        uint64_t sz = (uint64_t)np * 4096ull;
        if (!ga10b_phys_in_dram(ph, sz)) {
            skipped_count++;
            continue;
        }
        if (pmm_user_reserve_add(ph, sz) != 0) {
            uart_printf("[ga10b-reserve]   pmm_user_reserve_add "
                        "failed at extent %u/%u — table full, "
                        "tail unprotected\n",
                        (unsigned)i, (unsigned)n_extents);
            break;
        }
        reserved_count++;
        total_pages += np;
    }
    uart_printf("[ga10b-reserve]   weights extents: %u reserved "
                "(%llu pages, ~%llu MB), %u skipped (outside DRAM), "
                "from handoff@0x%lx\n",
                (unsigned)reserved_count,
                (unsigned long long)total_pages,
                (unsigned long long)(total_pages / 256ull),
                (unsigned)skipped_count,
                (unsigned long)handoff_phys_for_log);
}

/* ---------------------------------------------------------------
 * Orchestrator.
 * --------------------------------------------------------------- */

void ga10b_kexec_handoff_register_reserves(void)
{
    /* Always-on phase: read FECS_CURRENT_CTX and reserve the
     * channel's inst block. If this skip-returns (poison MMIO,
     * target=0 fresh boot, outside-DRAM, PMM table full), the rest
     * of the handoff machinery is meaningless — no kexec'd channel
     * to protect. */
    if (!reserve_inst_block()) {
        return;
    }

    reserve_runlist();

    /* Handoff-dependent phase: locate the helper-published handoff
     * dmabuf in DRAM by scanning for its magic. ~50 ms one-time-only
     * boot cost. A fresh-boot or no-helper test path that doesn't
     * have a kexec'd channel works fine without these reservations —
     * just leaves PMM with more pages to allocate from.
     *
     * #834: use the kind-aware + validate-filtered scanner here
     * (NOT the bare magic-match `ga10b_find_handoff_in_range`).
     * Stale handoff dmabufs from prior helper invocations remain in
     * DRAM with intact magic but inst_block_phys=0 (the helper's
     * fallback when gpu_read_fecs_inst_block_phys() fails). With the
     * unfiltered scanner, this reserve picked up the stale handoff
     * — at a lower phys than the fresh one — and pinned its
     * userd/gpfifo/pushbuf/sem buffers, which point at *old*
     * channels long-since freed. The FRESH channel's buffers, which
     * the helper actually used and which the GPU writes to, were
     * left unreserved → PMM clobbered them on first allocation →
     * silent corruption + boot-to-boot dispatch flakiness +
     * eventual PMM-free-list page faults from GPU writes into
     * PMM-owned pages. Observed on jetson-nano-2 (2026-05-16):
     *   stale 0x12c097000: userd=0x13ae13000 gpfifo=0x12c034000
     *                       pushbuf=0x13c4a0000 sem=0x129e2b000
     *   fresh 0x13aee2000: userd=0x10d706000 gpfifo=0x13accd000
     *                       pushbuf=0x13a9a0000 sem=0x13af83000
     * All four buffers different — boot reserve pinned the wrong
     * set entirely. Switching to the kind-aware scanner cascades
     * the validate-handoff inst_block_phys=0 rejection (commit
     * a099cd3b) into this code path too. */
    uart_puts("[ga10b-reserve] scanning DRAM for handoff magic + "
              "weights-pool extents...\n");
    uint64_t handoff_phys =
        ga10b_find_handoff_of_kind_in_range(0x100000000ull,
                                             0x200000000ull,
                                             4096ull,
                                             GA10B_PIPELINE_KIND_MNIST);
    if (handoff_phys == 0) {
        uart_puts("[ga10b-reserve]   handoff not found — skipping "
                  "weights-pool extent reservation\n");
        return;
    }

    reserve_handoff_dmabuf(handoff_phys);

    const volatile struct ga10b_channel_handoff *h =
        (const volatile struct ga10b_channel_handoff *)
        (uintptr_t)handoff_phys;

    reserve_helper_buffers(h);
    reserve_per_op_shaders(h);
    reserve_gr_ctx_extents(h);
    reserve_weights_extents(h, handoff_phys);
}

uint64_t ga10b_kexec_inherited_inst_block_phys(void)
{
    return g_boot_inst_block_phys;
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

/* No-op stub for non-Jetson platforms. The header's documentation
 * notes the function is safe to call from a platform-guarded call
 * site; this empty body lets non-Jetson builds still link if a
 * future caller forgets the `#if`. Compiler will inline-eliminate
 * the call entirely at LTO time. */
void ga10b_kexec_handoff_register_reserves(void)
{
}

/* Non-Jetson stub: no kexec channel-inheritance on this platform,
 * so the inherited inst-block is meaningless. Return 0 to signal
 * "no boot capture" — callers must already handle that case (the
 * Jetson implementation also returns 0 on fresh boots / poisoned
 * FECS reads). */
uint64_t ga10b_kexec_inherited_inst_block_phys(void)
{
    return 0;
}

#endif
