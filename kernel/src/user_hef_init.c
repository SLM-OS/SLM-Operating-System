/*
 * user_hef_init.c — Write a user-embedded Hailo .hef to VFS at boot.
 *
 * Partners user_hef_embed.S. When the kernel is built with
 * ENABLE_USER_HEF_EMBED=1 + USER_HEF_BLOB=<path>, the binary bytes
 * land in .rodata (user_hef_start / user_hef_end) and this function
 * writes them into /mnt/files/user.hef so Lua scripts and the shell
 * can reach them without a FAT driver.
 *
 * Callable from main.c's boot sequence *after* vfs_init() /
 * littlefs_mount_at("/mnt/files", ...). A no-op build (option off)
 * emits an empty stub that always returns 0 — main.c doesn't need
 * to #ifdef the call site.
 *
 * Patterned after sched_hef_init.c.
 */

#include "vfs.h"
#include "littlefs_slm.h"
#include "uart.h"
#include <stddef.h>

#if defined(ENABLE_USER_HEF_EMBED)

extern const unsigned char user_hef_start[];
extern const unsigned char user_hef_end[];

int user_hef_init(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt =
        (struct lfs_mount *)vfs_get_mount_ctx("/mnt/files", &subpath);
    if (!mnt) {
        uart_puts("[WARN] user_hef_init: /mnt/files not mounted\r\n");
        return -1;
    }

    size_t len = (size_t)(user_hef_end - user_hef_start);
    if (len == 0) {
        uart_puts("[WARN] user_hef_init: embedded .hef is zero bytes\r\n");
        return -1;
    }

    int f = littlefs_file_open(mnt, "/user.hef",
                               LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (f < 0) {
        uart_puts("[WARN] user_hef_init: open /user.hef failed\r\n");
        return -1;
    }
    int wrote = littlefs_file_write(mnt, f, user_hef_start, len);
    littlefs_file_close(mnt, f);

    if (wrote < 0 || (size_t)wrote != len) {
        /* Short write = LittleFS ENOSPC (ramdisk too small for the
         * HEF). Silently swallowing this leaves /mnt/files/user.hef
         * a 0-byte stub and every downstream `hailo load` fails
         * with a confusing "too small" error. Bump ramdisk block
         * count in ramdisk.h if this fires. */
        uart_puts("[ERR ] user_hef_init: short write — ramdisk too small\r\n");
        return -1;
    }

    uart_puts("[INFO] user_hef_init: wrote /mnt/files/user.hef\r\n");
    return 0;
}

#else /* !ENABLE_USER_HEF_EMBED */

int user_hef_init(void)
{
    return 0;
}

#endif
