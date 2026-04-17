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
 * Trace helper — prints a UTF-16 literal via UEFI's ConOut protocol.
 *
 * Only usable before ExitBootServices (per UEFI spec §7.4.1, all Boot
 * Services including ConOut become invalid after EBS succeeds). Compiled
 * in unconditionally — the four or five call sites cost ~80 bytes of
 * .rodata and a handful of indirect calls, well inside the ~1 MB image
 * budget, and give the only viable post-deploy visibility into where the
 * stub progresses before the machine is handed over.
 *
 * UEFI expects CHAR16 strings (UTF-16). String literals use `u"..."`.
 * Each line terminates with "\r\n" since UEFI's text console treats
 * "\n" alone as line-feed without carriage return, producing the
 * staircase effect seen in some early bring-ups.
 */
static void efi_print(efi_system_table_t *sys_table, const efi_char16_t *str)
{
    if (sys_table == NULL || sys_table->con_out == NULL) {
        return;
    }
    if (sys_table->con_out->output_string == NULL) {
        return;
    }
    /*
     * Cast away const on the string pointer: UEFI's EFI_TEXT_STRING
     * prototype is non-const even though the call is read-only (the
     * spec treats String as IN). Safe in practice; the C-side const
     * discipline is preserved on the caller above.
     */
    sys_table->con_out->output_string(sys_table->con_out,
                                      (efi_char16_t *)str);
}

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
         *
         * NOTE: cannot TRACE() from here — `efi_exit_boot` doesn't have
         * sys_table in scope; the caller will observe the retry branch
         * through the status this function returns.
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

    /* Disable MMU (read SCTLR, clear M/C/I bits, write back).
     *
     * Use sctlr_el1 (not sctlr_el2): when UEFI's VHE is active
     * (E2H=1, TGE=1), sctlr_el1 is aliased to SCTLR_EL2. Using the
     * _el1 form works correctly both with and without VHE.
     * Direct sctlr_el2 access faults when VHE is active because
     * UEFI's exception vectors trap it. */
    __asm__ volatile(
        "mrs    x0, sctlr_el1\n"
        "bic    x0, x0, #(1 << 0)\n"   /* M: MMU enable */
        "bic    x0, x0, #(1 << 2)\n"   /* C: Data cache enable */
        "bic    x0, x0, #(1 << 12)\n"  /* I: Instruction cache enable */
        "msr    sctlr_el1, x0\n"
        "isb\n"
        ::: "x0", "memory"
    );

    /* Invalidate TLBs.
     * tlbi vmalle1 is the VHE-compatible form: with VHE it invalidates
     * all EL2 TLB entries; without VHE it invalidates EL1 entries. */
    __asm__ volatile(
        "tlbi   vmalle1\n"
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

    /*
     * Trace markers — visible on UEFI's console (and therefore on the
     * Jetson USB-C serial via TCU) because UEFI's ConOut is still alive
     * here. Each marker is a distinct letter so the last one observed
     * on the wire localizes the failure without needing a debugger.
     * See docs/jetson-uefi-direct-result.md §"Key findings" for why
     * direct UARTC MMIO isn't a viable tracing path from this context.
     */
    static const efi_char16_t m_entry[]     = u"[slmos] A efi_entry\r\n";
    static const efi_char16_t m_find_done[] = u"[slmos] B find_fdt done\r\n";
    static const efi_char16_t m_pre_ebs[]   = u"[slmos] C calling ExitBootServices\r\n";
    static const efi_char16_t m_ebs_fail[]  = u"[slmos] ! ExitBootServices failed\r\n";
    static const efi_char16_t m_post_ebs[]  = u"[slmos] D ExitBootServices returned\r\n";
    static const efi_char16_t m_pre_mmu[]   = u"[slmos] E calling disable_mmu\r\n";
    static const efi_char16_t m_pre_ret[]   = u"[slmos] F returning fdt\r\n";

    efi_print(sys_table, m_entry);

    /* Find DTB in configuration table (must be done before ExitBootServices) */
    fdt = efi_find_fdt(sys_table);
    efi_print(sys_table, m_find_done);

    /* Exit boot services — takes over the machine.
     *
     * Per UEFI §7.4.1, Boot Services (including ConOut) become invalid
     * after EBS succeeds. Still, the post-EBS markers D/E/F below test
     * that premise: if the underlying UART driver is a plain MMIO loop
     * rather than a protocol service, writes may keep landing on the
     * serial wire even after EBS. Either outcome is information —
     * markers appearing narrow the crash window; silence indicates
     * the protocol teardown is real on this firmware.
     */
    efi_print(sys_table, m_pre_ebs);
    status = efi_exit_boot(handle, sys_table->boot_services);

    if (status != EFI_SUCCESS) {
        efi_print(sys_table, m_ebs_fail);
        return NULL;
    }

#if defined(PLATFORM_JETSON_ORIN_NANO)
    /*
     * Take ownership of the EL2 exception vectors IMMEDIATELY after
     * ExitBootServices returns, before any other post-EBS work runs.
     *
     * Until this runs, VBAR_EL2 still points at ArmCpuDxe's vector
     * table, which depends on Boot Services state that the EBS just
     * tore down. Any synchronous fault in the code that follows
     * (post-EBS efi_prints, efi_disable_mmu, the return to boot.S,
     * the Jetson EL2 block's HCR_EL2 write, UARTC probe) would
     * bounce to that now-invalid handler and hang silently — the
     * historical failure mode tracked in
     * docs/jetson-uefi-direct-result.md §5c.
     *
     * `jetson_early_vbar_el2` is the SLM-OS-owned diagnostic table
     * defined at the end of boot.S. It saves ESR/ELR/FAR/SPSR/HCR to
     * `jetson_early_fault_slot` in BSS and emits "!FAULT\r\n" over
     * UARTC. The declared size [2048] matches the 16 × 0x80 ARM64
     * vector-table layout so the compiler can catch a stray
     * out-of-range indexing in any future C caller.
     *
     * Only runs from EL2 — CurrentEL-gated because reading or
     * writing VBAR_EL2 from EL1 would trap. If firmware drops us to
     * EL1 (unexpected on Jetson but possible elsewhere), print a
     * warning so the silent-skip doesn't look like a successful
     * install.
     *
     * Kexec path doesn't pass through here (it jumps straight to
     * real_start with x1 = 0), so this change is UEFI-direct only.
     */
    {
        extern uint8_t jetson_early_vbar_el2[2048];
        static const efi_char16_t m_vbar_not_el2[] =
            u"[slmos] W not at EL2, VBAR_EL2 not installed\r\n";
        uint64_t cur_el;
        __asm__ volatile("mrs %0, CurrentEL" : "=r"(cur_el));
        if (cur_el == 8) {
            __asm__ volatile(
                "msr vbar_el2, %0\n"
                "isb\n"
                :
                : "r"(jetson_early_vbar_el2)
                : "memory");
        } else {
            /* ConOut may be torn down post-EBS (see comment block
             * below), but if it still works on this firmware the
             * warning reaches the serial console; if not, the
             * absence of a subsequent `!FAULT` from the handler is
             * itself a signal. */
            efi_print(sys_table, m_vbar_not_el2);
        }
    }
#endif

    /*
     * POST-EBS efi_print calls below are firmware-dependent.
     *
     * The ConOut protocol is formally invalid here (UEFI §7.4.1). On
     * Jetson firmware v36.4.7 the protocol struct happens to still
     * have stable vtable pointers — output_string survives the EBS
     * teardown and D/E/F land on serial as no-ops (they also survive
     * being called with MMU on/off). On other firmware the struct
     * may be freed, zeroed, or left with dangling function pointers,
     * in which case efi_print's NULL guards don't help — a non-NULL
     * garbage `output_string` dereference would synchronously fault.
     * On Jetson, the VBAR_EL2 swap above catches that fault; on
     * other ARM64 UEFI targets without the swap, the fault would
     * still go to UEFI's torn-down vector.
     */
    efi_print(sys_table, m_post_ebs);

    /* Disable MMU and clean caches for the normal boot path */
    efi_print(sys_table, m_pre_mmu);
    efi_disable_mmu();

    efi_print(sys_table, m_pre_ret);
    return fdt;
}
