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
 * Format a uint64_t as 16-hex-digit UTF-16 ("0x0123456789ABCDEF\r\n")
 * and print it via ConOut. Used pre-EBS to dump system-register values
 * for diagnostic purposes — the console is still alive here and this
 * is the only portable way to get register contents off the machine
 * without a working post-EBS output path.
 *
 * `label` is a UTF-16 literal printed before the hex value. Total
 * output: "<label>0x................\r\n".
 */
static void efi_print_hex(efi_system_table_t *sys_table,
                          const efi_char16_t *label,
                          uint64_t value)
{
    efi_char16_t buf[22];   /* "0x" + 16 hex digits + "\r\n" + NUL */
    static const efi_char16_t hex_digits[] = u"0123456789ABCDEF";

    efi_print(sys_table, label);

    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned nibble = (value >> (60 - 4 * i)) & 0xF;
        buf[2 + i] = hex_digits[nibble];
    }
    buf[18] = '\r';
    buf[19] = '\n';
    buf[20] = 0;

    efi_print(sys_table, buf);
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
     * SCTLR to write depends on the EL + VHE state we're running
     * under. Three regimes matter:
     *
     *   (a) EL1 (no EL2 involvement):
     *         active MMU control = SCTLR_EL1.
     *         TLB  = tlbi vmalle1.
     *   (b) EL2 with VHE on (HCR_EL2.E2H=1, e.g. kexec-from-Linux
     *       where Linux already configured VHE):
     *         SCTLR_EL1 is architecturally aliased to SCTLR_EL2.
     *         tlbi vmalle1 invalidates EL2 TLB entries.
     *   (c) EL2 with VHE off (HCR_EL2.E2H=0, e.g. UEFI-direct on
     *       Jetson firmware v36.4.7 — see
     *       docs/jetson-uefi-direct-result.md §5c and P1 dump):
     *         SCTLR_EL1 is a SEPARATE, dormant register. Writing
     *         it here does NOT touch the active EL2 MMU — the
     *         MMU stays ON. That is the latent bug that made the
     *         subsequent `msr hcr_el2` in boot.S hang (flipping
     *         E2H mid-flight with UEFI's translation active).
     *         Correct target = SCTLR_EL2, TLB = tlbi alle2.
     *
     * Runtime branch on CurrentEL + HCR_EL2.E2H picks the right
     * target. The inline asm uses a fall-through/branch idiom so
     * each arm runs with its own SCTLR + TLBI sequence. */
    {
        uint64_t cur_el_raw;
        uint64_t cur_el;
        __asm__ volatile("mrs %0, CurrentEL" : "=r"(cur_el_raw));
        cur_el = (cur_el_raw >> 2) & 3;   /* 1=EL1, 2=EL2, 3=EL3 */

        int use_el2 = 0;
        if (cur_el == 2) {
            uint64_t hcr;
            __asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
            /* Treat E2H=0 as "EL2 non-VHE, must target SCTLR_EL2".
             * E2H=1 means VHE is already on and SCTLR_EL1 aliases
             * SCTLR_EL2, so the EL1-path sequence is correct. */
            use_el2 = ((hcr >> 34) & 1) == 0;
        }

        if (use_el2) {
            __asm__ volatile(
                "mrs    x0, sctlr_el2\n"
                "bic    x0, x0, #(1 << 0)\n"   /* M  */
                "bic    x0, x0, #(1 << 2)\n"   /* C  */
                "bic    x0, x0, #(1 << 12)\n"  /* I  */
                "msr    sctlr_el2, x0\n"
                "isb\n"
                "tlbi   alle2\n"
                "dsb    nsh\n"
                "isb\n"
                ::: "x0", "memory"
            );
        } else {
            __asm__ volatile(
                "mrs    x0, sctlr_el1\n"
                "bic    x0, x0, #(1 << 0)\n"   /* M  */
                "bic    x0, x0, #(1 << 2)\n"   /* C  */
                "bic    x0, x0, #(1 << 12)\n"  /* I  */
                "msr    sctlr_el1, x0\n"
                "isb\n"
                "tlbi   vmalle1\n"
                "dsb    nsh\n"
                "isb\n"
                ::: "x0", "memory"
            );
        }
    }
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

    efi_print(sys_table, m_entry);

    /*
     * DIAGNOSTIC: dump CurrentEL and HCR_EL2 so the next session
     * knows exactly what UEFI has configured before deciding how to
     * patch the EL2 block's HCR_EL2 write (skip/ORR/RMW).
     *
     * Reading hcr_el2 requires EL2. At EL1 the mrs traps; handle
     * that by reading CurrentEL first and only mrs'ing hcr_el2 if
     * we're at EL2. PR #226 thought we were at EL1 on Jetson; this
     * session's probes (see docs/jetson-uefi-direct-result.md §5b)
     * showed EL2. The dump here prints both values so a reader can
     * confirm without re-running brk probes.
     */
    {
        uint64_t cur_el;
        __asm__ volatile("mrs %0, CurrentEL" : "=r"(cur_el));
        efi_print_hex(sys_table, u"[slmos] CurrentEL=", cur_el);

        /* CurrentEL[3:2] encodes EL: 0=EL0, 4=EL1, 8=EL2, 12=EL3. */
        if (cur_el == 8) {
            uint64_t hcr_el2;
            __asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr_el2));
            efi_print_hex(sys_table, u"[slmos] HCR_EL2 =", hcr_el2);
        } else {
            efi_print(sys_table,
                      u"[slmos] not at EL2, skipping hcr_el2 read\r\n");
        }
    }

    /* Find DTB in configuration table (must be done before ExitBootServices) */
    fdt = efi_find_fdt(sys_table);
    efi_print(sys_table, m_find_done);

    /* Exit boot services — takes over the machine.
     *
     * Per UEFI §7.4.1, Boot Services (including ConOut) become
     * invalid after EBS succeeds. Past revisions of this stub still
     * issued post-EBS efi_print calls as "best-effort" markers
     * (D/E/F) based on the observation that v36.4.7's protocol
     * struct survived EBS with stable vtable pointers. The
     * 2026-04-17 Path-2 P3 hardware probe invalidated that
     * observation: once the SLM-OS VBAR_EL2 is installed
     * post-EBS, the first post-EBS ConOut dereference faults
     * immediately (EC=0x00 "Unknown", ELR inside UEFI's still-
     * live memory range). The prior "markers land as no-ops"
     * interpretation was an artifact of UEFI's vectors silently
     * absorbing the fault and returning; our handler catches it
     * honestly.
     *
     * Post-EBS ConOut calls removed from this function. Any
     * further diagnostic output between here and `primary_cpu`
     * must go through UARTC directly — the Jetson EL2 block in
     * boot.S already has a working UARTC path for that. */
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

    /* Disable MMU and clean caches for the normal boot path.
     *
     * No ConOut calls past this point — see the comment block
     * above efi_pre_ebs for why. Any post-EBS tracing needs to
     * go through UARTC directly. */
    efi_disable_mmu();

    return fdt;
}
