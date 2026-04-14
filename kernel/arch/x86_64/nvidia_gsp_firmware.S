/*
 * nvidia_gsp_firmware.S — Embed the NVIDIA GSP firmware blobs into
 * the x86-64 kernel ELF via `.incbin`. The blobs are generated at
 * build time by scripts/tools/extract-gsp-firmware.sh into
 * ${CMAKE_BINARY_DIR}/gsp_fw/ — NOT committed to the repo (proprietary
 * NVIDIA firmware).
 *
 * Four symbols per blob — start, end, size — so the C side can build
 * `struct gsp_firmware_blob` entries via pointer arithmetic.
 *
 * Total embedded size is ~38 MB (dominated by gsp.bin). Kernel binary
 * grows accordingly; that's expected and fits within the 64 MB ESP
 * partition the slmos-x86-disk target produces.
 *
 * GSP_FW_DIR must be defined by CMake to the absolute path to the
 * extracted firmware directory.
 */

#if !defined(GSP_FW_GSP_PATH) || !defined(GSP_FW_BOOTLOADER_PATH) || \
    !defined(GSP_FW_BOOTER_LOAD_PATH) || !defined(GSP_FW_BOOTER_UNLOAD_PATH)
# error "GSP_FW_*_PATH defines must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

.section .rodata.gsp_firmware, "a", @progbits
.align 4096

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

.section .note.GNU-stack, "", @progbits
