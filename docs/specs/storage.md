# Storage / Filesystem — Fact Sheet

Block devices, VFS, filesystem backends.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Block device backend | RAM disk (1 MB) | RAM disk (1 MB) **+ BCM2712 EMMC2 SDHCI on real hardware** | RAM disk (1 MB) | RAM disk (1 MB) |
| Block device drivers | `kernel/drivers/ramdisk.c` | `kernel/drivers/ramdisk.c`, `kernel/drivers/sdhci.c` (BCM2712 EMMC2) | `kernel/drivers/ramdisk.c` | `kernel/drivers/ramdisk.c` |
| Persistent storage | ❌ (RAM-only) | ✅ FAT32 boot partition on SD card | ❌ | ❌ |
| Filesystems in tree | LittleFS | LittleFS + FatFs (FAT12/16/32, `kernel/lib/fatfs/`) | LittleFS | LittleFS |
| LittleFS version | v2.x via `kernel/lib/littlefs/` | Same | Same | Same |
| FatFs version | — | R0.15 (vendored, used for boot FAT) | — | — |
| `/mnt/files` (workspace) | LittleFS RAM-backed | LittleFS — prefers `0:/slmstore/files.lfs` on boot FAT, falls back to RAM | LittleFS RAM-backed | LittleFS RAM-backed |
| Boot-media auth root | — | `0:/slmstore/` on FAT32 boot partition (FatFs drive 0) — autoload blobs, persistent LFS image, staged kernels | — | — |
| Boot-media gate | — | `boot_media_acquire/release` refcount; gate opens after `vmm_init` (#414) | — | — |
| VFS backends | RAM disk + LittleFS | RAM disk + LittleFS + FatFs | RAM disk + LittleFS | RAM disk + LittleFS |
| File operations | open/close/read/write/seek/stat/mkdir/rm/rename | Same | Same | Same |
| Max path length | `VFS_PATH_MAX` (256) | Same | Same | Same |
| Shell file ops | `ls cd pwd cat write mkdir rm mv cp touch stat tree wc hexdump grep find df truncate append` | Same | Same | Same |
| File handle generation counter | ✅ (prevents stale-handle reuse, 8dee772) | ✅ | ✅ | ✅ |
| VFS lock | Global spinlock | Same | Same | Same |
| Model file loading from VFS | ONNX via `model load <path>` | Same | Same | Same |
| Embedded assets | Lua scripts, ONNX models, AI scheduler weights via `.incbin` | Same | Same | Same |

## Skipped / Blocked

- **Real block device drivers (eMMC, NVMe) on non-Pi platforms** — deferred. Pi 5 BCM2712 EMMC2 SDHCI is now in tree (`kernel/drivers/sdhci.c`, PR #389) and drives FAT32 access on the SD boot partition. Other platforms still rely on RAM disk:
  - Jetson: eMMC controller (behind CBB — untested)
  - x86-64: NVMe or SATA AHCI driver
- **Pi 5 `pmm_free_pages` free-list page fault under stealing (#166)** — closed. Was a generic PMM/spinlock issue, not storage-specific.
- **LittleFS file handle reuse with stale file state** — closed 2026-04-17 (8dee772) via per-handle generation counter.
- **Persistent `/mnt/files` on Jetson/x86-64/QEMU** — gated on a real block device for those platforms.
- **Journal / transaction-aware ops** — LittleFS has its own power-loss-safety model; not exposed to shell as explicit transactions.
- **File permissions / ownership** — no users, no permissions, no ACLs. Single-user bare-metal.
- **Directory iteration callbacks** — `find` shell command supports basic wildcard; no `readdir` opendir()-style API in shell.

## See also

- `docs/filesystem.md` (narrative; persistent LFS on boot FAT)
- `docs/vfs.md` (VFS design)
- `docs/dynamic-kernel-replace-plan.md` (boot_media + SDHCI + tryboot kernel staging)
- `kernel/lib/littlefs/` (LittleFS source)
- `kernel/lib/fatfs/` (FatFs source)
- `kernel/drivers/ramdisk.c` (RAM disk)
- `kernel/drivers/sdhci.c`, `kernel/include/sdhci.h` (BCM2712 EMMC2 SDHCI, Pi 5)

*Last updated: 28 April 2026*
