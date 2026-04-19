/*
 * sched_hef_embed.S — Embed the scheduler MLP .hef via .incbin.
 *
 * The scheduler_mlp_pi5.hef file produced by scripts/hailo/compile_hef.sh
 * (Phase 6.1) needs to reach the kernel's VFS as /mnt/files/... so
 * `hailo load <path> sched` (Phase 6.2) can read it. /mnt/files is a
 * RAM disk populated at boot from in-kernel blobs; this file embeds
 * the .hef bytes into the kernel's .rodata so sched_hef_init.c can
 * write them into the littlefs image.
 *
 * Only compiled and linked when ENABLE_SCHEDULER_HEF_EMBED=ON in CMake.
 * SCHEDULER_HEF_PATH is supplied by set_source_files_properties
 * COMPILE_DEFINITIONS so GAS's .incbin resolves regardless of the
 * assembler's working directory.
 */

#if !defined(SCHEDULER_HEF_PATH)
# error "SCHEDULER_HEF_PATH must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

    .section .rodata

    .global sched_hef_start
    .global sched_hef_end
    .align 4
sched_hef_start:
    .incbin xstr(SCHEDULER_HEF_PATH)
sched_hef_end:

#if defined(__ELF__) && (defined(__x86_64__) || defined(__i386__))
    .section .note.GNU-stack, "", @progbits
#endif
