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

void ga10b_kexec_handoff_register_reserves(void)
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
        return;
    }

    uint32_t target = (reg >> GA10B_FECS_CTX_TARGET_SHIFT) &
                      GA10B_FECS_CTX_TARGET_MASK;
    if (target == 0u) {
        uart_printf("[ga10b-reserve] FECS_CURRENT_CTX=0x%08x (target=0 "
                    "— no current ctx, or fresh boot). Skipping.\n",
                    (unsigned)reg);
        return;
    }

    uint64_t inst_phys =
        ((uint64_t)(reg & GA10B_FECS_CTX_PTR_MASK)) << 12;

    if (!ga10b_phys_in_dram(inst_phys, GA10B_INST_BLOCK_BYTES)) {
        uart_printf("[ga10b-reserve] decoded inst_phys=0x%lx outside "
                    "DRAM (FECS_CURRENT_CTX=0x%08x, target=%u). "
                    "Skipping reservation.\n",
                    (unsigned long)inst_phys, (unsigned)reg,
                    (unsigned)target);
        return;
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
        return;
    }

    uart_printf("[ga10b-reserve] reserved kexec'd channel inst block "
                "at phys=0x%lx (size=%u, FECS_CURRENT_CTX=0x%08x, "
                "target=%u)\n",
                (unsigned long)inst_phys,
                (unsigned)GA10B_INST_BLOCK_BYTES,
                (unsigned)reg, (unsigned)target);

    /* #788 Stage 7: read the GR engine's runlist submit_base register
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
        } else {
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
            } else {
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
        }
    }

    /* #788 Stage 4 — scan DRAM for the handoff and reserve every
     * weights-pool extent it lists. The IOVMM-stitched 1.5 GB
     * weights pool is physically scattered across hundreds-to-
     * thousands of contiguous runs; without reserving each run's
     * pages, SLM-OS PMM allocates from those pages and the CE
     * memcpy writes during W3 staging clobber whatever kernel
     * data SLM-OS put there.
     *
     * The handoff itself lives somewhere in the 4-8 GB DRAM
     * range (the helper's nvmap allocation lands wherever the
     * IOVMM heap has room). Scanning at 4 KB stride is ~50 ms
     * one-time-only at boot; acceptable cost for the protection.
     *
     * If the scan fails (no handoff found) we still finished the
     * inst-block reservation above — a fresh-boot or no-helper
     * test path that doesn't have a kexec'd channel works
     * normally without the extents protection. */
    uart_puts("[ga10b-reserve] scanning DRAM for handoff magic + "
              "weights-pool extents...\n");
    uint64_t handoff_phys =
        ga10b_find_handoff_in_range(0x100000000ull, 0x200000000ull,
                                     4096ull);
    if (handoff_phys == 0) {
        uart_puts("[ga10b-reserve]   handoff not found — skipping "
                  "weights-pool extent reservation\n");
        return;
    }

    /* Reserve the handoff dmabuf page itself first (cheap + small). */
    if (pmm_user_reserve_add(handoff_phys & ~4095ull, 4096ull) != 0) {
        uart_printf("[ga10b-reserve]   pmm_user_reserve_add for "
                    "handoff page @0x%lx failed (table full?)\n",
                    (unsigned long)handoff_phys);
    } else {
        uart_printf("[ga10b-reserve]   reserved handoff dmabuf "
                    "@0x%lx (4 KB)\n",
                    (unsigned long)(handoff_phys & ~4095ull));
    }
    const volatile struct ga10b_channel_handoff *h =
        (const volatile struct ga10b_channel_handoff *)
        (uintptr_t)handoff_phys;
    uint32_t version = h->version;
    uint32_t n_extents = h->weights_n_extents;
    uint64_t extents_phys = h->weights_extents_phys;

#if 1  /* Stage 5 bisect B: only USERD + GPFIFO reserved */
#define BISECT_B_USERD_GPFIFO_ONLY 1
#else
#define BISECT_B_USERD_GPFIFO_ONLY 0
#endif

#if BISECT_B_USERD_GPFIFO_ONLY  /* Stage 5 bisect B: only USERD + GPFIFO */
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
            /* Stage 7: full helper-buf reservation. Stage 4 was
             * broken by pushbuf/sem reservation (bisect E/F);
             * Stage 7 adds the runlist reservation above which
             * should make the rebuild's PT-page allocations safe. */
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
#else
    uart_puts("[ga10b-reserve]   STAGE5-BISECT: helper-buffer reservations SKIPPED\n");
#endif
    if (version < 9u || n_extents == 0u || extents_phys == 0u) {
        uart_printf("[ga10b-reserve]   handoff@0x%lx v=%u no v9 "
                    "extents (n=%u extents_phys=0x%lx) — skipping\n",
                    (unsigned long)handoff_phys, (unsigned)version,
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

    /* Reserve the extents dmabuf itself so PMM doesn't allocate
     * from it before the rebuild path reads it. The dmabuf can
     * be 100s of KB for a fragmented IOVMM allocation (1033
     * extents × 16 B = 16 KB; capped at GA10B_WEIGHTS_EXTENTS_MAX
     * × 16 B = 128 KB), much larger than the inst-block page or
     * handoff dmabuf — without explicit reservation, SLM-OS PMM
     * easily hands those pages to ramdisk allocations or model
     * preload buffers, and by rebuild-time the extents array
     * reads as random bytes. */
    {
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
                (unsigned long)handoff_phys);
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

#endif
