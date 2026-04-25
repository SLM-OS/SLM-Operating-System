/*
 * mnist_digit_init.c — Write embedded MNIST test digits into VFS at boot.
 *
 * Partners mnist_digit_embed.S. When the kernel is built with
 * ENABLE_MNIST_DIGITS_EMBED=ON + MNIST_DIGITS_DIR=<path>, ten test
 * digits land in .rodata (mnist_digit_<n>_start / _end) and this
 * function writes them into /mnt/files/digits/digit_<n>.bin so the
 * shell or Lua's slm.model_infer_file() can reach them.
 *
 * Callable from main.c's boot sequence after vfs_init() /
 * littlefs_mount_at("/mnt/files", ...). A no-op build (option OFF)
 * emits an empty stub that always returns 0 — main.c doesn't need
 * to #ifdef the call site.
 */

#include "vfs.h"
#include "littlefs_slm.h"
#include "uart.h"
#include <stddef.h>

int mnist_digit_init(void);

#if defined(ENABLE_MNIST_DIGITS_EMBED)

#define DIGIT_DECL(n)                                              \
    extern const unsigned char mnist_digit_##n##_start[];          \
    extern const unsigned char mnist_digit_##n##_end[];

DIGIT_DECL(0)
DIGIT_DECL(1)
DIGIT_DECL(2)
DIGIT_DECL(3)
DIGIT_DECL(4)
DIGIT_DECL(5)
DIGIT_DECL(6)
DIGIT_DECL(7)
DIGIT_DECL(8)
DIGIT_DECL(9)

struct digit_blob {
    const char           *path;
    const unsigned char  *start;
    const unsigned char  *end;
};

static int write_blob(struct lfs_mount *mnt, const struct digit_blob *b)
{
    size_t len = (size_t)(b->end - b->start);
    if (len == 0) return -1;

    int f = littlefs_file_open(mnt, b->path,
                               LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (f < 0) return -1;

    int rc = littlefs_file_write(mnt, f, b->start, len);
    littlefs_file_close(mnt, f);
    return rc < 0 ? -1 : 0;
}

int mnist_digit_init(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt =
        (struct lfs_mount *)vfs_get_mount_ctx("/mnt/files", &subpath);
    if (!mnt) {
        uart_puts("[WARN] mnist_digit_init: /mnt/files not mounted\r\n");
        return -1;
    }

    /* mkdir-equivalent: littlefs_mkdir returns 0 on success and is
     * idempotent for already-existing directories on this build. */
    (void)littlefs_mkdir(mnt, "/digits");

    static const struct digit_blob blobs[] = {
        {"/digits/digit_0.bin", NULL, NULL},
        {"/digits/digit_1.bin", NULL, NULL},
        {"/digits/digit_2.bin", NULL, NULL},
        {"/digits/digit_3.bin", NULL, NULL},
        {"/digits/digit_4.bin", NULL, NULL},
        {"/digits/digit_5.bin", NULL, NULL},
        {"/digits/digit_6.bin", NULL, NULL},
        {"/digits/digit_7.bin", NULL, NULL},
        {"/digits/digit_8.bin", NULL, NULL},
        {"/digits/digit_9.bin", NULL, NULL},
    };

    /* Patch the start/end pointers — extern arrays can't be used
     * as compile-time initializers for a static struct table. */
    struct digit_blob runtime_blobs[10];
    runtime_blobs[0] = (struct digit_blob){blobs[0].path, mnist_digit_0_start, mnist_digit_0_end};
    runtime_blobs[1] = (struct digit_blob){blobs[1].path, mnist_digit_1_start, mnist_digit_1_end};
    runtime_blobs[2] = (struct digit_blob){blobs[2].path, mnist_digit_2_start, mnist_digit_2_end};
    runtime_blobs[3] = (struct digit_blob){blobs[3].path, mnist_digit_3_start, mnist_digit_3_end};
    runtime_blobs[4] = (struct digit_blob){blobs[4].path, mnist_digit_4_start, mnist_digit_4_end};
    runtime_blobs[5] = (struct digit_blob){blobs[5].path, mnist_digit_5_start, mnist_digit_5_end};
    runtime_blobs[6] = (struct digit_blob){blobs[6].path, mnist_digit_6_start, mnist_digit_6_end};
    runtime_blobs[7] = (struct digit_blob){blobs[7].path, mnist_digit_7_start, mnist_digit_7_end};
    runtime_blobs[8] = (struct digit_blob){blobs[8].path, mnist_digit_8_start, mnist_digit_8_end};
    runtime_blobs[9] = (struct digit_blob){blobs[9].path, mnist_digit_9_start, mnist_digit_9_end};

    int written = 0;
    for (int i = 0; i < 10; i++) {
        if (write_blob(mnt, &runtime_blobs[i]) == 0) written++;
    }

    if (written == 10) {
        uart_puts("[INFO] mnist_digit_init: wrote 10 digits to /mnt/files/digits/\r\n");
    } else {
        uart_puts("[WARN] mnist_digit_init: partial write to /mnt/files/digits/\r\n");
    }
    return written == 10 ? 0 : -1;
}

#else /* !ENABLE_MNIST_DIGITS_EMBED */

int mnist_digit_init(void)
{
    return 0;
}

#endif
