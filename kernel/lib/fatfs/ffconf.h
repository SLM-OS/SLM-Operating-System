/*
 * ffconf.h — SLM-OS configuration for FatFs R0.16.
 *
 * Tailored for the dynamic-kernel-replace plan:
 *   - FAT32 read/write only (no exFAT, no FAT12/16 init).
 *   - Long filename support (kernel_2712.img is 11 chars, exceeds 8.3).
 *   - ANSI/OEM char set (CP437) — no Unicode strings on the API.
 *   - Single 512-byte-sector volume bound to a `struct blkdev`.
 *   - MBR multi-partition mode so volume 0 maps to partition 1 of the
 *     SD card (matches the lab's dual-boot SD layout — see
 *     docs/pi5-dual-boot-setup.md:29-35).
 *   - Single-thread, no file lock, no RTC.
 *
 * For test builds f_mkfs() and f_fdisk() are needed (the test
 * formats an in-memory ramdisk fresh on each run). FF_USE_MKFS is
 * therefore enabled even for the production kernel — production
 * never calls f_mkfs(), and the linker only pulls in what is used.
 */

#ifndef _FFCONF_DEFINED
#define _FFCONF_DEFINED
#define FFCONF_DEF	80386	/* Revision ID; matches FF_DEFINED in ff.h */


/*-----------------------------------------------------------------*/
/* Function configurations                                         */
/*-----------------------------------------------------------------*/

#define FF_FS_READONLY	0
/* Read+write — needed for kernel stage / promote / rollback. */

#define FF_FS_MINIMIZE	0
/* 0 = full feature set (f_unlink, f_rename, f_mkdir, f_chmod,
 * f_truncate, f_stat, f_getfree, f_chdir). All are used by the
 * admin commands or their tests. */

#define FF_USE_FIND		0
#define FF_USE_MKFS		1	/* Tests format ramdisk fresh on each run. */
#define FF_USE_FASTSEEK	0
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	0
#define FF_USE_LABEL	0
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	0


/*-----------------------------------------------------------------*/
/* Locale and namespace configurations                             */
/*-----------------------------------------------------------------*/

#define FF_CODE_PAGE	437
/* CP437 (US-ASCII compatible). Static SBCS — only the CP437 table
 * in ffunicode.c is compiled in; the linker drops the others. */

#define FF_USE_LFN		1
#define FF_MAX_LFN		64
/* LFN with a static work area. 64 chars is generous — kernel_2712.img
 * is 15 chars; budget covers config.txt-sidecar paths if added later. */

#define FF_LFN_UNICODE	0
/* 0 = ANSI/OEM API (1-byte char strings). Avoids dragging UTF-16 /
 * UTF-8 conversion paths into the kernel. */

#define FF_LFN_BUF		255
#define FF_SFN_BUF		12
/* These are *informational* bounds used by FatFs to size internal
 * structures; the actual API path-length cap is FF_MAX_LFN. Keep at
 * the documented defaults so any path that fits FF_MAX_LFN works. */

#define FF_FS_RPATH		0	/* No relative paths or chdir state. */


/*-----------------------------------------------------------------*/
/* Drive / volume configurations                                   */
/*-----------------------------------------------------------------*/

#define FF_VOLUMES		1
#define FF_STR_VOLUME_ID	0

#define FF_MULTI_PARTITION	1
/* Required: the lab SD card is MBR + 3 primary partitions (SLMOS at
 * partition 1, PIOS_BOOT at 2, PIOS_ROOT at 3). FatFs needs to mount
 * partition 1 specifically, not auto-detect. The VolToPart[] table
 * is defined in kernel/lib/fatfs/diskio.c. */

#define FF_MIN_SS		512
#define FF_MAX_SS		512
/* 512-byte sectors — what the Pi 5 EMMC2 SDHCI controller and the
 * existing lab card use. */

#define FF_LBA64		0
/* 32-bit LBA. The lab card's largest production partition (the
 * ~7.5 GB SLMOS partition) is ~15.7 M sectors at 512 B — orders of
 * magnitude below the 2 TiB / 4 G-sector LBA32 ceiling. The 48 MB
 * test ramdisk is only 98304 sectors. Bump to FF_LBA64=1 only if a
 * future production SD partition exceeds 2 TiB. */

#define FF_MIN_GPT		0x10000000
#define FF_USE_TRIM		0


/*-----------------------------------------------------------------*/
/* System configurations                                           */
/*-----------------------------------------------------------------*/

#define FF_FS_TINY		0
#define FF_FS_EXFAT		0
#define FF_FS_NORTC		1
#define FF_NORTC_MON	4
#define FF_NORTC_MDAY	25
#define FF_NORTC_YEAR	2026
/* No kernel RTC — every newly-created file gets timestamped
 * 2026-04-25 00:00. Updated on each major plan-doc revision so file
 * dates roughly track the build era. */

#define FF_FS_NOFSINFO	0
#define FF_FS_LOCK		0
#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	0
#define FF_FS_CRTIME	0


#endif /* _FFCONF_DEFINED */
