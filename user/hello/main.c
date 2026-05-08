/*
 * main.c — EL0 hello binary, built as a stand-alone ARM64 ELF and
 * embedded into the kernel image via .incbin
 * (kernel/src/user_hello_embed.S).
 *
 * Demonstrates the elf_load_user / task_create_user_elf path: the
 * kernel parses this ELF's PT_LOAD segments, allocates physical
 * pages, copies code+rodata+data, and maps each segment into the
 * per-task user L1 with the right RWX bits. Control reaches `_start`
 * via ERET, which calls `main`, which logs a banner via SYS_LOG and
 * returns; `_start` then issues SYS_EXIT.
 *
 * No libc, no startup files — just the SVC stubs in user_syscall.h.
 */

#include "user_syscall.h"

static const char banner[] = "[ELFTEST] hello from EL0 ELF\n";

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sys_log(banner, sizeof(banner) - 1);
    return 0;
}
