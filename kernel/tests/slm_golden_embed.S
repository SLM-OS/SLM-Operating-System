/*
 * slm_golden_embed.S - Golden fixtures for kernel/tests/test_slm_golden.c.
 *
 * scripts/fixtures/slm/: the ggml reference vectors, a small Qwen2 GGUF and
 * llama.cpp's greedy output on it (about 860 KB together). Built only with
 * ENABLE_BOOT_TESTS, so production kernels carry no golden data. Paths come
 * from CMake COMPILE_DEFINITIONS as unquoted tokens (same pattern as
 * oplib_embed.S).
 */

#if !defined(SLM_GOLDEN_OPS_PATH) || !defined(SLM_GOLDEN_TINY_PATH) || \
    !defined(SLM_GOLDEN_GREEDY_PATH)
# error "SLM_GOLDEN_*_PATH must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

    .section .rodata

    .global slm_golden_ops_start
    .global slm_golden_ops_end
    .balign 16
slm_golden_ops_start:
    .incbin xstr(SLM_GOLDEN_OPS_PATH)
slm_golden_ops_end:

    .global slm_golden_tiny_start
    .global slm_golden_tiny_end
    .balign 16
slm_golden_tiny_start:
    .incbin xstr(SLM_GOLDEN_TINY_PATH)
slm_golden_tiny_end:

    .global slm_golden_greedy_start
    .global slm_golden_greedy_end
    .balign 16
slm_golden_greedy_start:
    .incbin xstr(SLM_GOLDEN_GREEDY_PATH)
slm_golden_greedy_end:

    .section .note.GNU-stack,"",%progbits
