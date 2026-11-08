/*
 * slm_real_golden_embed.S - The fixture for `slm golden` (#1165), built with
 * SLM_REAL_GOLDEN=ON: llama.cpp's greedy tokens and the exact CPU reference's
 * logits for the real Qwen2.5-1.5B model (scripts/fixtures/slm/
 * qwen25_real_golden.rec, about 2.4 MB). The model itself comes from
 * /boot/models. The path comes from CMake as an unquoted token, as in
 * kernel/tests/slm_golden_embed.S.
 */
#if !defined(SLM_REAL_GOLDEN_PATH)
# error "SLM_REAL_GOLDEN_PATH must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

    .section .rodata
    .global slm_real_golden_rec_start
    .global slm_real_golden_rec_end
    .balign 16
slm_real_golden_rec_start:
    .incbin xstr(SLM_REAL_GOLDEN_PATH)
slm_real_golden_rec_end:

    .section .note.GNU-stack,"",%progbits
