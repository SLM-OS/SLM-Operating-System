# Storage / Filesystem — Fact Sheet

Block devices, VFS, filesystem backends.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Block device backend | RAM disk (1 MB) | RAM disk (1 MB) | RAM disk (1 MB) | RAM disk (1 MB) |
| Block device driver | `kernel/drivers/ramdisk.c` | Same | Same | Same |
| Persistent storage | ❌ (RAM-only) | ❌ | ❌ | ❌ |
| Filesystem | LittleFS | LittleFS | LittleFS | LittleFS |
| LittleFS version | v2.x via `kernel/fs/littlefs/` | Same | Same | Same |
| Mount points | `/mnt/files` | Same | Same | Same |
| VFS backends | RAM disk + LittleFS | Same | Same | Same |
| File operations | open/close/read/write/seek/stat/mkdir/rm/rename | Same | Same | Same |
| Max path length | `VFS_PATH_MAX` (256) | Same | Same | Same |
| Shell file ops | `ls cd pwd cat write mkdir rm mv cp touch stat tree wc hexdump grep find df truncate append` | Same | Same | Same |
| File handle generation counter | ✅ (prevents stale-handle reuse, 8dee772) | ✅ | ✅ | ✅ |
| VFS lock | Global spinlock | Same | Same | Same |
| Model file loading from VFS | ONNX via `model load <path>` | Same | Same | Same |
| Embedded assets | Lua scripts, ONNX models, AI scheduler weights via `.incbin` | Same | Same | Same |

## Skipped / Blocked

- **Real block device drivers (SD card, eMMC, NVMe)** — deferred. The RAM-disk model is sufficient for the current demo/test workflow. Adding real storage would require:
  - Pi 5: SD host controller at the ARM-side EMMC2 block, or MMC over RP1
  - Jetson: eMMC controller (behind CBB — untested)
  - x86-64: NVMe or SATA AHCI driver
- **Pi 5 `pmm_free_pages` free-list page fault under stealing (#166)** — closed. Was a generic PMM/spinlock issue, not storage-specific.
- **LittleFS file handle reuse with stale file state** — closed 2026-04-17 (8dee772) via per-handle generation counter.
- **Multi-filesystem mount points** — VFS supports it but only LittleFS is currently mounted.
- **Journal / transaction-aware ops** — LittleFS has its own power-loss-safety model; not exposed to shell as explicit transactions.
- **File permissions / ownership** — no users, no permissions, no ACLs. Single-user bare-metal.
- **Directory iteration callbacks** — `find` shell command supports basic wildcard; no `readdir` opendir()-style API in shell.

## See also

- `docs/filesystem.md` (narrative)
- `docs/vfs.md` (VFS design)
- `kernel/fs/littlefs/` (LittleFS source)
- `kernel/drivers/ramdisk.c` (block device)

*Last updated: 18 April 2026*
