/*
 * nvidia_ga10b_firmware.S — embed GA10B nvgpu-native firmware via
 * `.incbin`. The blobs live outside the repo (NVIDIA-proprietary);
 * the build host must fetch them via scripts/tools/fetch-ga10b-firmware.sh
 * and point CMake at the resulting directory with -DGA10B_FIRMWARE_DIR=...
 *
 * Unlike the discrete-Ampere nvidia_gsp_firmware.S (4 blobs), GA10B
 * needs ~14 files covering ACR, FECS, GPCCS, PMU, NET images, and the
 * safety scheduler. Each gets its own start/end symbol pair.
 *
 * Only the blobs actually consumed by the bringup code are pulled in.
 * The NET images + safety scheduler are embedded for completeness
 * but the minimum bringup path only needs ACR + FECS + GPCCS + PMU.
 *
 * Per-blob absolute paths must be passed in via CMake:
 *   GA10B_FW_ACR_TEXT_PATH
 *   GA10B_FW_ACR_DATA_PATH
 *   GA10B_FW_ACR_MANIFEST_PATH
 *   GA10B_FW_FECS_PATH
 *   GA10B_FW_FECS_SIG_PATH
 *   GA10B_FW_GPCCS_PATH
 *   GA10B_FW_GPCCS_SIG_PATH
 *   GA10B_FW_PMU_IMAGE_PATH
 *   GA10B_FW_PMU_DESC_PATH
 *   GA10B_FW_PMU_SIG_PATH
 *   GA10B_FW_NET_A_PATH
 *   GA10B_FW_NET_B_PATH
 *   GA10B_FW_NET_C_PATH
 *   GA10B_FW_NET_D_PATH
 *   GA10B_FW_SAFETY_TEXT_PATH
 *   GA10B_FW_SAFETY_DATA_PATH
 *   GA10B_FW_SAFETY_MANIFEST_PATH
 */

#if !defined(GA10B_FW_ACR_TEXT_PATH)
# error "GA10B_FW_*_PATH defines must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

.section .rodata.ga10b_firmware, "a", %progbits
.balign 256

/* ---- ACR-GSP (root-of-trust ucode on GSP Falcon) ---- */
.global ga10b_fw_acr_text_start
.global ga10b_fw_acr_text_end
ga10b_fw_acr_text_start:
    .incbin xstr(GA10B_FW_ACR_TEXT_PATH)
ga10b_fw_acr_text_end:

.global ga10b_fw_acr_data_start
.global ga10b_fw_acr_data_end
ga10b_fw_acr_data_start:
    .incbin xstr(GA10B_FW_ACR_DATA_PATH)
ga10b_fw_acr_data_end:

.global ga10b_fw_acr_manifest_start
.global ga10b_fw_acr_manifest_end
ga10b_fw_acr_manifest_start:
    .incbin xstr(GA10B_FW_ACR_MANIFEST_PATH)
ga10b_fw_acr_manifest_end:

/* ---- FECS (GR front-end context-switch ucode) ---- */
.global ga10b_fw_fecs_start
.global ga10b_fw_fecs_end
ga10b_fw_fecs_start:
    .incbin xstr(GA10B_FW_FECS_PATH)
ga10b_fw_fecs_end:

.global ga10b_fw_fecs_sig_start
.global ga10b_fw_fecs_sig_end
ga10b_fw_fecs_sig_start:
    .incbin xstr(GA10B_FW_FECS_SIG_PATH)
ga10b_fw_fecs_sig_end:

/* ---- GPCCS (per-GPC context-switch ucode) ---- */
.global ga10b_fw_gpccs_start
.global ga10b_fw_gpccs_end
ga10b_fw_gpccs_start:
    .incbin xstr(GA10B_FW_GPCCS_PATH)
ga10b_fw_gpccs_end:

.global ga10b_fw_gpccs_sig_start
.global ga10b_fw_gpccs_sig_end
ga10b_fw_gpccs_sig_start:
    .incbin xstr(GA10B_FW_GPCCS_SIG_PATH)
ga10b_fw_gpccs_sig_end:

/* ---- PMU ---- */
.global ga10b_fw_pmu_image_start
.global ga10b_fw_pmu_image_end
ga10b_fw_pmu_image_start:
    .incbin xstr(GA10B_FW_PMU_IMAGE_PATH)
ga10b_fw_pmu_image_end:

.global ga10b_fw_pmu_desc_start
.global ga10b_fw_pmu_desc_end
ga10b_fw_pmu_desc_start:
    .incbin xstr(GA10B_FW_PMU_DESC_PATH)
ga10b_fw_pmu_desc_end:

.global ga10b_fw_pmu_sig_start
.global ga10b_fw_pmu_sig_end
ga10b_fw_pmu_sig_start:
    .incbin xstr(GA10B_FW_PMU_SIG_PATH)
ga10b_fw_pmu_sig_end:

/* ---- NET runtime images (4 variants) ---- */
.global ga10b_fw_net_a_start
.global ga10b_fw_net_a_end
ga10b_fw_net_a_start:
    .incbin xstr(GA10B_FW_NET_A_PATH)
ga10b_fw_net_a_end:

.global ga10b_fw_net_b_start
.global ga10b_fw_net_b_end
ga10b_fw_net_b_start:
    .incbin xstr(GA10B_FW_NET_B_PATH)
ga10b_fw_net_b_end:

.global ga10b_fw_net_c_start
.global ga10b_fw_net_c_end
ga10b_fw_net_c_start:
    .incbin xstr(GA10B_FW_NET_C_PATH)
ga10b_fw_net_c_end:

.global ga10b_fw_net_d_start
.global ga10b_fw_net_d_end
ga10b_fw_net_d_start:
    .incbin xstr(GA10B_FW_NET_D_PATH)
ga10b_fw_net_d_end:

/* ---- Safety scheduler (Tegra-safety variant) ---- */
.global ga10b_fw_safety_text_start
.global ga10b_fw_safety_text_end
ga10b_fw_safety_text_start:
    .incbin xstr(GA10B_FW_SAFETY_TEXT_PATH)
ga10b_fw_safety_text_end:

.global ga10b_fw_safety_data_start
.global ga10b_fw_safety_data_end
ga10b_fw_safety_data_start:
    .incbin xstr(GA10B_FW_SAFETY_DATA_PATH)
ga10b_fw_safety_data_end:

.global ga10b_fw_safety_manifest_start
.global ga10b_fw_safety_manifest_end
ga10b_fw_safety_manifest_start:
    .incbin xstr(GA10B_FW_SAFETY_MANIFEST_PATH)
ga10b_fw_safety_manifest_end:
