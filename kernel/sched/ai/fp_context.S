/*
 * fp_context.S - FP/SIMD state save/restore for AI scheduler
 *
 * Saves and restores V0-V31 + FPCR/FPSR around AI inference calls
 * to prevent FP register corruption in interrupt context.
 * Full implementation in M6.
 */

#if defined(__aarch64__)

.global fp_save
.global fp_restore

/*
 * void fp_save(struct fp_state *state)
 *
 * Save all 32 SIMD registers (V0-V31) plus FPCR and FPSR.
 * state->regs is at offset 0 (32 * 16 = 512 bytes)
 * state->fpcr is at offset 512
 * state->fpsr is at offset 516
 */
fp_save:
    stp  q0,  q1,  [x0, #0]
    stp  q2,  q3,  [x0, #32]
    stp  q4,  q5,  [x0, #64]
    stp  q6,  q7,  [x0, #96]
    stp  q8,  q9,  [x0, #128]
    stp  q10, q11, [x0, #160]
    stp  q12, q13, [x0, #192]
    stp  q14, q15, [x0, #224]
    stp  q16, q17, [x0, #256]
    stp  q18, q19, [x0, #288]
    stp  q20, q21, [x0, #320]
    stp  q22, q23, [x0, #352]
    stp  q24, q25, [x0, #384]
    stp  q26, q27, [x0, #416]
    stp  q28, q29, [x0, #448]
    stp  q30, q31, [x0, #480]
    mrs  x1, fpcr
    str  w1, [x0, #512]
    mrs  x1, fpsr
    str  w1, [x0, #516]
    ret

/*
 * void fp_restore(const struct fp_state *state)
 *
 * Restore all 32 SIMD registers plus FPCR and FPSR.
 */
fp_restore:
    ldp  q0,  q1,  [x0, #0]
    ldp  q2,  q3,  [x0, #32]
    ldp  q4,  q5,  [x0, #64]
    ldp  q6,  q7,  [x0, #96]
    ldp  q8,  q9,  [x0, #128]
    ldp  q10, q11, [x0, #160]
    ldp  q12, q13, [x0, #192]
    ldp  q14, q15, [x0, #224]
    ldp  q16, q17, [x0, #256]
    ldp  q18, q19, [x0, #288]
    ldp  q20, q21, [x0, #320]
    ldp  q22, q23, [x0, #352]
    ldp  q24, q25, [x0, #384]
    ldp  q26, q27, [x0, #416]
    ldp  q28, q29, [x0, #448]
    ldp  q30, q31, [x0, #480]
    ldr  w1, [x0, #512]
    msr  fpcr, x1
    ldr  w1, [x0, #516]
    msr  fpsr, x1
    ret

#elif defined(__x86_64__)

.global fp_save
.global fp_restore

/*
 * x86-64: use FXSAVE/FXRSTOR (512-byte state area, 16-byte aligned)
 * struct fp_state on x86-64 must be at least 512 bytes, 16-byte aligned.
 */
fp_save:
    fxsave (%rdi)
    ret

fp_restore:
    fxrstor (%rdi)
    ret

#else
#error "Unsupported architecture for fp_context.S"
#endif
