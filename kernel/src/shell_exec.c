/*
 * shell_exec.c - ELF execution commands for SLM-OS shell
 *
 * Commands: elftest, run, kill
 * Includes embedded test ELF binary and program registry.
 */

#include "shell_internal.h"
#include "uart.h"
#include "elf.h"
#include "task.h"
#include "sched.h"
#include "string.h"
#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Embedded ELF Registry
 *
 * Each entry contains a name and pointer to an embedded ELF binary.
 * Use `run` to list available programs, `run <name>` to execute.
 * ============================================================================ */

typedef struct {
    const char *name;           /* Program name */
    const char *description;    /* Short description */
    const uint8_t *data;        /* Pointer to ELF binary */
    size_t size;                /* Size of ELF binary */
} elf_program_t;

/*
 * Minimal ARM64 ELF binary that just returns.
 *
 * Layout:
 *   0x00-0x3F: ELF64 header (64 bytes)
 *   0x40-0x77: Program header (56 bytes)
 *   0x78-0x7B: Code: ret instruction (4 bytes)
 *
 * Total: 124 bytes
 */
static const uint8_t test_elf_binary[] = {
    /* ELF Header (64 bytes) */
    0x7f, 'E', 'L', 'F',     /* e_ident[0-3]: ELF magic */
    2,                       /* e_ident[4]: ELFCLASS64 */
    1,                       /* e_ident[5]: ELFDATA2LSB (little-endian) */
    1,                       /* e_ident[6]: EV_CURRENT */
    0,                       /* e_ident[7]: ELFOSABI_NONE */
    0, 0, 0, 0, 0, 0, 0, 0,  /* e_ident[8-15]: padding */
    2, 0,                    /* e_type: ET_EXEC */
    0xB7, 0,                 /* e_machine: EM_AARCH64 (183) */
    1, 0, 0, 0,              /* e_version: 1 */
    0x78, 0, 0, 0, 0, 0, 0, 0,  /* e_entry: 0x78 (code offset) */
    0x40, 0, 0, 0, 0, 0, 0, 0,  /* e_phoff: 0x40 (program header offset) */
    0, 0, 0, 0, 0, 0, 0, 0,  /* e_shoff: 0 (no section headers) */
    0, 0, 0, 0,              /* e_flags: 0 */
    0x40, 0,                 /* e_ehsize: 64 */
    0x38, 0,                 /* e_phentsize: 56 */
    1, 0,                    /* e_phnum: 1 */
    0, 0,                    /* e_shentsize: 0 */
    0, 0,                    /* e_shnum: 0 */
    0, 0,                    /* e_shstrndx: 0 */

    /* Program Header (56 bytes at offset 0x40) */
    1, 0, 0, 0,              /* p_type: PT_LOAD */
    5, 0, 0, 0,              /* p_flags: PF_R | PF_X */
    0x78, 0, 0, 0, 0, 0, 0, 0,  /* p_offset: 0x78 */
    0x78, 0, 0, 0, 0, 0, 0, 0,  /* p_vaddr: 0x78 */
    0x78, 0, 0, 0, 0, 0, 0, 0,  /* p_paddr: 0x78 */
    4, 0, 0, 0, 0, 0, 0, 0,  /* p_filesz: 4 */
    4, 0, 0, 0, 0, 0, 0, 0,  /* p_memsz: 4 */
    4, 0, 0, 0, 0, 0, 0, 0,  /* p_align: 4 */

    /* Code (4 bytes at offset 0x78) */
    0xC0, 0x03, 0x5F, 0xD6   /* ret (ARM64: 0xD65F03C0) */
};

/* Registry of embedded ELF programs */
static const elf_program_t elf_programs[] = {
    {"test",    "Minimal ELF that returns immediately", test_elf_binary, sizeof(test_elf_binary)},
    /* Add more embedded ELF programs here */
};

#define NUM_ELF_PROGRAMS (sizeof(elf_programs) / sizeof(elf_programs[0]))

/*
 * Find an ELF program by name.
 */
static const elf_program_t *find_elf_program(const char *name)
{
    for (size_t i = 0; i < NUM_ELF_PROGRAMS; i++) {
        if (strcmp(name, elf_programs[i].name) == 0) {
            return &elf_programs[i];
        }
    }
    return NULL;
}

/*
 * elftest - Test ELF loader with a minimal handcrafted ELF
 */
int cmd_elftest(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    uart_puts("ELF Loader Test:\r\n\r\n");

    /* Test 1: Invalid data (not an ELF) */
    uart_puts("  Test 1: Invalid data... ");
    uint8_t not_elf[] = "This is not an ELF file";
    int ret = elf_validate(not_elf, sizeof(not_elf));
    if (ret == ELF_ERR_INVALID) {
        uart_puts("PASS (correctly rejected)\r\n");
    } else {
        uart_printf("FAIL (expected ELF_ERR_INVALID, got %d)\r\n", ret);
    }

    /* Test 2: Truncated file */
    uart_puts("  Test 2: Truncated file... ");
    uint8_t truncated[] = {0x7f, 'E', 'L', 'F'};
    ret = elf_validate(truncated, sizeof(truncated));
    if (ret == ELF_ERR_TRUNCATED) {
        uart_puts("PASS (correctly rejected)\r\n");
    } else {
        uart_printf("FAIL (expected ELF_ERR_TRUNCATED, got %d)\r\n", ret);
    }

    /* Test 3: Minimal valid ELF64 header (wrong arch) */
    uart_puts("  Test 3: Wrong architecture... ");
    uint8_t wrong_arch[64] = {0};
    wrong_arch[0] = 0x7f; wrong_arch[1] = 'E'; wrong_arch[2] = 'L'; wrong_arch[3] = 'F';
    wrong_arch[4] = 2;    /* ELFCLASS64 */
    wrong_arch[5] = 1;    /* ELFDATA2LSB */
    wrong_arch[6] = 1;    /* EV_CURRENT */
    /* e_type at offset 16 */
    wrong_arch[16] = 2;   /* ET_EXEC */
    /* e_machine at offset 18 - x86_64 */
    wrong_arch[18] = 0x3E;
    ret = elf_validate(wrong_arch, sizeof(wrong_arch));
    if (ret == ELF_ERR_ARCH) {
        uart_puts("PASS (correctly rejected)\r\n");
    } else {
        uart_printf("FAIL (expected ELF_ERR_ARCH, got %d)\r\n", ret);
    }

    /* Test 4: Minimal valid ARM64 ELF header */
    uart_puts("  Test 4: Valid ARM64 header... ");
    uint8_t valid_header[64] = {0};
    valid_header[0] = 0x7f; valid_header[1] = 'E'; valid_header[2] = 'L'; valid_header[3] = 'F';
    valid_header[4] = 2;    /* ELFCLASS64 */
    valid_header[5] = 1;    /* ELFDATA2LSB */
    valid_header[6] = 1;    /* EV_CURRENT */
    /* e_type at offset 16 */
    valid_header[16] = 2;   /* ET_EXEC */
    /* e_machine at offset 18 - AARCH64 */
    valid_header[18] = 183; /* EM_AARCH64 */
    /* e_phentsize at offset 54 */
    valid_header[54] = 56;  /* sizeof(Elf64_Phdr) */
    /* e_phnum at offset 56 = 0, so no segments to validate */
    ret = elf_validate(valid_header, sizeof(valid_header));
    if (ret == ELF_OK) {
        uart_puts("PASS (accepted)\r\n");
    } else {
        uart_printf("FAIL (expected ELF_OK, got %d: %s)\r\n", ret, elf_strerror(ret));
    }

    uart_puts("\r\nELF loader validation tests complete.\r\n");
    uart_puts("Note: Full load tests require an actual ELF binary.\r\n");

    return 0;
}

int cmd_run(int argc, char *argv[])
{
    /* No arguments: list available programs */
    if (argc < 2) {
        uart_puts("Available programs:\r\n\r\n");
        for (size_t i = 0; i < NUM_ELF_PROGRAMS; i++) {
            uart_printf("  %-12s %s\r\n",
                        elf_programs[i].name,
                        elf_programs[i].description);
        }
        uart_puts("\r\nUsage: run <name>\r\n");
        return 0;
    }

    /* Find the program by name */
    const elf_program_t *prog = find_elf_program(argv[1]);
    if (!prog) {
        uart_printf("Unknown program: %s\r\n", argv[1]);
        uart_puts("Use 'run' to list available programs.\r\n");
        return -1;
    }

    uart_printf("Loading '%s'...\r\n", prog->name);

    /* Load the ELF */
    struct elf_info info;
    int ret = elf_load(prog->data, prog->size, &info);
    if (ret != ELF_OK) {
        uart_printf("  Failed to load ELF: %s\r\n", elf_strerror(ret));
        return -1;
    }

    uart_printf("  Loaded %zu segment(s), entry=0x%lx\r\n",
                info.num_segments, info.entry);

    /*
     * Build argv for the ELF program.
     * argv[0] = program name
     * argv[1..n] = additional arguments from command line
     */
    int elf_argc = argc - 1;  /* Skip "run" */
    char **elf_argv = &argv[1];  /* Points to program name */

    /* Create task from ELF with arguments */
    struct task *task = elf_create_task_with_args(&info, prog->name,
                                                   elf_argc, elf_argv);
    if (!task) {
        uart_puts("  Failed to create task\r\n");
        elf_unload(&info);
        return -1;
    }

    uart_printf("  Created task '%s' (id=%u) with %d arg(s)\r\n",
                prog->name, task->id, elf_argc);

    /* Add to scheduler */
    scheduler_add_task(task);
    uart_puts("  Task added to scheduler\r\n");

    /*
     * Note: ELF segment memory is now automatically freed when the task
     * terminates. The cleanup callback set by elf_create_task_with_args
     * calls elf_unload() to free the segment memory.
     */

    return 0;
}

/*
 * kill <pid> - Terminate a task by ID
 */
int cmd_kill(int argc, char *argv[])
{
    if (argc < 2) {
        uart_puts("Usage: kill <pid>\r\n");
        uart_puts("  Terminates a task by its process ID.\r\n");
        uart_puts("  Use 'tasks' to see running task IDs.\r\n");
        return -1;
    }

    uint32_t pid;
    if (shell_parse_uint(argv[1], &pid) != 0) {
        uart_printf("Invalid PID: %s\r\n", argv[1]);
        return -1;
    }

    /* Find the task */
    struct task *target = task_get(pid);
    if (!target) {
        uart_printf("No task with PID %lu\r\n", (unsigned long)pid);
        return -1;
    }

    /* Don't allow killing the current task (shell) */
    struct task *current = task_current();
    if (target == current) {
        uart_puts("Cannot kill the current task (shell)\r\n");
        return -1;
    }

    /* Don't allow killing idle tasks (they have special names like "idle" or "idle_0") */
    if (strcmp(target->name, "idle") == 0 ||
        (target->name[0] == 'i' && target->name[1] == 'd' &&
         target->name[2] == 'l' && target->name[3] == 'e' &&
         target->name[4] == '_')) {
        uart_puts("Cannot kill idle tasks\r\n");
        return -1;
    }

    /* Check if already terminated */
    if (target->state == TASK_TERMINATED) {
        uart_printf("Task %lu is already terminated\r\n", (unsigned long)pid);
        return 0;
    }

    uart_printf("Killing task '%s' (pid=%lu)...\r\n", target->name, (unsigned long)pid);

    /* Mark as terminated and remove from scheduler */
    target->state = TASK_TERMINATED;
    scheduler_remove_task(target);

    /* Destroy the task (frees stack) */
    task_destroy(target);

    uart_puts("Task terminated.\r\n");
    return 0;
}
