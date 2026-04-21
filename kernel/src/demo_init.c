/*
 * demo_init.c - Write embedded demo scripts to filesystem at boot
 *
 * Writes /mnt/files/demo.lua and /mnt/files/demo_menu.lua at boot so
 * they can be run from the shell via `lua /mnt/files/demo.lua` or
 * `lua /mnt/files/demo_menu.lua`.
 *
 * Content is embedded in demo_scripts.S via .incbin from the source
 * files under scripts/; the demo_lua_* / demo_menu_lua_* symbols are
 * provided by that assembly file (#13). Embedding is gated on the
 * EMBED_DEMO_SCRIPTS CMake option (#14) — when off, demo_init() is a
 * no-op and no demo content is carried in the kernel image.
 */

#include "vfs.h"
#include "littlefs_slm.h"
#include "uart.h"
#include <stddef.h>

#if defined(EMBED_DEMO_SCRIPTS)

/*
 * Provided by demo_scripts.S via .incbin on the scripts/ lua source
 * files. Length of each blob is (_end - _start). The scripts/ files
 * are the single source of truth — there is no C-string mirror to
 * keep in sync (#315, #323).
 */
extern const unsigned char demo_lua_start[];
extern const unsigned char demo_lua_end[];
extern const unsigned char demo_menu_lua_start[];
extern const unsigned char demo_menu_lua_end[];
extern const unsigned char demo_auto_lua_start[];
extern const unsigned char demo_auto_lua_end[];
extern const unsigned char demo_multiproc_lua_start[];
extern const unsigned char demo_multiproc_lua_end[];
extern const unsigned char demo_hailo_lua_start[];
extern const unsigned char demo_hailo_lua_end[];

int demo_init(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx("/mnt/files", &subpath);
    if (!mnt) {
        uart_puts("[WARN] demo_init: /mnt/files not mounted\r\n");
        return -1;
    }

    int f = littlefs_file_open(mnt, "/demo.lua",
                               LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (f < 0) {
        uart_puts("[WARN] demo_init: failed to create /mnt/files/demo.lua\r\n");
        return -1;
    }
    littlefs_file_write(mnt, f, demo_lua_start,
                        (size_t)(demo_lua_end - demo_lua_start));
    littlefs_file_close(mnt, f);

    int fm = littlefs_file_open(mnt, "/demo_menu.lua",
                                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fm < 0) {
        uart_puts("[WARN] demo_init: failed to create /mnt/files/demo_menu.lua\r\n");
        /* demo.lua succeeded, so don't fail the whole init. */
        return 0;
    }
    littlefs_file_write(mnt, fm, demo_menu_lua_start,
                        (size_t)(demo_menu_lua_end - demo_menu_lua_start));
    littlefs_file_close(mnt, fm);

    /* #192: scripted auto-demo sequencer. Now embedded via .incbin
     * (#315) — scripts/demo_auto.lua is the source of truth. */
    int fa = littlefs_file_open(mnt, "/demo_auto.lua",
                                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fa >= 0) {
        littlefs_file_write(mnt, fa, demo_auto_lua_start,
                            (size_t)(demo_auto_lua_end - demo_auto_lua_start));
        littlefs_file_close(mnt, fa);
    }

    /* #323: multi-process demo for concurrent-task narration. */
    int fmp = littlefs_file_open(mnt, "/multiproc_demo.lua",
                                 LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fmp >= 0) {
        littlefs_file_write(mnt, fmp, demo_multiproc_lua_start,
                            (size_t)(demo_multiproc_lua_end - demo_multiproc_lua_start));
        littlefs_file_close(mnt, fmp);
    }

    /* Phase 7: Hailo NPU demo — exercises slm.hailo.status/load/infer.
     * Degrades to a status-only walk on platforms without an AI HAT+. */
    int fh = littlefs_file_open(mnt, "/demo_hailo.lua",
                                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fh >= 0) {
        littlefs_file_write(mnt, fh, demo_hailo_lua_start,
                            (size_t)(demo_hailo_lua_end - demo_hailo_lua_start));
        littlefs_file_close(mnt, fh);
    }

    /* #64: boot-time model preload config. One model name per line.
     * "mnist" is the default; edit the file to change. Lines starting
     * with '#' are comments. Empty file disables boot preloading.
     * Read by model_boot_preload() in shell_init after the scheduler
     * is running. */
    int fp = littlefs_file_open(mnt, "/preload.conf",
                                LFS_O_RDONLY);
    if (fp < 0) {
        /* File doesn't exist — create with default. */
        fp = littlefs_file_open(mnt, "/preload.conf",
                                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
        if (fp >= 0) {
            static const char default_conf[] =
                "# Model preload config — one name per line.\n"
                "# Models listed here are loaded in background tasks\n"
                "# at boot, before the shell prompt appears.\n"
                "# Edit this file to change what gets preloaded.\n"
                "mnist\n";
            littlefs_file_write(mnt, fp, default_conf,
                                sizeof(default_conf) - 1);
            littlefs_file_close(mnt, fp);
        }
    } else {
        /* File exists — leave it alone (user may have edited it). */
        littlefs_file_close(mnt, fp);
    }

    return 0;
}

#else  /* !EMBED_DEMO_SCRIPTS */

/*
 * Demo scripts omitted from this build. The Lua interpreter and the
 * slm.* bindings remain fully functional; users can `lua <inline>` or
 * load scripts written to the filesystem at runtime.
 */
int demo_init(void)
{
    return 0;
}

#endif /* EMBED_DEMO_SCRIPTS */
