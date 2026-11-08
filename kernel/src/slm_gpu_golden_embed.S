/*
 * slm_gpu_golden_embed.S - Fixtures for `slm gpu golden` (#475), built with
 * SLM_GPU_GOLDEN=ON. Paths come from CMake as unquoted tokens, as in
 * kernel/tests/slm_golden_embed.S. .gguf content must be page aligned for the
 * loader's copy to be mapped, which rust_slm_load arranges itself.
 */
#define xstr(s) str(s)
#define str(s) #s

    .section .rodata
    .global slm_gpu_golden_tiny_start
    .global slm_gpu_golden_tiny_end
    .balign 16
slm_gpu_golden_tiny_start:
    .incbin xstr(SLM_GOLDEN_TINY_PATH)
slm_gpu_golden_tiny_end:

    .global slm_gpu_golden_rec_start
    .global slm_gpu_golden_rec_end
    .balign 16
slm_gpu_golden_rec_start:
    .incbin xstr(SLM_GOLDEN_GREEDY_PATH)
slm_gpu_golden_rec_end:
