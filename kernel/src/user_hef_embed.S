/*
 * user_hef_embed.S — Embed a user-supplied Hailo .hef via .incbin.
 *
 * Phase 8 deploy path. /mnt/files is a LittleFS RAM disk populated at
 * boot from in-kernel blobs; there's no FAT driver, so a .hef staged
 * on the boot partition can't reach SLM-OS at runtime. Embedding the
 * bytes into the kernel .rodata is the only available path short of
 * building a FAT driver.
 *
 * Only compiled and linked when USER_HEF_BLOB=<path> is passed to
 * CMake. USER_HEF_PATH is supplied by set_source_files_properties
 * COMPILE_DEFINITIONS so GAS's .incbin resolves regardless of the
 * assembler's working directory. user_hef_init.c writes the bytes
 * to /mnt/files/user.hef at boot.
 *
 * Patterned after sched_hef_embed.S.
 */

#if !defined(USER_HEF_PATH)
# error "USER_HEF_PATH must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

    .section .rodata

    .global user_hef_start
    .global user_hef_end
    .align 4
user_hef_start:
    .incbin xstr(USER_HEF_PATH)
user_hef_end:

#if defined(__ELF__) && (defined(__x86_64__) || defined(__i386__))
    .section .note.GNU-stack, "", @progbits
#endif
