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

#include "ga10b_gmmu.h"
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
