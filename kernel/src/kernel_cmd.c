/*
 * kernel_cmd.c — `kernel` admin shell command surface.
 *
 * Stage 4 of the dynamic-kernel-replace plan (#370). See
 * `kernel/include/kernel_cmd.h` for the public API and the
 * subcommand contract; `docs/dynamic-kernel-replace-plan.md` for
 * the overall design.
 *
 * Backend wiring goes through `boot_media_acquire/release`
 * (kernel/src/boot_media.c) so the SDHCI controller is created
 * once and pinned via the keep-alive ref for the kernel's
 * lifetime. boot_media's platform dispatch picks
 * `sdhci_create_bcm2712()` on Pi 5 and `sdhci_create_qemu_pci()` on
 * QEMU virt; on other platforms acquire returns NULL and every
 * subcommand reports "no boot partition" cleanly.
 *
 * Sub-task 5 of #371 surfaced the reason for routing through
 * boot_media: `sdhci_create_bcm2712()` issues a BCM mailbox
 * SET_CLOCK_STATE on every call, and on `pieeprom-2024-09-23.bin`
 * that mailbox tag starts returning code 0x00000000 (failure)
 * after a few rapid create→destroy cycles, breaking back-to-back
 * `kernel stage`/`rollback`. The keep-alive ref keeps the
 * controller created and the firmware mailbox quiet between
 * commands.
 */

#include "platform.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "kernel_cmd.h"
#include "shell.h"
#include "blkdev.h"
#include "vfs.h"
#include "pmm.h"
#include "config.h"
#include "debug.h"
#include "sha256.h"
#include "fat32.h"
#include "boot_media.h"
#include "../lib/fatfs/ff.h"
#include "smp.h"            /* psci_system_reset */
#include "build_info.h"     /* SLMOS_VERSION etc. */

#if defined(PLATFORM_RASPI5)
#include "bcm_mailbox.h"   /* tryboot mailbox tags (Stage 1) */
#endif

/* ---- Constants. ---- */

#define VOL                       "0:"
#define KERNEL_IMG_PATH           VOL "/kernel_2712.img"
#define TRYBOOT_IMG_PATH          VOL "/tryboot.img"
#define TRYBOOT_SHA_PATH          VOL "/tryboot.sha"

/* Cap a staged kernel image at 16 MB. The Pi 5 production kernel
 * is around 1–4 MB; 16 MB is a generous header against ELF growth
 * without committing arbitrarily large PMM allocations. */
#define KERNEL_STAGE_MAX_BYTES    (16u * 1024u * 1024u)

/* Streaming buffer for VFS → FatFs copy. One PMM page; small
 * enough that a stage of a 4 MB image needs ~1024 chunks but each
 * chunk's I/O is bounded. */
#define STAGE_CHUNK_BYTES         PAGE_SIZE

/* ---- Boot-partition lifecycle helpers. ---- */

/* Single static FATFS for the volume; FatFs is non-reentrant
 * (FF_FS_REENTRANT=0) and the shell dispatcher serializes
 * mutating commands, so one struct is sufficient. */
static FATFS g_kernel_fs;
static struct blkdev *g_kernel_dev;

/*
 * Probe the SDHCI controller, attach to FatFs, and mount partition
 * 1. Returns 0 on success, negative on any failure (controller
 * absent, no card, no FAT32 partition). Caller MUST pair every
 * successful return with `boot_volume_unmount()` before returning
 * to the shell — even error paths after mount need to detach so
 * subsequent subcommands aren't fighting a stuck volume.
 *
 * Goes through `boot_media_acquire()` rather than calling the
 * platform-specific create function directly. This matters on Pi 5
 * (#371 sub-task 5): the BCM mailbox `SET_CLOCK_STATE` request that
 * `sdhci_create_bcm2712()` issues to enable EMMC2 starts returning
 * code 0x00000000 (failure) after a few rapid create→destroy
 * cycles, which made back-to-back `kernel stage`/`rollback`
 * commands fail. `boot_media`'s keep-alive ref pins the controller
 * after the first successful create so subsequent acquires reuse
 * the cached device, and the firmware mailbox isn't re-toggled.
 */
static int boot_volume_mount(void)
{
    if (g_kernel_dev) {
        ERROR("kernel: boot volume already mounted (lifecycle bug)");
        return -1;
    }
    g_kernel_dev = boot_media_acquire();
    if (!g_kernel_dev) {
        return -1;
    }
    fatfs_disk_attach(g_kernel_dev);
    FRESULT res = f_mount(&g_kernel_fs, VOL, /*opt=*/1);
    if (res != FR_OK) {
        ERROR("kernel: f_mount(\"%s\") = %d (no FAT32 on partition 1?)",
              VOL, res);
        fatfs_disk_detach();
        boot_media_release(g_kernel_dev);
        g_kernel_dev = NULL;
        return -1;
    }
    return 0;
}

static void boot_volume_unmount(void)
{
    if (!g_kernel_dev) {
        return;
    }
    f_mount(NULL, VOL, 0);
    fatfs_disk_detach();
    boot_media_release(g_kernel_dev);
    g_kernel_dev = NULL;
}

/* ---- File-existence helpers. ---- */

static bool fat_file_exists(const char *path)
{
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;
}

/* Hex-encode is provided by `kernel/lib/sha256.c::sha256_bytes_to_hex`
 * (added on main alongside the vendored SHA-256 lib). The .sha
 * sidecar uses it directly. */

/* ---- subcommand: status ---- */

static int cmd_kernel_status(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* Always report the running kernel's identity, even if the
     * boot volume can't be mounted — tells the operator which
     * build is on the wire regardless of card state. */
    shell_printf("running kernel: SLM-OS " SLMOS_VERSION
                 " (build " SLMOS_BUILD_STAMP ", sha " SLMOS_BUILD_SHA ")\n");

    if (boot_volume_mount() < 0) {
        shell_puts("boot volume:    unavailable (no SDHCI controller, "
                   "no card, or no FAT32 on partition 1)\n");
        return -1;
    }

    bool has_tryboot = fat_file_exists(TRYBOOT_IMG_PATH);
    bool has_sha     = fat_file_exists(TRYBOOT_SHA_PATH);
    bool has_kernel  = fat_file_exists(KERNEL_IMG_PATH);

    if (has_tryboot && has_sha) {
        FILINFO fno;
        f_stat(TRYBOOT_IMG_PATH, &fno);
        shell_printf("staged image:   tryboot.img (%lu bytes), "
                     "tryboot.sha present\n",
                     (unsigned long)fno.fsize);
    } else if (has_tryboot) {
        shell_puts("staged image:   tryboot.img present BUT tryboot.sha "
                   "missing — re-stage to repair\n");
    } else if (has_sha) {
        shell_puts("staged image:   tryboot.sha present BUT tryboot.img "
                   "missing — orphan sidecar (rollback to clean up)\n");
    } else {
        shell_puts("staged image:   none\n");
    }

    if (has_kernel) {
        FILINFO fno;
        f_stat(KERNEL_IMG_PATH, &fno);
        shell_printf("active kernel:  kernel_2712.img (%lu bytes)\n",
                     (unsigned long)fno.fsize);
    } else {
        shell_puts("active kernel:  kernel_2712.img missing — card may "
                   "be unformatted\n");
    }

    boot_volume_unmount();
    return 0;
}

/* ---- subcommand: stage ---- */

/*
 * Read a chunk of the source file from the VFS into `buf`,
 * resuming at `offset`. Returns the bytes read, 0 on EOF, -1 on
 * error.
 */
static int stage_read_chunk(const char *src, size_t offset,
                            uint8_t *buf, size_t want)
{
    int got = vfs_read_path(src, (char *)buf, want, offset);
    return got;
}

static int cmd_kernel_stage(int argc, char *argv[])
{
    if (argc != 1) {
        shell_puts("usage: kernel stage <vfs-path>\n");
        return -1;
    }
    const char *src = argv[0];

    /* Stat source for size validation + buffer sizing. */
    struct vfs_entry_info info;
    if (vfs_stat_path(src, &info) < 0) {
        shell_printf("stage: source not found: %s\n", src);
        return -1;
    }
    if (info.type != 0) {  /* 0 = file */
        shell_printf("stage: %s is not a file\n", src);
        return -1;
    }
    if (info.size == 0) {
        shell_puts("stage: source file is empty\n");
        return -1;
    }
    if (info.size > KERNEL_STAGE_MAX_BYTES) {
        shell_printf("stage: source %u bytes exceeds %u-byte cap\n",
                     info.size, KERNEL_STAGE_MAX_BYTES);
        return -1;
    }

    uint32_t total = info.size;

    /* Allocate the streaming buffer once. */
    void *chunk = pmm_alloc_pages(1);
    if (!chunk) {
        shell_puts("stage: PMM exhausted allocating stream buffer\n");
        return -1;
    }

    if (boot_volume_mount() < 0) {
        shell_puts("stage: boot volume unavailable\n");
        pmm_free_pages(chunk, 1);
        return -1;
    }

    /* Open the destination file. CREATE_ALWAYS truncates if a stale
     * tryboot.img is on the card — re-staging is idempotent. */
    FIL fp;
    FRESULT res = f_open(&fp, TRYBOOT_IMG_PATH,
                         FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        shell_printf("stage: f_open(%s) = %d\n", TRYBOOT_IMG_PATH, res);
        boot_volume_unmount();
        pmm_free_pages(chunk, 1);
        return -1;
    }

    /* Stream copy + incremental SHA-256. */
    struct sha256_ctx hash;
    sha256_init(&hash);

    int rc = 0;
    uint32_t copied = 0;
    while (copied < total) {
        uint32_t want = total - copied;
        if (want > STAGE_CHUNK_BYTES) want = STAGE_CHUNK_BYTES;

        int got = stage_read_chunk(src, copied, chunk, want);
        if (got < 0) {
            shell_printf("stage: VFS read failed at offset %u\n", copied);
            rc = -1; break;
        }
        if (got == 0) {
            shell_printf("stage: short read at offset %u (expected %u more)\n",
                         copied, want);
            rc = -1; break;
        }

        sha256_update(&hash, chunk, (size_t)got);

        UINT written = 0;
        res = f_write(&fp, chunk, (UINT)got, &written);
        if (res != FR_OK || written != (UINT)got) {
            shell_printf("stage: f_write at offset %u failed (rc=%d, %u/%d)\n",
                         copied, res, written, got);
            rc = -1; break;
        }

        copied += (uint32_t)got;
    }

    f_close(&fp);

    if (rc == 0) {
        /* Write the sidecar as 64 hex chars + newline + NUL. */
        uint8_t digest[SHA256_DIGEST_LEN];
        sha256_final(&hash, digest);

        /* sha256_bytes_to_hex writes SHA256_HEX_LEN chars + NUL.
         * The sidecar wants a trailing newline too, hence the +2. */
        char hex[SHA256_HEX_LEN + 2];
        sha256_bytes_to_hex(digest, hex);
        hex[SHA256_HEX_LEN] = '\n';
        hex[SHA256_HEX_LEN + 1] = '\0';

        FIL sidecar;
        res = f_open(&sidecar, TRYBOOT_SHA_PATH,
                     FA_WRITE | FA_CREATE_ALWAYS);
        if (res == FR_OK) {
            UINT written = 0;
            f_write(&sidecar, hex, SHA256_HEX_LEN + 1, &written);
            f_close(&sidecar);
            if (written != SHA256_HEX_LEN + 1) {
                shell_puts("stage: short write of tryboot.sha sidecar\n");
                rc = -1;
            }
        } else {
            shell_printf("stage: f_open(%s) = %d\n", TRYBOOT_SHA_PATH, res);
            rc = -1;
        }

        if (rc == 0) {
            shell_printf("staged %u bytes from %s; sha = ",
                         total, src);
            shell_puts(hex);  /* trailing newline included */
        }
    }

    /* On failure leave any partial tryboot.img on the card; the next
     * `kernel rollback` cleans it up. Better than silently deleting
     * — the operator can inspect what's there. */

    boot_volume_unmount();
    pmm_free_pages(chunk, 1);
    return rc;
}

/* ---- subcommand: activate ---- */

static int cmd_kernel_activate(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* Sanity-check that there's something to activate. */
    if (boot_volume_mount() < 0) {
        shell_puts("activate: boot volume unavailable\n");
        return -1;
    }
    bool has_tryboot = fat_file_exists(TRYBOOT_IMG_PATH);
    boot_volume_unmount();
    if (!has_tryboot) {
        shell_puts("activate: no tryboot.img staged — run `kernel stage` first\n");
        return -1;
    }

#if defined(PLATFORM_RASPI5)
    /* Arm tryboot via the Pi 5 firmware mailbox. */
    int rc = bcm_mailbox_set_reboot_flags(1);
    if (rc != 0) {
        shell_printf("activate: SET_REBOOT_FLAGS failed (rc=%d)\n", rc);
        return -1;
    }
    rc = bcm_mailbox_notify_reboot();
    if (rc != 0) {
        shell_printf("activate: NOTIFY_REBOOT failed (rc=%d)\n", rc);
        return -1;
    }
    shell_puts("activate: tryboot armed; resetting...\n");
    psci_system_reset();
    /* unreachable */
    return 0;
#else
    /* On QEMU virt and other platforms there is no `[tryboot]`
     * firmware path. Document that and trigger the reset anyway so
     * the test harness can verify the flow up to the reset call. */
    shell_puts("activate: no tryboot mailbox on this platform; "
               "would reset on Pi 5\n");
    return 0;
#endif
}

/* ---- subcommand: promote ---- */

static int cmd_kernel_promote(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (boot_volume_mount() < 0) {
        shell_puts("promote: boot volume unavailable\n");
        return -1;
    }

    if (!fat_file_exists(TRYBOOT_IMG_PATH)) {
        shell_puts("promote: no tryboot.img staged\n");
        boot_volume_unmount();
        return -1;
    }

    int rc = 0;

    /* f_rename can't overwrite — unlink the existing kernel first.
     * If it doesn't exist that's still OK (FR_NO_FILE → ignore). */
    FRESULT res = f_unlink(KERNEL_IMG_PATH);
    if (res != FR_OK && res != FR_NO_FILE) {
        shell_printf("promote: f_unlink(%s) = %d\n", KERNEL_IMG_PATH, res);
        rc = -1;
        goto out;
    }

    res = f_rename(TRYBOOT_IMG_PATH, KERNEL_IMG_PATH);
    if (res != FR_OK) {
        shell_printf("promote: f_rename failed (rc=%d)\n", res);
        rc = -1;
        goto out;
    }

    /* The .sha sidecar described the bytes that were just promoted —
     * keeping it under the old name would lie about the live kernel.
     * Best-effort: don't fail promote on sha-cleanup error, but
     * report it. */
    res = f_unlink(TRYBOOT_SHA_PATH);
    if (res != FR_OK && res != FR_NO_FILE) {
        shell_printf("promote: warning — f_unlink(%s) = %d\n",
                     TRYBOOT_SHA_PATH, res);
    }

    shell_puts("promote: tryboot.img → kernel_2712.img\n");

out:
    boot_volume_unmount();
    return rc;
}

/* ---- subcommand: rollback ---- */

static int cmd_kernel_rollback(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (boot_volume_mount() < 0) {
        shell_puts("rollback: boot volume unavailable\n");
        return -1;
    }

    /* Best-effort cleanup of the staged-state files. Both unlinks
     * are non-fatal — rollback should converge "no candidate" even
     * if the card was already in that state. */
    FRESULT res_img = f_unlink(TRYBOOT_IMG_PATH);
    FRESULT res_sha = f_unlink(TRYBOOT_SHA_PATH);

    boot_volume_unmount();

#if defined(PLATFORM_RASPI5)
    /* Clear the firmware tryboot flag in case `kernel activate` was
     * already called but the user changed their mind before the
     * actual reboot fired. Best-effort. */
    int mb = bcm_mailbox_set_reboot_flags(0);
    if (mb != 0) {
        shell_printf("rollback: warning — clearing tryboot flag failed "
                     "(rc=%d)\n", mb);
    }
#endif

    if (res_img == FR_OK || res_sha == FR_OK) {
        shell_puts("rollback: cleared staged image\n");
    } else {
        shell_puts("rollback: nothing was staged\n");
    }
    return 0;
}

/* ---- top-level dispatch ---- */

static int cmd_kernel(int argc, char *argv[])
{
    if (argc < 2) {
        shell_puts("usage: kernel <status|stage|activate|promote|rollback>\n");
        return -1;
    }
    const char *sub = argv[1];
    int sub_argc = argc - 2;
    char **sub_argv = argv + 2;

    if (strcmp(sub, "status") == 0) {
        return cmd_kernel_status(sub_argc, sub_argv);
    }
    if (strcmp(sub, "stage") == 0) {
        return cmd_kernel_stage(sub_argc, sub_argv);
    }
    if (strcmp(sub, "activate") == 0) {
        return cmd_kernel_activate(sub_argc, sub_argv);
    }
    if (strcmp(sub, "promote") == 0) {
        return cmd_kernel_promote(sub_argc, sub_argv);
    }
    if (strcmp(sub, "rollback") == 0) {
        return cmd_kernel_rollback(sub_argc, sub_argv);
    }
    shell_printf("kernel: unknown subcommand `%s`\n", sub);
    return -1;
}

static const shell_cmd_t kernel_commands[] = {
    /* mutates=true so the dispatcher serializes concurrent admin
     * sessions — `kernel stage`/promote/rollback all touch the
     * shared SD-card state machine. `kernel status` is read-only
     * but rides under the same name; the cost of serialisation is
     * negligible for an admin command. */
    { "kernel", cmd_kernel,
      "Manage staged / active boot kernel (status/stage/activate/promote/rollback)",
      true, SHELL_CAT_HARDWARE },
};

void kernel_cmd_register_shell(void)
{
    for (size_t i = 0;
         i < sizeof(kernel_commands) / sizeof(kernel_commands[0]); i++) {
        shell_register_command(&kernel_commands[i]);
    }
}
