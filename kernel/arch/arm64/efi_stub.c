/*
 * efi_stub.c - Minimal EFI boot stub for SLM-OS
 *
 * Handles the EFI boot protocol when SLM-OS is launched directly by UEFI
 * as a PE/COFF EFI application. Extracts the DTB from the EFI configuration
 * table, checks that the kernel can be relocated to its link address,
 * calls ExitBootServices to take over the machine, cleans the relevant
 * ranges to the PoC and returns the DTB pointer to boot.S (which turns
 * the MMU off, relocates and continues the normal boot).
 *
 * This code runs on UEFI's stack before ExitBootServices. After
 * ExitBootServices, the UEFI stack memory may be freed but remains
 * physically intact (no concurrent writers with interrupts masked).
 *
 * Entry: called from boot.S with x0=EFI_HANDLE, x1=EFI_SYSTEM_TABLE*
 * Exit:  returns DTB pointer (or NULL), MMU still on, ranges cleaned;
 *        or EFI_STUB_ABORT before ExitBootServices.
 */

#include "efi.h"
#include "boot_source.h"
#include "boot_models.h"
#include "ncmem.h"
#include <stddef.h>

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
void efi_print(efi_system_table_t *sys_table, const efi_char16_t *str)
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
 * Format `value` as `digits` upper-case hex characters into `buf`.
 * Returns the number of characters written (== digits).
 */
static int efi_fmt_hex(efi_char16_t *buf, uint64_t value, int digits)
{
    static const efi_char16_t hex_digits[] = u"0123456789ABCDEF";
    for (int i = 0; i < digits; i++) {
        unsigned nibble = (value >> (4 * (digits - 1 - i))) & 0xF;
        buf[i] = hex_digits[nibble];
    }
    return digits;
}

/*
 * Print one memory descriptor as
 *   "[slmos]  mm tt 0x<start 16> +0x<pages 8> <tag>\r\n"
 * `tag` is a short UTF-16 note (may be empty).
 */
static void efi_print_desc(efi_system_table_t *sys_table,
                           const efi_memory_desc_t *d,
                           const efi_char16_t *tag)
{
    efi_char16_t buf[96];
    static const efi_char16_t prefix[] = u"[slmos]  mm ";
    int n = 0;

    for (int i = 0; prefix[i] != 0; i++) {
        buf[n++] = prefix[i];
    }
    n += efi_fmt_hex(&buf[n], d->type, 2);
    buf[n++] = ' ';
    buf[n++] = '0';
    buf[n++] = 'x';
    n += efi_fmt_hex(&buf[n], d->phys_start, 16);
    buf[n++] = ' ';
    buf[n++] = '+';
    buf[n++] = '0';
    buf[n++] = 'x';
    n += efi_fmt_hex(&buf[n], d->num_pages, 8);
    buf[n++] = ' ';
    for (int i = 0; tag[i] != 0 && n < 92; i++) {
        buf[n++] = tag[i];
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n] = 0;
    efi_print(sys_table, buf);
}

static inline bool efi_ranges_overlap(uint64_t a, uint64_t a_len,
                                      uint64_t b, uint64_t b_len)
{
    return a < b + b_len && b < a + a_len;
}

/*
 * Pure helper: decide whether the kernel can be relocated from where
 * UEFI loaded it (`src`) to its link address (`dst`) after
 * ExitBootServices. Pure (takes the descriptor array explicitly and
 * prints nothing) so kernel/tests/test_efi_reloc.c can drive it with
 * synthetic maps.
 *
 * Rules:
 *   - src == dst: nothing to copy, always OK.
 *   - src and dst ranges must not overlap. The copy loop runs from the
 *     source image, so an overlapping forward copy could overwrite the
 *     instructions doing the copy.
 *   - every page of dst must be described by the map (no holes — a
 *     hole is not RAM) and every descriptor touching dst must be a
 *     type the OS owns after EBS (efi_mem_type_free_after_ebs).
 *   - the DTB, when known, must not sit inside dst: the copy would
 *     overwrite it before kernel_main parses it.
 */
efi_reloc_verdict_t efi_check_relocation(const efi_memory_desc_t *map,
                                         uint64_t map_size,
                                         uint64_t desc_size,
                                         uint64_t src, uint64_t dst,
                                         uint64_t size,
                                         uint64_t fdt, uint64_t fdt_size)
{
    if (src == dst) {
        return EFI_RELOC_NOT_NEEDED;
    }
    if (efi_ranges_overlap(src, size, dst, size)) {
        return EFI_RELOC_OVERLAP;
    }
    if (fdt != 0 &&
        efi_ranges_overlap(fdt, fdt_size ? fdt_size : 1, dst, size)) {
        return EFI_RELOC_FDT_IN_DEST;
    }
    if (desc_size < sizeof(efi_memory_desc_t)) {
        return EFI_RELOC_BAD_MAP;
    }

    /*
     * Coverage is summed per descriptor, which is only sound if the
     * descriptors don't overlap (the spec guarantees it) and their
     * ends don't wrap. Reject a descriptor whose size or end overflows
     * rather than let a malformed map double-count and hide a hole.
     */
    uint64_t covered = 0;
    for (uint64_t off = 0; off + desc_size <= map_size; off += desc_size) {
        const efi_memory_desc_t *d =
            (const efi_memory_desc_t *)((const uint8_t *)map + off);
        if (d->num_pages > UINT64_MAX / EFI_PAGE_SIZE) {
            return EFI_RELOC_BAD_MAP;
        }
        uint64_t len = d->num_pages * EFI_PAGE_SIZE;
        if (d->phys_start > UINT64_MAX - len) {
            return EFI_RELOC_BAD_MAP;
        }
        if (!efi_ranges_overlap(d->phys_start, len, dst, size)) {
            continue;
        }
        if (!efi_mem_type_free_after_ebs(d->type)) {
            return EFI_RELOC_DEST_BUSY;
        }
        uint64_t lo = d->phys_start > dst ? d->phys_start : dst;
        uint64_t hi_d = d->phys_start + len;
        uint64_t hi = hi_d < dst + size ? hi_d : dst + size;
        covered += hi - lo;
    }
    if (covered < size) {
        return EFI_RELOC_DEST_NOT_RAM;
    }
    return EFI_RELOC_OK;
}

/*
 * Pure helper (#1196): true when every page of [base, base + len) is
 * described by the map with a type the OS owns after ExitBootServices,
 * the same rule efi_check_relocation applies to the link range. Such
 * memory is RAM that UEFI allocates from and maps, so cleaning it by
 * VA with UEFI's identity map live cannot fault. Descriptors whose size
 * or end overflows make the answer false.
 */
bool efi_range_os_owned(const efi_memory_desc_t *map, uint64_t map_size,
                        uint64_t desc_size, uint64_t base, uint64_t len)
{
    if (len == 0 || base > UINT64_MAX - len ||
        desc_size < sizeof(efi_memory_desc_t)) {
        return false;
    }

    uint64_t covered = 0;
    for (uint64_t off = 0; off + desc_size <= map_size; off += desc_size) {
        const efi_memory_desc_t *d =
            (const efi_memory_desc_t *)((const uint8_t *)map + off);
        if (d->num_pages > UINT64_MAX / EFI_PAGE_SIZE) {
            return false;
        }
        uint64_t dlen = d->num_pages * EFI_PAGE_SIZE;
        if (d->phys_start > UINT64_MAX - dlen) {
            return false;
        }
        if (!efi_ranges_overlap(d->phys_start, dlen, base, len)) {
            continue;
        }
        if (!efi_mem_type_free_after_ebs(d->type)) {
            return false;
        }
        uint64_t lo = d->phys_start > base ? d->phys_start : base;
        uint64_t hi_d = d->phys_start + dlen;
        uint64_t hi = hi_d < base + len ? hi_d : base + len;
        covered += hi - lo;
    }
    return covered >= len;
}

/*
 * Read the UEFI memory map into a freshly allocated pool buffer with
 * slack for later re-reads (the allocation itself, and anything the
 * firmware does while we print, can grow the map).
 */
efi_status_t efi_read_map(efi_boot_services_t *bs,
                          efi_memory_desc_t **map_out,
                          uint64_t *buf_size_out,
                          uint64_t *map_size_out,
                          uint64_t *map_key_out,
                          uint64_t *desc_size_out)
{
    efi_status_t status;
    uint64_t map_size = 0;
    uint64_t map_key = 0;
    uint64_t desc_size = 0;
    uint32_t desc_version = 0;
    efi_memory_desc_t *map = NULL;

    /* First call: get required buffer size. Expected result is
     * EFI_BUFFER_TOO_SMALL with map_size filled in. Conformant firmware
     * also fills desc_size; defend against one that leaves it 0 so the
     * slack below is never zero. */
    (void)bs->get_memory_map(&map_size, NULL, &map_key,
                             &desc_size, &desc_version);
    if (desc_size == 0) {
        desc_size = sizeof(efi_memory_desc_t);
    }
    /* Slack for the AllocatePool call itself and later map churn. */
    map_size += desc_size * 16;

    status = bs->allocate_pool(EFI_LOADER_DATA, map_size, (void **)&map);
    if (status != EFI_SUCCESS) {
        return status;
    }

    uint64_t buf_size = map_size;
    status = bs->get_memory_map(&map_size, map, &map_key,
                                &desc_size, &desc_version);
    if (status != EFI_SUCCESS) {
        bs->free_pool(map);
        return status;
    }

    *map_out = map;
    *buf_size_out = buf_size;
    *map_size_out = map_size;
    *map_key_out = map_key;
    *desc_size_out = desc_size;
    return EFI_SUCCESS;
}

/*
 * Call ExitBootServices with retry.
 *
 * ExitBootServices requires the map_key from the most recent
 * GetMemoryMap. The map is re-read into the existing buffer (no new
 * allocations — the buffer was sized with slack by efi_read_map)
 * immediately before each attempt, since anything done between the
 * caller's own GetMemoryMap and here (ConOut prints included) may have
 * changed it. Per UEFI §7.4.6, after a failed EBS only GetMemoryMap may
 * be called before retrying.
 */
static efi_status_t efi_exit_boot(efi_handle_t handle,
                                  efi_boot_services_t *bs,
                                  efi_memory_desc_t *map,
                                  uint64_t buf_size)
{
    efi_status_t status = EFI_INVALID_PARAMETER;

    for (int attempt = 0; attempt < 2; attempt++) {
        uint64_t map_size = buf_size;
        uint64_t map_key = 0;
        uint64_t desc_size = 0;
        uint32_t desc_version = 0;

        status = bs->get_memory_map(&map_size, map, &map_key,
                                    &desc_size, &desc_version);
        if (status != EFI_SUCCESS) {
            return status;
        }
        status = bs->exit_boot_services(handle, map_key);
        if (status == EFI_SUCCESS) {
            break;
        }
    }

    /* After a successful ExitBootServices boot services are gone and
     * the OS owns the machine. Don't call free_pool or any other boot
     * service from here. */
    return status;
}

/*
 * Clean and invalidate [start, start + len) to the Point of Coherency
 * by virtual address (DC CIVAC), with UEFI's identity map still live.
 *
 * Why by VA and not by set/way: boot.S turns the MMU and caches off
 * right after efi_stub_entry returns, and everything it touches from
 * then on (instruction fetch from the loaded image, the copy source
 * and destination, the DTB) is read and written as Device memory that
 * bypasses the caches. Those ranges must therefore be in DRAM, and no
 * dirty line may be left that a later eviction could write back over
 * the relocated kernel. DC CIVAC by VA is architecturally guaranteed
 * to reach the PoC through every cache level, including system caches
 * beyond the CPU cluster. Set/way maintenance is not: it only operates
 * on the caches of the executing PE, and Arm's guidance (ARM ARM
 * D7.5.9 "Performing cache maintenance instructions") is that it is
 * not a means of pushing data to the PoC. The previous set/way-based
 * efi_disable_mmu() then kept running C code (its own epilogue) with
 * the MMU off, and the board faulted in exactly that window — TF-A
 * reported "RAS Uncorrectable Error in IOB ... Error response from
 * slave" (docs/deploy/jetson-uefi-direct.md, Findings 1-2).
 */
void efi_dcache_clean_inval(uint64_t start, uint64_t len)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    /* CTR_EL0.DminLine [19:16] = log2(words) of the smallest D line. */
    uint64_t line = 4ULL << ((ctr >> 16) & 0xF);
    uint64_t end = start + len;

    for (uint64_t a = start & ~(line - 1); a < end; a += line) {
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Show the descriptors that matter for the relocation: everything
 * overlapping the link range, plus whatever holds the loaded image and
 * the DTB (fdt == 0: none).
 */
static void efi_print_relevant_descs(efi_system_table_t *sys_table,
                                     const efi_memory_desc_t *map,
                                     uint64_t map_size, uint64_t desc_size,
                                     uint64_t load_base, uint64_t link_base,
                                     uint64_t image_size, uint64_t fdt)
{
    for (uint64_t off = 0; off + desc_size <= map_size; off += desc_size) {
        const efi_memory_desc_t *d =
            (const efi_memory_desc_t *)((const uint8_t *)map + off);
        uint64_t len = d->num_pages * EFI_PAGE_SIZE;
        if (efi_ranges_overlap(d->phys_start, len, link_base, image_size)) {
            efi_print_desc(sys_table, d, u"link");
        }
        if (efi_ranges_overlap(d->phys_start, len, load_base, image_size)) {
            efi_print_desc(sys_table, d, u"load");
        }
        if (fdt != 0 && efi_ranges_overlap(d->phys_start, len, fdt, 1)) {
            efi_print_desc(sys_table, d, u"fdt");
        }
    }
}

/*
 * Print the relocation verdict in words so a failed boot explains
 * itself on the console before control returns to the firmware.
 */
static void efi_print_verdict(efi_system_table_t *sys_table,
                              efi_reloc_verdict_t v)
{
    switch (v) {
    case EFI_RELOC_OK:
        efi_print(sys_table, u"[slmos] reloc: copy to link address after EBS\r\n");
        break;
    case EFI_RELOC_NOT_NEEDED:
        efi_print(sys_table, u"[slmos] reloc: loaded at link address\r\n");
        break;
    case EFI_RELOC_OVERLAP:
        efi_print(sys_table, u"[slmos] reloc: ABORT load and link ranges overlap\r\n");
        break;
    case EFI_RELOC_FDT_IN_DEST:
        efi_print(sys_table, u"[slmos] reloc: ABORT DTB inside link range\r\n");
        break;
    case EFI_RELOC_DEST_BUSY:
        efi_print(sys_table, u"[slmos] reloc: ABORT firmware memory in link range\r\n");
        break;
    case EFI_RELOC_DEST_NOT_RAM:
        efi_print(sys_table, u"[slmos] reloc: ABORT link range not all RAM\r\n");
        break;
    case EFI_RELOC_BAD_MAP:
        efi_print(sys_table, u"[slmos] reloc: ABORT bad memory map\r\n");
        break;
    }
}

/* FDT header: magic and totalsize are big-endian u32 at offsets 0/4. */
static uint64_t efi_fdt_size(const void *fdt)
{
    const uint8_t *p = fdt;
    if (fdt == NULL) {
        return 0;
    }
    uint32_t magic = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | p[3];
    if (magic != 0xd00dfeedU) {
        return 0;
    }
    return ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
           ((uint32_t)p[6] << 8) | p[7];
}

/*
 * EFI stub entry point.
 *
 * Called from boot.S when EFI boot is detected:
 *   x0 = image handle, x1 = system table (from UEFI)
 *   x2 = load_base  — runtime address of _start (where UEFI put us)
 *   x3 = link_base  — link-time address of _start (where the rest of
 *                     the kernel expects to run)
 *   x4 = image_size — SizeOfImage (_kernel_size): text+data+bss+stack
 *
 * Returns the DTB pointer (or NULL) with ExitBootServices done and the
 * image, destination and DTB cleaned to the PoC. boot.S then turns the
 * MMU off, copies the image from load_base to link_base and jumps into
 * the copy. Returns EFI_STUB_ABORT, WITHOUT calling
 * ExitBootServices, when the relocation can't be done safely; boot.S
 * then returns EFI_LOAD_ERROR to the firmware, which moves on to the
 * next boot option instead of hanging the board.
 *
 * Why relocate instead of running in place: the PE image has no
 * .reloc section and the kernel is not position-independent — linker
 * symbols, function-pointer tables, smp_boot.S literals and the
 * identity-mapped page tables all assume PA == link address, exactly
 * as the kexec path provides. Copying the image to the link address
 * restores that invariant for the whole kernel at once.
 */
void *efi_stub_entry(efi_handle_t handle, efi_system_table_t *sys_table,
                     uint64_t load_base, uint64_t link_base,
                     uint64_t image_size)
{
    void *fdt = NULL;
    efi_status_t status;
    efi_boot_services_t *bs = sys_table->boot_services;

    efi_print(sys_table, u"[slmos] A efi_entry\r\n");

    {
        uint64_t cur_el;
        __asm__ volatile("mrs %0, CurrentEL" : "=r"(cur_el));
        efi_print_hex(sys_table, u"[slmos] CurrentEL=", cur_el);
        /* CurrentEL[3:2] encodes EL: 0=EL0, 4=EL1, 8=EL2, 12=EL3.
         * hcr_el2 is only readable at EL2. */
        if (cur_el == 8) {
            uint64_t hcr_el2;
            __asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr_el2));
            efi_print_hex(sys_table, u"[slmos] HCR_EL2  =", hcr_el2);
        }
    }

    /* Find DTB in configuration table (must be done before ExitBootServices) */
    fdt = efi_find_fdt(sys_table);
    uint64_t fdt_size = efi_fdt_size(fdt);
    efi_print_hex(sys_table, u"[slmos] load     =", load_base);
    efi_print_hex(sys_table, u"[slmos] link     =", link_base);
    efi_print_hex(sys_table, u"[slmos] size     =", image_size);
    efi_print_hex(sys_table, u"[slmos] fdt      =", (uint64_t)(uintptr_t)fdt);
    efi_print_hex(sys_table, u"[slmos] fdt_size =", fdt_size);

    efi_memory_desc_t *map = NULL;
    uint64_t buf_size = 0, map_size = 0, map_key = 0, desc_size = 0;
    status = efi_read_map(bs, &map, &buf_size, &map_size, &map_key,
                          &desc_size);
    if (status != EFI_SUCCESS) {
        efi_print(sys_table, u"[slmos] ! GetMemoryMap failed\r\n");
        return EFI_STUB_ABORT;
    }

    efi_print_relevant_descs(sys_table, map, map_size, desc_size,
                             load_base, link_base, image_size,
                             (uint64_t)(uintptr_t)fdt);

    efi_reloc_verdict_t verdict =
        efi_check_relocation(map, map_size, desc_size, load_base,
                             link_base, image_size,
                             (uint64_t)(uintptr_t)fdt, fdt_size);
    efi_print_verdict(sys_table, verdict);
    if (verdict != EFI_RELOC_OK && verdict != EFI_RELOC_NOT_NEEDED) {
        bs->free_pool(map);
        return EFI_STUB_ABORT;
    }

    /*
     * Read any model files while the firmware's file protocols are
     * alive (ADR 0002). This grows the memory map, so the map is read
     * again afterwards and the relocation decision repeated on the map
     * the machine is actually handed over with. Model pages are
     * LoaderData, which counts as OS-owned, so that check would not
     * notice a model inside the link range: the range picker keeps them
     * apart and efi_stub_entry's exclusion list below is what enforces
     * it. Nothing in this block can stop the boot.
     */
    efi_models_stats_t model_stats;
    efi_models_cfg_t model_cfg = {
        .floor = EFI_MODEL_FLOOR,
        .limit = EFI_MODEL_LIMIT,
        .excl_start = { link_base, load_base, (uint64_t)(uintptr_t)fdt },
        .excl_len = { image_size, image_size, fdt ? (fdt_size ? fdt_size : 1) : 0 },
    };
    efi_models_load(sys_table, &model_cfg, &slmos_boot_models, &model_stats);

    bs->free_pool(map);
    status = efi_read_map(bs, &map, &buf_size, &map_size, &map_key,
                          &desc_size);
    if (status != EFI_SUCCESS) {
        efi_print(sys_table, u"[slmos] ! GetMemoryMap failed\r\n");
        efi_models_release(sys_table, &slmos_boot_models);
        return EFI_STUB_ABORT;
    }
    verdict = efi_check_relocation(map, map_size, desc_size, load_base,
                                   link_base, image_size,
                                   (uint64_t)(uintptr_t)fdt, fdt_size);
    if (verdict != EFI_RELOC_OK && verdict != EFI_RELOC_NOT_NEEDED) {
        efi_print_verdict(sys_table, verdict);
        efi_models_release(sys_table, &slmos_boot_models);
        bs->free_pool(map);
        return EFI_STUB_ABORT;
    }

#if defined(NC_MEM_BASE)
    /*
     * #1196: UEFI ran with the NC block in its Write-Back identity map, so
     * CPU 0, which is not reset at the hand-off, may hold dirty lines for
     * it. They are cleaned below, after ExitBootServices, together with
     * the image. Only if the map says the block is OS-owned RAM, which
     * UEFI maps: a by-VA clean of an unmapped address would fault with no
     * console left. The descriptors are printed for the record.
     */
    bool nc_clean = efi_range_os_owned(map, map_size, desc_size,
                                       NC_MEM_BASE, NC_MEM_SIZE);
    for (uint64_t off = 0; off + desc_size <= map_size; off += desc_size) {
        const efi_memory_desc_t *d =
            (const efi_memory_desc_t *)((const uint8_t *)map + off);
        if (d->num_pages <= UINT64_MAX / EFI_PAGE_SIZE &&
            efi_ranges_overlap(d->phys_start, d->num_pages * EFI_PAGE_SIZE,
                               NC_MEM_BASE, NC_MEM_SIZE)) {
            efi_print_desc(sys_table, d, u"nc");
        }
    }
    efi_print(sys_table, nc_clean
              ? u"[slmos] nc block: OS-owned, clean+invalidate at exit\r\n"
              : u"[slmos] ! nc block: not OS-owned RAM, not cleaned\r\n");
#endif

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
    efi_print(sys_table, u"[slmos] C calling ExitBootServices\r\n");
    status = efi_exit_boot(handle, bs, map, buf_size);

    if (status != EFI_SUCCESS) {
        /* Neither attempt succeeded. UEFI §7.4.6: firmware may already
         * have partly shut down Boot Services during the first call,
         * and a loader must not call any boot service other than
         * GetMemoryMap after it, so no ConOut message and no FreePool
         * here. Hand EFI_LOAD_ERROR back to the firmware; the console
         * shows "C calling ExitBootServices" with no "EL2" after it. */
        return EFI_STUB_ABORT;
    }

#if defined(PLATFORM_JETSON_ORIN_NANO)
    /*
     * Take ownership of the EL2 exception vectors IMMEDIATELY after
     * ExitBootServices returns, before any other post-EBS work runs.
     *
     * Until this runs, VBAR_EL2 still points at ArmCpuDxe's vector
     * table, which depends on Boot Services state that the EBS just
     * tore down. Any synchronous fault in the code that follows
     * (the cache maintenance below, the return to boot.S, the MMU-off,
     * the Jetson EL2 block's HCR_EL2 write, UARTC probe) would
     * bounce to that now-invalid handler and hang silently — the
     * historical failure mode tracked in
     * docs/archive/investigations/jetson-uefi-direct-result.md §5c.
     *
     * `jetson_early_vbar_el2` is the SLM-OS-owned diagnostic table
     * defined at the end of boot.S. It saves ESR/ELR/FAR/SPSR/HCR to
     * `jetson_early_fault_slot` in BSS and emits "!FAULT\r\n" over
     * UARTC. The declared size [2048] matches the 16 × 0x80 ARM64
     * vector-table layout so the compiler can catch a stray
     * out-of-range indexing in any future C caller.
     *
     * Only runs from EL2 — CurrentEL-gated because reading or
     * writing VBAR_EL2 from EL1 would trap. At EL1 there is nothing
     * to install (and no way to report it: ConOut is gone after a
     * successful ExitBootServices), so the EL1 case is a silent skip.
     *
     * Kexec path doesn't pass through here (it jumps straight to
     * real_start with x1 = 0), so this change is UEFI-direct only.
     */
    {
        extern uint8_t jetson_early_vbar_el2[2048];
        uint64_t cur_el;
        __asm__ volatile("mrs %0, CurrentEL" : "=r"(cur_el));
        if (cur_el == 8) {
            __asm__ volatile(
                "msr vbar_el2, %0\n"
                "isb\n"
                :
                : "r"(jetson_early_vbar_el2)
                : "memory");
        }
    }
#endif

    /*
     * Push everything boot.S will touch with the MMU off out to DRAM
     * (see efi_dcache_clean_inval for why this is by VA):
     *   - the loaded image: instruction fetch and copy source;
     *   - the copy destination: clean+invalidate so no stale dirty
     *     line from UEFI's earlier use of that memory can be evicted
     *     over the relocated kernel later;
     *   - the DTB: kernel_main parses it before the MMU comes back;
     *   - the NC block (Jetson, #1196): SLM-OS maps it Non-cacheable,
     *     so a dirty UEFI line left there would be evicted over it.
     * Model files were cleaned the same way when they were read
     * (efi_model.c) and nothing has written them since.
     * Nothing after this point may write memory that the MMU-off code
     * reads: the C epilogue only loads from the stack, and boot.S
     * abandons the UEFI stack after the return.
     *
     * The MMU itself is switched off in boot.S, not here, so no stack
     * access happens with the MMU off.
     *
     * No ConOut calls past ExitBootServices — see the comment block
     * above. Post-EBS tracing goes through UARTC directly.
     *
     * The boot source is recorded first: it lives in .data, so it must
     * be written before the image is cleaned and copied for the
     * relocated kernel to see it (boot_source.h).
     */
    slmos_boot_source = BOOT_SOURCE_UEFI;

    efi_dcache_clean_inval(load_base, image_size);
    if (link_base != load_base) {
        efi_dcache_clean_inval(link_base, image_size);
    }
    if (fdt != NULL) {
        efi_dcache_clean_inval((uint64_t)(uintptr_t)fdt,
                               fdt_size ? fdt_size : EFI_PAGE_SIZE);
    }
#if defined(NC_MEM_BASE)
    /* The NC block (#1196, decided before ExitBootServices above): no
     * dirty line from UEFI may survive into SLM-OS's Non-cacheable
     * mapping of it. ncmem_init cleans it again with the MMU on. */
    if (nc_clean) {
        efi_dcache_clean_inval(NC_MEM_BASE, NC_MEM_SIZE);
    }
#endif

    return fdt;
}
