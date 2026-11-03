/*
 * mnist_gpu_embed.S — Embed the inputs of the cold-boot MNIST GPU
 * pipeline (#1088, kernel/gpu/nvidia/ga10b_mnist_pipeline.c).
 *
 *   mnist_onnx_*         models/test/mnist.onnx (26 KB). The pipeline
 *                        reads its six weight initializers straight out
 *                        of the protobuf, so the GPU uses exactly the
 *                        weights the CPU path (runtime include_bytes!)
 *                        and scripts/mnist-extract-weights.py use.
 *   mnist_input_synth_*  scripts/fixtures/mnist_input_synth.bin, the
 *                        extraction script's deterministic synthetic
 *                        image (numpy default_rng(42) * 0.3, 3136 B),
 *                        the input buffer's initial content.
 *
 * Both files are tracked in git, so every build has them; built on all
 * platforms so the unit tests can check the ONNX walk on the real file.
 * Paths come from CMake COMPILE_DEFINITIONS as unquoted tokens (same
 * pattern as oplib_embed.S).
 */

#if !defined(MNIST_ONNX_PATH) || !defined(MNIST_INPUT_SYNTH_PATH)
# error "MNIST_ONNX_PATH and MNIST_INPUT_SYNTH_PATH must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

    .section .rodata

    .global mnist_onnx_start
    .global mnist_onnx_end
    .align 4
mnist_onnx_start:
    .incbin xstr(MNIST_ONNX_PATH)
mnist_onnx_end:

    .global mnist_input_synth_start
    .global mnist_input_synth_end
    .align 4
mnist_input_synth_start:
    .incbin xstr(MNIST_INPUT_SYNTH_PATH)
mnist_input_synth_end:

#if defined(__ELF__) && (defined(__x86_64__) || defined(__i386__))
    .section .note.GNU-stack, "", @progbits
#endif
