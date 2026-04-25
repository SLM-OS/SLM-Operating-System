/*
 * diskio.c — FatFs-to-blkdev shim.
 *
 * Bridges FatFs's sector-based diskio.h interface to SLM-OS's
 * `struct blkdev` (kernel/include/blkdev.h). Single-disk for now
 * (`pdrv == 0`); multi-disk would live in Stage 3+ once the SDHCI
 * driver is wired in alongside the ramdisk stub.
 *
 * The attached blkdev MUST use 512-byte blocks (matches FF_MIN_SS /
 * FF_MAX_SS in ffconf.h and the production SD card). FatFs's
 * `LBA_t sector` index maps 1:1 to blkdev's `block` index; FatFs's
 * `count` maps to a single multi-block read by passing
 * `count * block_size` as the byte length with `off = 0`.
 *
 * Multi-partition: FF_MULTI_PARTITION = 1, so FatFs consults the
 * VolToPart[] table to map a logical volume to a (drive, partition)
 * tuple. Volume 0 → physical drive 0, partition 1.
 */

#include "ff.h"
#include "diskio.h"

#include "../../include/blkdev.h"

/* Owned by the kernel-side fat32_attach()/_detach() API. NULL until
 * a backing blkdev is wired in. Single disk = single global pointer;
 * keep the shim small. */
static struct blkdev *fatfs_attached_dev = NULL;

void fatfs_disk_attach(struct blkdev *dev)
{
    fatfs_attached_dev = dev;
}

void fatfs_disk_detach(void)
{
    fatfs_attached_dev = NULL;
}

struct blkdev *fatfs_disk_current(void)
{
    return fatfs_attached_dev;
}

/* ---- FatFs-required entry points. ---- */

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0) {
        return STA_NOINIT;
    }
    return fatfs_attached_dev ? 0 : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0) {
        return STA_NOINIT;
    }
    return fatfs_attached_dev ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !fatfs_attached_dev) {
        return RES_NOTRDY;
    }
    /* Sectors map directly to blkdev blocks. The blkdev read API
     * spans block boundaries internally, so a multi-sector read is
     * a single call with off = 0 and size = count * block_size. */
    int rc = fatfs_attached_dev->ops->read(
        fatfs_attached_dev,
        (uint32_t)sector, 0,
        buff,
        (uint32_t)count * fatfs_attached_dev->block_size);
    return (rc == BLKDEV_OK) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !fatfs_attached_dev) {
        return RES_NOTRDY;
    }
    if (!fatfs_attached_dev->ops->prog) {
        return RES_WRPRT;
    }
    int rc = fatfs_attached_dev->ops->prog(
        fatfs_attached_dev,
        (uint32_t)sector, 0,
        buff,
        (uint32_t)count * fatfs_attached_dev->block_size);
    return (rc == BLKDEV_OK) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0 || !fatfs_attached_dev) {
        return RES_NOTRDY;
    }
    switch (cmd) {
    case CTRL_SYNC:
        if (fatfs_attached_dev->ops->sync) {
            int rc = fatfs_attached_dev->ops->sync(fatfs_attached_dev);
            return (rc == BLKDEV_OK) ? RES_OK : RES_ERROR;
        }
        return RES_OK;

    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = (LBA_t)fatfs_attached_dev->block_count;
        return RES_OK;

    case GET_SECTOR_SIZE:
        /* Documented constraint: only meaningful when FF_MAX_SS !=
         * FF_MIN_SS. We pin both at 512 in ffconf.h, but FatFs still
         * calls this on f_mkfs() so answer it. */
        *(WORD *)buff = (WORD)fatfs_attached_dev->block_size;
        return RES_OK;

    case GET_BLOCK_SIZE:
        /* Erase block size in *sectors*. blkdev's block_size is the
         * erase unit in *bytes*; for a 512-byte-sector device it's
         * always 1 sector per erase block (or larger on flash, but
         * we use ramdisk + SD where 1 is correct). */
        *(DWORD *)buff = 1;
        return RES_OK;

    default:
        return RES_PARERR;
    }
}

/* ---- Volume-to-partition mapping. ---- */

#if FF_MULTI_PARTITION

PARTITION VolToPart[FF_VOLUMES] = {
    /* { physical drive, partition }
     *   partition: 0 = auto/single-volume, 1-4 = primary partition,
     *              5+ = logical partition.
     *
     * Lab Pi 5 SD card layout (docs/pi5-dual-boot-setup.md:29-35):
     *   pdrv 0, partition 1 = SLMOS (FAT32) — the volume we mount.
     */
    { 0, 1 },
};

#endif /* FF_MULTI_PARTITION */

/* ---- Time stamping. ----
 *
 * With FF_FS_NORTC = 1, FatFs uses the FF_NORTC_* values from
 * ffconf.h directly and never calls get_fattime(). The function
 * is still declared in ff.h, so define a fallback that simply
 * returns the same fixed timestamp — defensive in case a future
 * config flip enables RTC mode without an actual time source. */

DWORD get_fattime(void)
{
    /* DOS time format:
     *   bits[31:25] year - 1980
     *   bits[24:21] month (1-12)
     *   bits[20:16] day   (1-31)
     *   bits[15:11] hour  (0-23)
     *   bits[10: 5] minute(0-59)
     *   bits[ 4: 0] second / 2  (0-29 = 0..58)
     */
    return ((DWORD)(2026 - 1980) << 25)
         | ((DWORD)4              << 21)
         | ((DWORD)25             << 16);
}
