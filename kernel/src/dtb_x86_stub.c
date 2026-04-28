/*
 * dtb_x86_stub.c - x86-64 stubs for dtb_* symbols.
 *
 * The full DTB parser (`kernel/src/dtb.c`) is ARM64-only — x86-64 boots
 * via Multiboot2 / Linux kexec and gets memory + boot info from the GRUB
 * info struct or the bzImage parameter block, not from a flattened
 * device tree.
 *
 * Two helpers are still referenced from platform-neutral code:
 *
 *   - `dtb_get_memreserves` (called from `pmm_add_region_split` to
 *     carve firmware-reserved ranges out of new regions)
 *   - `dtb_get_chosen` (called from `kernel_main` to seed lwip's RNG
 *     with `/chosen` entropy)
 *
 * On x86-64 there is no DTB and no /chosen node, so both stubs report
 * "no entries" and the platform-neutral callers fall through their
 * "no firmware reserves / no entropy" paths cleanly.
 */

#include "dtb.h"

int dtb_get_memreserves(dtb_memreserve_t *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

const dtb_chosen_t *dtb_get_chosen(void)
{
    static const dtb_chosen_t empty = { 0 };
    return &empty;
}
