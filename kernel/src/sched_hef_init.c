/*
 * sched_hef_init.c — Write the embedded scheduler MLP .hef into VFS at boot.
 *
 * Partners sched_hef_embed.S. When the kernel is built with
 * ENABLE_SCHEDULER_HEF_EMBED=ON + SCHEDULER_HEF_BLOB=<path>, the binary
 * bytes land in .rodata (sched_hef_start / sched_hef_end) and this
 * function writes them into /mnt/files/scheduler_mlp.hef so the shell's
 * `hailo load /mnt/files/scheduler_mlp.hef sched` path can reach them.
 *
 * Callable from main.c's boot sequence *after* vfs_init() /
 * littlefs_mount_at("/mnt/files", ...). A no-op build (option OFF)
 * emits an empty stub that always returns 0 — main.c doesn't need to
 * #ifdef the call site.
 */

#include "vfs.h"
#include "littlefs_slm.h"
#include "uart.h"
#include <stddef.h>

#if defined(ENABLE_SCHEDULER_HEF_EMBED)

extern const unsigned char sched_hef_start[];
extern const unsigned char sched_hef_end[];

int sched_hef_init(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt =
        (struct lfs_mount *)vfs_get_mount_ctx("/mnt/files", &subpath);
    if (!mnt) {
        uart_puts("[WARN] sched_hef_init: /mnt/files not mounted\r\n");
        return -1;
    }

    size_t len = (size_t)(sched_hef_end - sched_hef_start);
    if (len == 0) {
        uart_puts("[WARN] sched_hef_init: embedded .hef is zero bytes\r\n");
        return -1;
    }

    int f = littlefs_file_open(mnt, "/scheduler_mlp.hef",
                               LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (f < 0) {
        uart_puts("[WARN] sched_hef_init: open /scheduler_mlp.hef failed\r\n");
        return -1;
    }
    (void)littlefs_file_write(mnt, f, sched_hef_start, len);
    littlefs_file_close(mnt, f);

    uart_puts("[INFO] sched_hef_init: wrote /mnt/files/scheduler_mlp.hef\r\n");
    return 0;
}

#else /* !ENABLE_SCHEDULER_HEF_EMBED */

int sched_hef_init(void)
{
    return 0;
}

#endif
