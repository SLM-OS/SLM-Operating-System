/*
 * efi_stub.c - Minimal EFI boot stub for SLM-OS
 *
 * Handles the EFI boot protocol when SLM-OS is launched directly by UEFI
 * as a PE/COFF EFI application. Extracts the DTB from the EFI configuration
 * table, calls ExitBootServices to take over the machine, then disables the
 * MMU and returns the DTB pointer to boot.S.
 *
 * This code runs on UEFI's stack before ExitBootServices. After
 * ExitBootServices, the UEFI stack memory may be freed but remains
 * physically intact (no concurrent writers with interrupts masked).
 *
 * Entry: called from boot.S with x0=EFI_HANDLE, x1=EFI_SYSTEM_TABLE*
 * Exit:  returns DTB pointer (or NULL), MMU disabled, caches cleaned
 */

#include "efi.h"
#include <stddef.h>

/* Forward declaration for MMU disable (assembly in boot.S or mmu.S) */
static void efi_disable_mmu(void);

/*
 * Find the FDT (Device Tree) in the EFI configuration table.
 */
static void *efi_find_fdt(efi_system_table_t *sys_table)
{
    efi_guid_t fdt_guid = EFI_FDT_GUID;
    efi_config_table_t *tables = sys_table->config_table;
    uint64_t nr = sys_table->nr_tables;

    for (uint64_t i = 0; i < nr; i++) {
        if (efi_guid_eq(&tables[i].vendor_guid, &fdt_guid)) {
            return tables[i].vendor_table;
        }
    }
    return NULL;
}

/*
 * Call ExitBootServices with retry.
 *
 * ExitBootServices requires the map_key from the most recent GetMemoryMap.
 * If the memory map changes between the two calls (e.g., due to the
 * GetMemoryMap allocation itself), ExitBootServices returns
 * EFI_INVALID_PARAMETER. In that case, re-call GetMemoryMap into the
 * existing buffer (no new allocations!) and retry once.
 */
static efi_status_t efi_exit_boot(efi_handle_t handle,
                                   efi_boot_services_t *bs)
{
    efi_status_t status;
    uint64_t map_size = 0;
    uint64_t map_key = 0;
    uint64_t desc_size = 0;
    uint32_t desc_version = 0;
    efi_memory_desc_t *map = NULL;

    /* First call: get required buffer size */
    status = bs->get_memory_map(&map_size, NULL, &map_key,
                                &desc_size, &desc_version);
    /* Expected: EFI_BUFFER_TOO_SMALL, map_size now has required size */

    /* Add slack for the AllocatePool call itself changing the map */
    map_size += desc_size * 8;

    /* Allocate buffer */
    status = bs->allocate_pool(EFI_LOADER_DATA, map_size, (void **)&map);
    if (status != EFI_SUCCESS) {
        return status;
    }

    /* Second call: populate the buffer */
    status = bs->get_memory_map(&map_size, map, &map_key,
                                &desc_size, &desc_version);
    if (status != EFI_SUCCESS) {
        bs->free_pool(map);
        return status;
    }

    /* Exit boot services */
    status = bs->exit_boot_services(handle, map_key);

    if (status != EFI_SUCCESS) {
        /*
         * Map key was stale. Re-call GetMemoryMap into the SAME buffer
         * (no new allocations allowed after failed ExitBootServices).
         */
        status = bs->get_memory_map(&map_size, map, &map_key,
                                    &desc_size, &desc_version);
        if (status != EFI_SUCCESS) {
            return status;
        }

        status = bs->exit_boot_services(handle, map_key);
    }

    /* After ExitBootServices: boot services are gone, we own the machine.
     * Don't call free_pool or any other boot service from here. */
    return status;
}

/*
 * Disable MMU and clean caches.
 *
 * After ExitBootServices, UEFI's identity-mapped page tables are still
 * active. SLM-OS boot code expects MMU off. Disable it here so the
 * normal boot path works unchanged.
 */
static void efi_disable_mmu(void)
{
    /* Clean and invalidate all data caches */
    __asm__ volatile(
        "dsb    sy\n"
        "mrs    x0, clidr_el1\n"
        "and    x3, x0, #0x7000000\n"   /* Get LoC (Level of Coherence) */
        "lsr    x3, x3, #23\n"          /* Cache level value */
        "cbz    x3, 5f\n"               /* If 0, no caches */
        "mov    x10, #0\n"              /* Start with level 0 */
    "1:\n"
        "add    x2, x10, x10, lsr #1\n" /* x2 = 3*level */
        "lsr    x1, x0, x2\n"
        "and    x1, x1, #7\n"           /* Cache type for this level */
        "cmp    x1, #2\n"
        "b.lt   4f\n"                   /* Skip if no data cache */
        "msr    csselr_el1, x10\n"      /* Select this cache level */
        "isb\n"
        "mrs    x1, ccsidr_el1\n"       /* Read cache size ID */
        "and    x2, x1, #7\n"          /* Line size field */
        "add    x2, x2, #4\n"          /* log2(line size in bytes) */
        "ubfx   x4, x1, #3, #10\n"    /* Max way number */
        "clz    w5, w4\n"              /* Bit position of way */
        "ubfx   x7, x1, #13, #15\n"   /* Max set number */
    "2:\n"
        "mov    x9, x4\n"              /* Working copy of max way */
    "3:\n"
        "lsl    x6, x9, x5\n"
        "orr    x11, x10, x6\n"        /* Combine set, way, level */
        "lsl    x6, x7, x2\n"
        "orr    x11, x11, x6\n"
        "dc     cisw, x11\n"           /* Clean & invalidate by set/way */
        "subs   x9, x9, #1\n"
        "b.ge   3b\n"
        "subs   x7, x7, #1\n"
        "b.ge   2b\n"
    "4:\n"
        "add    x10, x10, #2\n"        /* Next cache level */
        "cmp    x10, x3\n"
        "b.lt   1b\n"
    "5:\n"
        "dsb    sy\n"
        "isb\n"
        ::: "x0", "x1", "x2", "x3", "x4", "x5", "x6",
            "x7", "x9", "x10", "x11", "memory"
    );

    /* Invalidate instruction cache */
    __asm__ volatile(
        "ic     ialluis\n"
        "dsb    ish\n"
        "isb\n"
        ::: "memory"
    );

    /* Disable MMU (read SCTLR, clear M/C/I bits, write back)
     * At EL2: use sctlr_el2 directly (not VHE-aliased sctlr_el1,
     * since VHE isn't enabled yet at this point). */
    __asm__ volatile(
        "mrs    x0, sctlr_el2\n"
        "bic    x0, x0, #(1 << 0)\n"   /* M: MMU enable */
        "bic    x0, x0, #(1 << 2)\n"   /* C: Data cache enable */
        "bic    x0, x0, #(1 << 12)\n"  /* I: Instruction cache enable */
        "msr    sctlr_el2, x0\n"
        "isb\n"
        ::: "x0", "memory"
    );

    /* Invalidate TLBs */
    __asm__ volatile(
        "tlbi   alle2\n"
        "dsb    nsh\n"
        "isb\n"
        ::: "memory"
    );
}

/*
 * EFI stub entry point.
 *
 * Called from boot.S when EFI boot is detected.
 * Returns the DTB physical address (or NULL if not found).
 * On return, MMU is disabled and caches are cleaned.
 */
void *efi_stub_entry(efi_handle_t handle, efi_system_table_t *sys_table)
{
    void *fdt = NULL;
    efi_status_t status;

    /* Find DTB in configuration table (must be done before ExitBootServices) */
    fdt = efi_find_fdt(sys_table);

    /* Exit boot services — takes over the machine */
    status = efi_exit_boot(handle, sys_table->boot_services);

    if (status != EFI_SUCCESS) {
        /* ExitBootServices failed. Can't print (no UART yet).
         * Return NULL to signal failure to boot.S. */
        return NULL;
    }

    /* Disable MMU and clean caches for the normal boot path */
    efi_disable_mmu();

    return fdt;
}
