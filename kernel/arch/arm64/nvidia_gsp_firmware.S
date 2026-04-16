/*
 * nvidia_gsp_firmware.S — Embed the NVIDIA GSP firmware blobs into
 * the ARM64 kernel ELF via `.incbin`. The blobs are generated at
 * build time by scripts/tools/extract-gsp-firmware.sh into
 * ${CMAKE_BINARY_DIR}/gsp_fw/ — NOT committed to the repo (proprietary
 * NVIDIA firmware).
 *
 * Four symbols per blob — start, end — so the C side can build
 * `struct gsp_firmware_blob` entries via pointer arithmetic.
 *
 * On Jetson Orin Nano (GA10B), firmware is extracted from
 * /lib/firmware/nvidia/ga10b/gsp/ on the build host. The file format
 * is identical to ga107 (x86-64 discrete) — same 535.113.01 version,
 * same RISC-V ELF + Falcon ucode layout.
 *
 * Total embedded size is ~38 MB (dominated by gsp.bin). Kernel binary
 * grows accordingly; this is expected for the SD-card boot image.
 *
 * GSP_FW_*_PATH must be defined by CMake to the absolute paths of the
 * extracted firmware files.
 */

#if !defined(GSP_FW_GSP_PATH) || !defined(GSP_FW_BOOTLOADER_PATH) || \
    !defined(GSP_FW_BOOTER_LOAD_PATH) || !defined(GSP_FW_BOOTER_UNLOAD_PATH)
# error "GSP_FW_*_PATH defines must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

.section .rodata.gsp_firmware, "a", %progbits
.balign 4096

.global gsp_fw_gsp_start
.global gsp_fw_gsp_end
gsp_fw_gsp_start:
    .incbin xstr(GSP_FW_GSP_PATH)
gsp_fw_gsp_end:

.global gsp_fw_bootloader_start
.global gsp_fw_bootloader_end
gsp_fw_bootloader_start:
    .incbin xstr(GSP_FW_BOOTLOADER_PATH)
gsp_fw_bootloader_end:

.global gsp_fw_booter_load_start
.global gsp_fw_booter_load_end
gsp_fw_booter_load_start:
    .incbin xstr(GSP_FW_BOOTER_LOAD_PATH)
gsp_fw_booter_load_end:

.global gsp_fw_booter_unload_start
.global gsp_fw_booter_unload_end
gsp_fw_booter_unload_start:
    .incbin xstr(GSP_FW_BOOTER_UNLOAD_PATH)
gsp_fw_booter_unload_end:
