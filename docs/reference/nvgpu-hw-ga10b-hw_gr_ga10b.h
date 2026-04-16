/*
 * Excerpt from OE4T/linux-nvgpu @ l4t/l4t-r36.5
 *   drivers/gpu/nvgpu/include/nvgpu/hw/ga10b/hw_gr_ga10b.h
 *   drivers/gpu/nvgpu/include/nvgpu/hw/ga10b/hw_pwr_ga10b.h
 *   drivers/gpu/nvgpu/include/nvgpu/gr/gr_falcon.h
 *
 * Source URL:
 *   https://raw.githubusercontent.com/OE4T/linux-nvgpu/l4t/l4t-r36.5/
 *     drivers/gpu/nvgpu/include/nvgpu/hw/ga10b/hw_gr_ga10b.h
 *
 * Registers needed for FECS / GPCCS / PMU STARTCPU + wait-for-ready bringup on
 * GA10B (Jetson Orin Nano). SPDX: MIT (same license as upstream nvgpu).
 */

/* ---- FECS falcon (BAR0 + 0x00409000 window) ------------------------------ */

#define gr_fecs_cpuctl_r()                                         (0x00409100U)
#define gr_fecs_cpuctl_startcpu_f(v)                     ((U32(v) & 0x1U) << 1U)
#define gr_fecs_cpuctl_alias_r()                                   (0x00409130U)

#define gr_fecs_dmactl_r()                                         (0x0040910cU)
#define gr_fecs_dmactl_require_ctx_f(v)                  ((U32(v) & 0x1U) << 0U)
#define gr_fecs_dmactl_dmem_scrubbing_m()                      (U32(0x1U) << 1U)
#define gr_fecs_dmactl_imem_scrubbing_m()                      (U32(0x1U) << 2U)

#define gr_fecs_bootvec_r()                                        (0x00409104U)
#define gr_fecs_bootvec_vec_f(v)                  ((U32(v) & 0xffffffffU) << 0U)

#define gr_fecs_falcon_hwcfg_r()                                   (0x00409108U)
/* NOTE: gr_fecs_hwcfg2_r() is NOT defined in GA10B's hw_gr_ga10b.h.
 * Turing/Volta had it; Ampere restructured. Use gr_fecs_falcon_hwcfg_r() or
 * the FALCON2 hwcfg2 regs from falcon2_gr_falcon_ga10b.c instead. */

#define gr_fecs_current_ctx_r()                                    (0x00409b00U)
#define gr_fecs_current_ctx_ptr_f(v)               ((U32(v) & 0xfffffffU) << 0U)
#define gr_fecs_current_ctx_ptr_v(r)                  (((r) >> 0U) & 0xfffffffU)
#define gr_fecs_current_ctx_target_vid_mem_f()                            (0x0U)
#define gr_fecs_current_ctx_target_sys_mem_coh_f()                 (0x20000000U)
#define gr_fecs_current_ctx_target_sys_mem_ncoh_f()                (0x30000000U)
#define gr_fecs_current_ctx_valid_f(v)                  ((U32(v) & 0x1U) << 31U)
#define gr_fecs_current_ctx_valid_false_f()                               (0x0U)

#define gr_fecs_mailbox0_r()                                       (0x00409040U)
#define gr_fecs_mailbox1_r()                                       (0x00409044U)

/* FECS context-switch mailboxes: 18 mailboxes at 0x00409800 + i*4 */
#define gr_fecs_ctxsw_mailbox_r(i) \
        (nvgpu_safe_add_u32(0x00409800U, nvgpu_safe_mult_u32((i), 4U)))
#define gr_fecs_ctxsw_mailbox__size_1_v()                          (0x00000012U)  /* 18 */
#define gr_fecs_ctxsw_mailbox_value_f(v)          ((U32(v) & 0xffffffffU) << 0U)
#define gr_fecs_ctxsw_mailbox_value_pass_v()                       (0x00000001U)
#define gr_fecs_ctxsw_mailbox_value_fail_v()                       (0x00000002U)
#define gr_fecs_ctxsw_mailbox_value_ctxsw_checksum_mismatch_v()    (0x00000021U)

/* ---- GPCCS (broadcast) falcon ------------------------------------------- */

#define gr_gpccs_cpuctl_r()                                        (0x0041a100U)
#define gr_gpccs_cpuctl_startcpu_f(v)                    ((U32(v) & 0x1U) << 1U)
#define gr_gpccs_dmactl_r()                                        (0x0041a10cU)
#define gr_gpccs_dmactl_require_ctx_f(v)                 ((U32(v) & 0x1U) << 0U)
#define gr_gpccs_dmactl_imem_scrubbing_m()                     (U32(0x1U) << 2U)
#define gr_gpcs_gpccs_falcon_hwcfg_r()                             (0x0041a108U)

/* Broadcast GPCCS ctxsw mailboxes */
#define gr_gpccs_ctxsw_mailbox_r(i) \
        (nvgpu_safe_add_u32(0x0041a800U, nvgpu_safe_mult_u32((i), 4U)))
#define gr_gpccs_ctxsw_mailbox__size_1_v()                         (0x00000012U)  /* 18 */
#define gr_gpccs_ctxsw_mailbox_value_f(v)         ((U32(v) & 0xffffffffU) << 0U)

/* Per-GPC0 GPCCS ctxsw mailboxes (used for bootstrap completion polling) */
#define gr_gpc0_gpccs_ctxsw_mailbox_r(i) \
        (nvgpu_safe_add_u32(0x00502800U, nvgpu_safe_mult_u32((i), 4U)))

/* ---- PMU / pwr falcon (hw_pwr_ga10b.h) ---------------------------------- */

#define pwr_falcon_mailbox0_r()                                    (0x0010a040U)
#define pwr_falcon_mailbox1_r()                                    (0x0010a044U)
#define pwr_falcon_cpuctl_r()                                      (0x0010a100U)
#define pwr_falcon_cpuctl_startcpu_f(v)                  ((U32(v) & 0x1U) << 1U)
#define pwr_falcon_cpuctl_alias_r()                                (0x0010a130U)

/* ---- Ctxsw mailbox slot assignments (gr_falcon.h) ----------------------- */
/* Software contract between nvgpu host driver and FECS/GPCCS ucode. */
#define NVGPU_GR_FALCON_FECS_CTXSW_MAILBOX0  0U
#define NVGPU_GR_FALCON_FECS_CTXSW_MAILBOX1  1U
#define NVGPU_GR_FALCON_FECS_CTXSW_MAILBOX2  2U
#define NVGPU_GR_FALCON_FECS_CTXSW_MAILBOX4  4U
#define NVGPU_GR_FALCON_FECS_CTXSW_MAILBOX6  6U
#define NVGPU_GR_FALCON_FECS_CTXSW_MAILBOX7  7U

/* Bootstrap-complete sentinel:
 *   The only mailbox *value* constant defined in hw_gr_ga10b.h is _pass_v()
 *   == 0x00000001. No 0xC0C0 / BOOT_OK / INIT_DONE symbols appear in the
 *   GA10B public headers. In nvgpu ga10b gr_falcon.c, FECS bootstrap success
 *   is signalled by mailbox[0] == 1 (pass_v). The legacy 0xC0C0 / "deadbeef"
 *   style sentinels are not used on Ampere. */
