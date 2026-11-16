/*
 * dtb_x86_stub.c - x86-64 stubs for dtb_* symbols.
 *
 * The full DTB parser (`kernel/src/dtb.c`) is ARM64-only — x86-64 boots
 * via Multiboot2 / Linux kexec and gets memory + boot info from the GRUB
 * info struct or the bzImage parameter block, not from a flattened
 * device tree.
 *
 * Three helpers are still referenced from platform-neutral code:
 *
 *   - `dtb_get_memreserves` (called from `pmm_collect_reserves` to
 *     carve firmware-reserved ranges out of new regions)
 *   - `dtb_get_blob_range` (called from `pmm_collect_reserves` and
 *     `pmm_init` to reserve the DTB blob itself and check it, #1244)
 *   - `dtb_get_chosen` (called from `kernel_main` to seed lwip's RNG
 *     with `/chosen` entropy)
 *
 * On x86-64 there is no DTB and no /chosen node, so every stub reports
 * "no entries" (or no blob) and the platform-neutral callers fall
 * through their "no firmware reserves / no blob / no entropy" paths
 * cleanly.
 */

#include "dtb.h"

int dtb_get_memreserves(dtb_memreserve_t *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

bool dtb_get_blob_range(uint64_t *addr, uint64_t *size)
{
    (void)addr;
    (void)size;
    return false;       /* no DTB on x86-64 */
}

const dtb_chosen_t *dtb_get_chosen(void)
{
    static const dtb_chosen_t empty = { 0 };
    return &empty;
}
