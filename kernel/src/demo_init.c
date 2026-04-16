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
 * Provided by demo_scripts.S via .incbin on scripts/demo.lua and
 * scripts/demo_menu.lua. Length of each blob is (_end - _start).
 */
extern const unsigned char demo_lua_start[];
extern const unsigned char demo_lua_end[];
extern const unsigned char demo_menu_lua_start[];
extern const unsigned char demo_menu_lua_end[];

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
