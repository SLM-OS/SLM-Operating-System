/*
 * elf.c - ELF64 loader for SLM-OS
 *
 * Minimal ELF64 parser and loader for ARM64 executables.
 */

#include "elf.h"
#include "pmm.h"
#include "task.h"
#include "sched.h"
#include "debug.h"
#include "string.h"
#include <stddef.h>

/*
 * ELF task entry wrapper.
 *
 * Similar to task_entry_wrapper in task.c, but calls entry(argc, argv)
 * instead of entry(arg).
 *
 * Register usage (set by elf_create_task_with_args):
 *   x19 = entry point address
 *   x20 = argc
 *   x21 = argv pointer
 */
typedef int (*elf_main_t)(int argc, char *argv[]);

static void elf_entry_wrapper(void)
{
    uint64_t entry_reg, argc_reg, argv_reg;

#if defined(PLATFORM_X86_64)
    __asm__ volatile("mov %%rbx, %0" : "=r"(entry_reg));
    __asm__ volatile("mov %%r12, %0" : "=r"(argc_reg));
    __asm__ volatile("mov %%r13, %0" : "=r"(argv_reg));
#else
    __asm__ volatile("mov %0, x19" : "=r"(entry_reg));
    __asm__ volatile("mov %0, x20" : "=r"(argc_reg));
    __asm__ volatile("mov %0, x21" : "=r"(argv_reg));
#endif

    elf_main_t entry = (elf_main_t)(uintptr_t)entry_reg;
    int argc = (int)argc_reg;
    char **argv = (char **)(uintptr_t)argv_reg;

    /* Call the ELF entry point */
    int ret = entry(argc, argv);

    /* Log return value */
    if (ret != 0) {
        INFO("ELF program exited with code %d", ret);
    }

    /* Exit the task */
    extern void task_exit(void);
    task_exit();
}

/*
 * ELF cleanup callback.
 *
 * Called when the task is destroyed. Frees ELF segment memory.
 * The cleanup_arg is a pointer to an elf_info structure stored
 * on the task's stack.
 */
static void elf_cleanup(void *cleanup_arg)
{
    struct elf_info *info = (struct elf_info *)cleanup_arg;
    if (info) {
        DEBUG_PRINT("ELF cleanup: unloading %zu segments", info->num_segments);
        elf_unload(info);
    }
}

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

    /* Check architecture matches current platform */
#if defined(PLATFORM_X86_64)
    if (ehdr->e_machine != 0x3E) {  /* EM_X86_64 = 62 */
        return ELF_ERR_ARCH;
    }
#else
    if (ehdr->e_machine != EM_AARCH64) {
        return ELF_ERR_ARCH;
    }
#endif

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
    /* Delegate to elf_create_task_with_args with no arguments */
    return elf_create_task_with_args(info, name, 0, NULL);
}

struct task *elf_create_task_with_args(const struct elf_info *info,
                                        const char *name,
                                        int argc, char *argv[])
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

    INFO("ELF: Creating task '%s' with entry at %p (vaddr=0x%lx), argc=%d",
         name ? name : "elf_task", entry_phys, info->entry, argc);

    /*
     * Allocate stack for the task.
     * We do this manually so we can set up argv on the stack.
     */
    size_t stack_pages = STACK_SIZE / PAGE_SIZE;
    void *stack = pmm_alloc_pages(stack_pages);
    if (!stack) {
        ERROR("elf_create_task: failed to allocate stack");
        return NULL;
    }

    /* Stack grows downward, so stack_top is at the high end */
    uintptr_t stack_top = (uintptr_t)stack + STACK_SIZE;
    uintptr_t sp = stack_top;

    /*
     * Reserve space at top of stack for elf_info copy.
     * This allows cleanup to free segment memory when task is destroyed.
     */
    sp -= sizeof(struct elf_info);
    sp &= ~15UL;  /* Align to 16 bytes */
    struct elf_info *info_copy = (struct elf_info *)sp;

    /* Copy elf_info to stack */
    const uint8_t *src = (const uint8_t *)info;
    uint8_t *dst = (uint8_t *)info_copy;
    for (size_t i = 0; i < sizeof(struct elf_info); i++) {
        dst[i] = src[i];
    }

    /*
     * Set up argv on the stack.
     *
     * Layout (high to low addresses):
     *   [arg strings]
     *   [NULL terminator]
     *   [argv[argc-1] pointer]
     *   ...
     *   [argv[0] pointer]
     *   <- sp points here
     *
     * We build this by:
     * 1. Copy all arg strings to the top of stack
     * 2. Build argv[] array below the strings
     */

    char **argv_ptrs = NULL;

    if (argc > 0 && argv != NULL) {
        /* First, calculate total string space needed */
        size_t strings_size = 0;
        for (int i = 0; i < argc; i++) {
            strings_size += strlen(argv[i]) + 1;  /* +1 for null terminator */
        }

        /* Align strings_size to 8 bytes */
        strings_size = (strings_size + 7) & ~7UL;

        /* Reserve space for strings at top of stack */
        sp -= strings_size;
        char *strings_area = (char *)sp;

        /* Copy strings and record their locations */
        char *str_ptr = strings_area;
        char *arg_locations[16];  /* Support up to 16 args */
        if (argc > 16) argc = 16;

        for (int i = 0; i < argc; i++) {
            arg_locations[i] = str_ptr;
            strcpy(str_ptr, argv[i]);
            str_ptr += strlen(argv[i]) + 1;
        }

        /* Reserve space for argv array (argc + 1 pointers, including NULL) */
        sp -= (argc + 1) * sizeof(char *);
        sp &= ~15UL;  /* Align to 16 bytes (ARM64 ABI requirement) */
        argv_ptrs = (char **)sp;

        /* Fill in argv array */
        for (int i = 0; i < argc; i++) {
            argv_ptrs[i] = arg_locations[i];
        }
        argv_ptrs[argc] = NULL;  /* NULL terminator */
    } else {
        /* No arguments - just set up empty argv */
        argc = 0;
        sp -= sizeof(char *);
        sp &= ~15UL;
        argv_ptrs = (char **)sp;
        argv_ptrs[0] = NULL;
    }

    /* Ensure stack pointer is 16-byte aligned (ARM64 ABI) */
    sp &= ~15UL;

    /*
     * Now create the task structure manually.
     * We can't use task_create() because we need to set up our own stack.
     */
    extern struct task *task_alloc(const char *name, uint8_t priority);
    struct task *task = task_alloc(name ? name : "elf_task", TASK_PRIORITY_NORMAL);
    if (!task) {
        ERROR("elf_create_task: failed to allocate task");
        pmm_free_pages(stack, stack_pages);
        return NULL;
    }

    /* Set up stack pointers */
    task->stack_base = stack;
    task->stack_top = (void *)stack_top;

    /* Set up initial context */
#if defined(PLATFORM_X86_64)
    task->context.rsp = sp;
    task->context.rip = (uint64_t)elf_entry_wrapper;
    task->context.rbp = 0;
    task->context.rflags = 0;
    task->context.rbx = (uint64_t)entry_phys;
    task->context.r12 = (uint64_t)argc;
    task->context.r13 = (uint64_t)argv_ptrs;
#else
    task->context.sp = sp;
    task->context.x30 = (uint64_t)elf_entry_wrapper;  /* Return address -> wrapper */
    task->context.x29 = 0;  /* Frame pointer */
    task->context.x19 = (uint64_t)entry_phys;
    task->context.x20 = (uint64_t)argc;
    task->context.x21 = (uint64_t)argv_ptrs;
#endif

    /* Set cleanup callback to free ELF segment memory when task is destroyed */
    extern void task_set_cleanup(struct task *task, void (*cleanup)(void *), void *arg);
    task_set_cleanup(task, elf_cleanup, info_copy);

    DEBUG_PRINT("ELF: Task '%s' stack=%p-%p, sp=0x%lx, argc=%d, argv=%p, cleanup=%p",
                task->name, task->stack_base, task->stack_top,
                (unsigned long)sp, argc, (void *)argv_ptrs, (void *)info_copy);

    return task;
}
