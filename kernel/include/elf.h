/*
 * elf.h - ELF64 loader for SLM-OS
 *
 * Minimal ELF64 parser and loader for ARM64 executables.
 * Supports loading PT_LOAD segments into kernel address space.
 *
 * Usage:
 *   struct elf_info info;
 *   int ret = elf_load(buffer, size, &info);
 *   if (ret == 0) {
 *       // Create task with info.entry as entry point
 *   }
 */

#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>

/* ELF Magic Number */
#define ELF_MAGIC       0x464C457F  /* "\x7FELF" in little-endian */

/* ELF Class (32-bit vs 64-bit) */
#define ELFCLASS64      2

/* ELF Data Encoding */
#define ELFDATA2LSB     1           /* Little-endian */

/* ELF Type */
#define ET_EXEC         2           /* Executable file */
#define ET_DYN          3           /* Shared object (PIE) */

/* ELF Machine */
#define EM_AARCH64      183         /* ARM 64-bit */

/* Program Header Types */
#define PT_NULL         0           /* Unused entry */
#define PT_LOAD         1           /* Loadable segment */
#define PT_DYNAMIC      2           /* Dynamic linking info */
#define PT_INTERP       3           /* Interpreter path */
#define PT_NOTE         4           /* Auxiliary info */
#define PT_PHDR         6           /* Program header table */

/* Program Header Flags */
#define PF_X            0x1         /* Execute */
#define PF_W            0x2         /* Write */
#define PF_R            0x4         /* Read */

/* ELF64 Header */
typedef struct {
    uint8_t     e_ident[16];    /* ELF identification */
    uint16_t    e_type;         /* Object file type */
    uint16_t    e_machine;      /* Machine type */
    uint32_t    e_version;      /* Object file version */
    uint64_t    e_entry;        /* Entry point address */
    uint64_t    e_phoff;        /* Program header offset */
    uint64_t    e_shoff;        /* Section header offset */
    uint32_t    e_flags;        /* Processor-specific flags */
    uint16_t    e_ehsize;       /* ELF header size */
    uint16_t    e_phentsize;    /* Program header entry size */
    uint16_t    e_phnum;        /* Number of program headers */
    uint16_t    e_shentsize;    /* Section header entry size */
    uint16_t    e_shnum;        /* Number of section headers */
    uint16_t    e_shstrndx;     /* Section name string table index */
} Elf64_Ehdr;

/* ELF64 Program Header */
typedef struct {
    uint32_t    p_type;         /* Segment type */
    uint32_t    p_flags;        /* Segment flags */
    uint64_t    p_offset;       /* File offset */
    uint64_t    p_vaddr;        /* Virtual address */
    uint64_t    p_paddr;        /* Physical address */
    uint64_t    p_filesz;       /* Size in file */
    uint64_t    p_memsz;        /* Size in memory */
    uint64_t    p_align;        /* Alignment */
} Elf64_Phdr;

/* Maximum number of loadable segments */
#define ELF_MAX_SEGMENTS    8

/* Loaded segment information */
struct elf_segment {
    uint64_t    vaddr;          /* Virtual address */
    uint64_t    size;           /* Size in memory */
    uint32_t    flags;          /* PF_R | PF_W | PF_X */
    void       *data;           /* Allocated memory */
};

/* ELF load result */
struct elf_info {
    uint64_t            entry;          /* Entry point address */
    uint64_t            load_base;      /* Base address of loaded image */
    uint64_t            load_size;      /* Total size of loaded image */
    size_t              num_segments;   /* Number of loaded segments */
    struct elf_segment  segments[ELF_MAX_SEGMENTS];
};

/* Error codes */
#define ELF_OK                  0
#define ELF_ERR_INVALID         (-1)   /* Not a valid ELF file */
#define ELF_ERR_ARCH            (-2)   /* Wrong architecture */
#define ELF_ERR_TYPE            (-3)   /* Not an executable */
#define ELF_ERR_NOMEM           (-4)   /* Out of memory */
#define ELF_ERR_SEGMENTS        (-5)   /* Too many segments */
#define ELF_ERR_TRUNCATED       (-6)   /* File truncated */
#define ELF_ERR_ARGV_TOO_LARGE  (-7)   /* argv strings exceed stack budget */

/* Maximum number of argv entries supported by elf_create_task_with_args. */
#define ELF_MAX_ARGV            16

/*
 * Validate an ELF file without loading.
 *
 * @buffer: Pointer to ELF file in memory
 * @size:   Size of buffer
 *
 * Returns: ELF_OK if valid, error code otherwise.
 */
int elf_validate(const void *buffer, size_t size);

/*
 * Load an ELF file into memory.
 *
 * Allocates memory for all PT_LOAD segments, copies data, and zeros BSS.
 * The loaded segments share the kernel address space (no isolation).
 *
 * @buffer: Pointer to ELF file in memory
 * @size:   Size of buffer
 * @info:   Output structure for load information
 *
 * Returns: ELF_OK on success, error code otherwise.
 *          On success, info is populated with entry point and segment details.
 *          Caller is responsible for calling elf_unload() to free memory.
 */
int elf_load(const void *buffer, size_t size, struct elf_info *info);

/*
 * Unload a previously loaded ELF.
 *
 * Frees all memory allocated for segments.
 *
 * @info: Load information from elf_load()
 */
void elf_unload(struct elf_info *info);

/*
 * Get a human-readable error message.
 *
 * @err: Error code from elf_validate() or elf_load()
 *
 * Returns: Static string describing the error.
 */
const char *elf_strerror(int err);

/* Forward declaration for task struct */
struct task;

/*
 * Create a kernel task from a loaded ELF.
 *
 * Creates a task that will execute the ELF's entry point. The task runs
 * in kernel space (no memory isolation). The ELF code can only use
 * kernel-provided functions via explicit FFI or must be self-contained.
 *
 * @info: Load information from elf_load()
 * @name: Task name (for debugging)
 *
 * Returns: Pointer to created task, or NULL on failure.
 *          The returned task is ready to be added to the scheduler.
 *          Caller retains ownership of info and must call elf_unload()
 *          after the task terminates.
 *
 * Note: Calls elf_create_task_with_args(info, name, 0, NULL).
 */
struct task *elf_create_task(const struct elf_info *info, const char *name);

/*
 * Create a kernel task from a loaded ELF with argc/argv support.
 *
 * Similar to elf_create_task, but passes argc and argv to the entry point.
 * The entry point signature should be: int main(int argc, char *argv[])
 *
 * @info: Load information from elf_load()
 * @name: Task name (for debugging)
 * @argc: Argument count
 * @argv: Argument vector (will be copied to task's stack)
 *
 * Returns: Pointer to created task, or NULL on failure.
 *          The returned task is ready to be added to the scheduler.
 *
 * Note: argv strings are copied to the task's stack, so the caller can
 *       free them after this function returns.
 */
struct task *elf_create_task_with_args(const struct elf_info *info,
                                        const char *name,
                                        int argc, char *argv[]);

/*
 * Copy argv strings into a bounded destination buffer, recording the
 * resulting pointers in arg_locations[]. This is the bounds-checked core
 * of elf_create_task_with_args's argv setup, extracted so it can be tested
 * without constructing a full ELF task.
 *
 * The function walks argv[0..argc-1], copying each string (including its
 * NUL terminator) into dst and recording the per-arg pointer in
 * arg_locations. It refuses to write past dst + dst_size; TOCTOU growth
 * of argv[i] between the caller's sizing pass and the copy is caught.
 *
 * @dst:           Destination buffer.
 * @dst_size:      Size of destination buffer in bytes.
 * @argc:          Number of arguments (must be 0..ELF_MAX_ARGV).
 * @argv:          Argument vector (each element NUL-terminated).
 * @arg_locations: Output array; arg_locations[i] receives a pointer into
 *                 dst for argv[i]'s copy. Must have room for argc entries.
 *
 * Returns: ELF_OK on success, ELF_ERR_ARGV_TOO_LARGE if the strings don't
 *          fit in dst, ELF_ERR_INVALID on NULL arguments or argc out of
 *          range.
 */
int elf_copy_argv_strings(char *dst, size_t dst_size,
                          int argc, char *const argv[],
                          char *arg_locations[]);

/*
 * Load a static ARM64 ELF into a per-task EL0 address space.
 *
 * For each PT_LOAD segment the loader allocates fresh physical pages
 * from PMM, copies p_filesz bytes, zero-fills the BSS tail up to
 * p_memsz, and maps each page into the user L1 at p_vaddr with
 * VMM_FLAG_USER + VMM_FLAG_PMM_OWNED + the R/W/X bits derived from
 * p_flags. Because every page is PMM_OWNED, vmm_destroy_user_l1 will
 * reclaim every leaf when the task is torn down — including any
 * partial install left behind by a failure mid-load. Callers are
 * expected to destroy the L1 themselves on a non-OK return.
 *
 * Per-segment validation (failure → ELF_ERR_INVALID):
 *   - p_vaddr must be page-aligned.
 *   - the entire segment ([p_vaddr, p_vaddr+p_memsz)) must lie below
 *     USER_ELF_STACK_PAGE_VA (so segments cannot collide with the
 *     stack the caller is about to map).
 *
 * @blob:      Pointer to the ELF image (typically a kernel-VA pointer
 *             into an .incbin'd blob).
 * @len:       Size of the ELF image.
 * @l1_pa:     Physical address of the per-task L1 to install into.
 * @entry_out: Receives the ELF's entry point (e_entry) on success.
 *
 * Returns ELF_OK on success or one of the existing ELF_ERR_* codes.
 */
int elf_load_user(const void *blob, size_t len,
                  uint64_t l1_pa, uint64_t *entry_out);

#endif /* ELF_H */
