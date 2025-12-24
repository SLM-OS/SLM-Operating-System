/*
 * elf.c - ELF64 loader for SLM-OS
 *
 * Minimal ELF64 parser and loader for ARM64 executables.
 */

#include "elf.h"
#include "pmm.h"
#include "task.h"
#include "debug.h"
#include <stddef.h>

/* Helper to check if pointer is within buffer bounds */
static inline int in_bounds(const void *buffer, size_t size,
                            const void *ptr, size_t len)
{
    const uint8_t *buf = buffer;
    const uint8_t *p = ptr;
    return (p >= buf) && (p + len <= buf + size);
}

/*
 * Validate ELF header.
 */
static int validate_header(const Elf64_Ehdr *ehdr, size_t size)
{
    /* Check minimum size for header */
    if (size < sizeof(Elf64_Ehdr)) {
        return ELF_ERR_TRUNCATED;
    }

    /* Check magic number */
    if (*(uint32_t *)ehdr->e_ident != ELF_MAGIC) {
        return ELF_ERR_INVALID;
    }

    /* Check 64-bit */
    if (ehdr->e_ident[4] != ELFCLASS64) {
        return ELF_ERR_INVALID;
    }

    /* Check little-endian */
    if (ehdr->e_ident[5] != ELFDATA2LSB) {
        return ELF_ERR_INVALID;
    }

    /* Check ARM64 architecture */
    if (ehdr->e_machine != EM_AARCH64) {
        return ELF_ERR_ARCH;
    }

    /* Check executable type */
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        return ELF_ERR_TYPE;
    }

    /* Check program header size */
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr)) {
        return ELF_ERR_INVALID;
    }

    /* Check program headers are within file */
    if (ehdr->e_phoff + ehdr->e_phnum * sizeof(Elf64_Phdr) > size) {
        return ELF_ERR_TRUNCATED;
    }

    return ELF_OK;
}

int elf_validate(const void *buffer, size_t size)
{
    return validate_header(buffer, size);
}

int elf_load(const void *buffer, size_t size, struct elf_info *info)
{
    const Elf64_Ehdr *ehdr = buffer;
    const Elf64_Phdr *phdr;
    int ret;

    /* Clear output structure */
    for (size_t i = 0; i < ELF_MAX_SEGMENTS; i++) {
        info->segments[i].data = NULL;
    }
    info->num_segments = 0;
    info->entry = 0;
    info->load_base = 0;
    info->load_size = 0;

    /* Validate header */
    ret = validate_header(ehdr, size);
    if (ret != ELF_OK) {
        return ret;
    }

    /* Get program headers */
    phdr = (const Elf64_Phdr *)((const uint8_t *)buffer + ehdr->e_phoff);

    /* First pass: calculate total size and find load base */
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    size_t loadable_count = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) {
            continue;
        }

        loadable_count++;
        if (loadable_count > ELF_MAX_SEGMENTS) {
            return ELF_ERR_SEGMENTS;
        }

        /* Validate segment data is within file */
        if (phdr[i].p_offset + phdr[i].p_filesz > size) {
            return ELF_ERR_TRUNCATED;
        }

        if (phdr[i].p_vaddr < min_vaddr) {
            min_vaddr = phdr[i].p_vaddr;
        }

        uint64_t end = phdr[i].p_vaddr + phdr[i].p_memsz;
        if (end > max_vaddr) {
            max_vaddr = end;
        }
    }

    if (loadable_count == 0) {
        return ELF_ERR_INVALID;
    }

    info->load_base = min_vaddr;
    info->load_size = max_vaddr - min_vaddr;

    /* Second pass: allocate and load segments */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) {
            continue;
        }

        /* Calculate pages needed (round up to page size) */
        size_t memsz = phdr[i].p_memsz;
        size_t pages = (memsz + PAGE_SIZE - 1) / PAGE_SIZE;

        /* Allocate pages */
        void *segment_mem = pmm_alloc_pages(pages);
        if (!segment_mem) {
            /* Cleanup already allocated segments */
            elf_unload(info);
            return ELF_ERR_NOMEM;
        }

        /* Zero the entire allocation (handles BSS) */
        uint8_t *p = segment_mem;
        for (size_t j = 0; j < pages * PAGE_SIZE; j++) {
            p[j] = 0;
        }

        /* Copy file data */
        const uint8_t *src = (const uint8_t *)buffer + phdr[i].p_offset;
        uint8_t *dst = segment_mem;
        for (size_t j = 0; j < phdr[i].p_filesz; j++) {
            dst[j] = src[j];
        }

        /* Store segment info */
        struct elf_segment *seg = &info->segments[info->num_segments];
        seg->vaddr = phdr[i].p_vaddr;
        seg->size = memsz;
        seg->flags = phdr[i].p_flags;
        seg->data = segment_mem;
        info->num_segments++;

        DEBUG_PRINT("ELF: Loaded segment %zu at %p (vaddr=0x%lx, size=0x%lx, flags=%c%c%c)",
                    info->num_segments - 1,
                    segment_mem,
                    phdr[i].p_vaddr,
                    memsz,
                    (phdr[i].p_flags & PF_R) ? 'R' : '-',
                    (phdr[i].p_flags & PF_W) ? 'W' : '-',
                    (phdr[i].p_flags & PF_X) ? 'X' : '-');
    }

    /* Set entry point */
    info->entry = ehdr->e_entry;

    INFO("ELF: Loaded %zu segments, entry=0x%lx", info->num_segments, info->entry);

    return ELF_OK;
}

void elf_unload(struct elf_info *info)
{
    for (size_t i = 0; i < info->num_segments; i++) {
        if (info->segments[i].data) {
            /* Calculate pages to free */
            size_t pages = (info->segments[i].size + PAGE_SIZE - 1) / PAGE_SIZE;
            pmm_free_pages(info->segments[i].data, pages);
            info->segments[i].data = NULL;
        }
    }
    info->num_segments = 0;
    info->entry = 0;
}

const char *elf_strerror(int err)
{
    switch (err) {
    case ELF_OK:
        return "Success";
    case ELF_ERR_INVALID:
        return "Not a valid ELF file";
    case ELF_ERR_ARCH:
        return "Wrong architecture (expected ARM64)";
    case ELF_ERR_TYPE:
        return "Not an executable";
    case ELF_ERR_NOMEM:
        return "Out of memory";
    case ELF_ERR_SEGMENTS:
        return "Too many segments";
    case ELF_ERR_TRUNCATED:
        return "File truncated";
    default:
        return "Unknown error";
    }
}

/*
 * Helper to find the physical address of a virtual address in loaded segments.
 *
 * For position-independent code (ET_DYN) or code at load_base=0,
 * the entry point vaddr maps directly to the first segment's data.
 * For absolute addresses, we find which segment contains it.
 */
static void *vaddr_to_phys(const struct elf_info *info, uint64_t vaddr)
{
    for (size_t i = 0; i < info->num_segments; i++) {
        const struct elf_segment *seg = &info->segments[i];
        if (vaddr >= seg->vaddr && vaddr < seg->vaddr + seg->size) {
            /* Calculate offset within segment */
            uint64_t offset = vaddr - seg->vaddr;
            return (uint8_t *)seg->data + offset;
        }
    }
    return NULL;
}

struct task *elf_create_task(const struct elf_info *info, const char *name)
{
    if (!info || info->num_segments == 0) {
        ERROR("elf_create_task: invalid ELF info");
        return NULL;
    }

    /* Find physical address of entry point */
    void *entry_phys = vaddr_to_phys(info, info->entry);
    if (!entry_phys) {
        ERROR("elf_create_task: entry point 0x%lx not in any segment", info->entry);
        return NULL;
    }

    INFO("ELF: Creating task '%s' with entry at %p (vaddr=0x%lx)",
         name ? name : "elf_task", entry_phys, info->entry);

    /*
     * Convert object pointer to function pointer.
     * ISO C forbids direct cast, so use a union to avoid -Wpedantic warning.
     */
    union {
        void *ptr;
        task_entry_t func;
    } entry_conv;
    entry_conv.ptr = entry_phys;

    /* Create task with the ELF entry point */
    struct task *task = task_create(name ? name : "elf_task", entry_conv.func, NULL);
    if (!task) {
        ERROR("elf_create_task: failed to create task");
        return NULL;
    }

    return task;
}
