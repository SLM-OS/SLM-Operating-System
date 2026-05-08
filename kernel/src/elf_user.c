/*
 * elf_user.c — Load a static ARM64 ELF into a per-task EL0 address
 * space.
 *
 * Companion to elf.c (which loads ELFs into the kernel address space
 * via PMM-owned kernel-VA pages and runs them as kernel-mode tasks).
 * This loader instead allocates fresh PMM pages, copies + zero-fills
 * each page on the kernel-VA side, and maps the page into the
 * per-task user L1 at the ELF's requested user VA with the right
 * R/W/X bits. The caller (`task_create_user_elf`) owns the L1 and is
 * responsible for tearing it down on failure — every page mapped
 * here carries VMM_FLAG_PMM_OWNED, so vmm_destroy_user_l1 reclaims
 * partial installs without explicit unwind from this function.
 */

#include "elf.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "debug.h"

/* USER_TEXT_VA is hard-coded into user/user.ld (the user binary's
 * linker script can't include kernel headers). Pin the value here so
 * a future move of USER_VA_BASE breaks the build instead of silently
 * mismatching the linked ELF. */
_Static_assert(USER_TEXT_VA == 0x4000000000UL,
               "user.ld assumes USER_TEXT_VA == 0x4000000000");

/* Translate a PT_LOAD's p_flags (PF_R | PF_W | PF_X) into VMM flags.
 * Always sets VMM_FLAG_USER (EL0 accessible) + VMM_FLAG_PMM_OWNED
 * (free leaf on user-L1 destroy). */
static uint32_t pf_to_vmm_flags(uint32_t p_flags)
{
    uint32_t f = VMM_FLAG_USER | VMM_FLAG_PMM_OWNED;
    if (p_flags & PF_R) {
        f |= VMM_FLAG_READ;
    }
    if (p_flags & PF_W) {
        f |= VMM_FLAG_WRITE;
    }
    if (p_flags & PF_X) {
        f |= VMM_FLAG_EXEC;
    }
    return f;
}

/* Map one PT_LOAD segment into the user L1. Allocates p_memsz worth
 * of pages, copies p_filesz bytes from the blob, zero-fills the BSS
 * tail. Per-page failures bail out without unwinding — the caller's
 * vmm_destroy_user_l1 reclaims everything we managed to install up
 * to this point via the PMM_OWNED bit. */
static int load_segment(const void *blob, size_t blob_len,
                        const Elf64_Phdr *ph, uint64_t l1_pa)
{
    if (ph->p_memsz == 0) {
        return ELF_OK;
    }

    /* Reject segments that would collide with the user-stack page or
     * spill past the user-VA window. p_vaddr+p_memsz can't wrap here
     * because elf_validate's first pass already rejected memsz that
     * would overflow when added to p_vaddr. */
    if ((ph->p_vaddr & (PAGE_SIZE - 1)) != 0) {
        return ELF_ERR_INVALID;
    }
    uint64_t seg_end = ph->p_vaddr + ph->p_memsz;
    if (ph->p_vaddr < USER_TEXT_VA || seg_end > USER_ELF_STACK_PAGE_VA) {
        return ELF_ERR_INVALID;
    }

    /* p_offset + p_filesz must fit within the blob (caller already
     * runs elf_validate which checks this, but recheck per-segment
     * here so this function is self-contained and survives any
     * future caller that skips the validate). */
    if (ph->p_filesz > ph->p_memsz) {
        return ELF_ERR_INVALID;
    }
    if (ph->p_offset > blob_len ||
        ph->p_filesz > blob_len - ph->p_offset) {
        return ELF_ERR_TRUNCATED;
    }

    uint32_t flags = pf_to_vmm_flags(ph->p_flags);
    uint64_t pages = (ph->p_memsz + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < pages; i++) {
        void *page = pmm_alloc_pages(1);
        if (!page) {
            return ELF_ERR_NOMEM;
        }
        memset(page, 0, PAGE_SIZE);

        /* Copy the slice of file data that overlaps this page, if
         * any. Pages past p_filesz are pure BSS and stay zero. */
        uint64_t off_in_seg = i * PAGE_SIZE;
        if (off_in_seg < ph->p_filesz) {
            uint64_t remaining = ph->p_filesz - off_in_seg;
            uint64_t copy_len = remaining < PAGE_SIZE ? remaining : PAGE_SIZE;
            const uint8_t *src = (const uint8_t *)blob + ph->p_offset + off_in_seg;
            memcpy(page, src, copy_len);
        }

        uint64_t va = ph->p_vaddr + off_in_seg;
        /* TODO(#735): vmm_user_map_page silently replaces an existing
         * L3 entry, so two PT_LOAD segments whose page-rounded ranges
         * overlap would leak the first allocation. Safe today because
         * the only caller (task_create_user_elf) loads the embedded
         * user_hello.elf, whose linker script enforces disjoint
         * page-aligned segments. Add an overwrite-rejection path
         * before accepting filesystem-loaded ELFs. */
        if (vmm_user_map_page(l1_pa, va, (uint64_t)(uintptr_t)page, flags) != 0) {
            pmm_free_pages(page, 1);
            return ELF_ERR_NOMEM;
        }
    }
    return ELF_OK;
}

int elf_load_user(const void *blob, size_t len,
                  uint64_t l1_pa, uint64_t *entry_out)
{
    if (!blob || !entry_out || l1_pa == 0) {
        return ELF_ERR_INVALID;
    }

    /* Reuse the existing header validator — checks magic, ELF64 LE,
     * AArch64 machine, ET_EXEC/ET_DYN, phentsize, phoff/phnum bounds
     * (incl. overflow guards). */
    int rc = elf_validate(blob, len);
    if (rc != ELF_OK) {
        return rc;
    }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)blob;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)blob + eh->e_phoff);

    /* First pass: reject the image up-front if any PT_LOAD's
     * p_vaddr+p_memsz would overflow. elf.c does the same; replicate
     * here so we never enter the install loop with a crafted phdr. */
    uint64_t loaded = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        if (ph[i].p_memsz > UINT64_MAX - ph[i].p_vaddr) {
            return ELF_ERR_TRUNCATED;
        }
        loaded++;
    }
    if (loaded == 0) {
        return ELF_ERR_INVALID;
    }
    if (loaded > ELF_MAX_SEGMENTS) {
        return ELF_ERR_SEGMENTS;
    }

    /* Second pass: install. On any per-segment failure, leave the
     * caller to tear down the L1 — partial PMM_OWNED leaves are
     * reclaimed by vmm_destroy_user_l1. */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        rc = load_segment(blob, len, &ph[i], l1_pa);
        if (rc != ELF_OK) {
            return rc;
        }
    }

    /* e_entry must land inside one of the mapped segments — the
     * arch-specific entry register is an unconditional ERET target,
     * so a stray entry would fault on the first instruction fetch.
     * Cheap check: must be in [USER_TEXT_VA, USER_ELF_STACK_PAGE_VA). */
    if (eh->e_entry < USER_TEXT_VA || eh->e_entry >= USER_ELF_STACK_PAGE_VA) {
        return ELF_ERR_INVALID;
    }

    *entry_out = eh->e_entry;
    DEBUG_PRINT("elf_load_user: %u PT_LOAD segments, entry=0x%lx",
                (unsigned)loaded, (unsigned long)eh->e_entry);
    return ELF_OK;
}
