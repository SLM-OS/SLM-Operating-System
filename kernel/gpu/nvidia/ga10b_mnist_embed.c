/*
 * ga10b_mnist_embed.c - Jetson glue for the cold-boot MNIST GPU
 * pipeline (#1088): sources from the kernel image, install on the cold
 * channel. See ga10b_mnist_pipeline.h.
 */
#include "ga10b_mnist_pipeline.h"
#include "ga10b_channel_handoff.h"
#include "ga10b_bringup.h"

#include "debug.h"
#include "gpu_handoff.h"        /* SLM_GPU_OP_*, SLM_GPU_TIER_*, SLM_GPU_DTYPE_* */
#include "oplib_pool.h"
#include "operator_library.h"
#include "string.h"

#include <stdint.h>

extern const uint8_t mnist_onnx_start[];
extern const uint8_t mnist_onnx_end[];
extern const uint8_t mnist_input_synth_start[];
extern const uint8_t mnist_input_synth_end[];

static const uint32_t shader_op_kind[GA10B_MNIST_N_SHADERS] = {
    [GA10B_MNIST_SH_CONV]    = SLM_GPU_OP_CONV2D,
    [GA10B_MNIST_SH_ADDRELU] = SLM_GPU_OP_ADD_BIAS,
    [GA10B_MNIST_SH_POOL]    = SLM_GPU_OP_MAXPOOL,
    [GA10B_MNIST_SH_GEMM]    = SLM_GPU_OP_GEMM_GENERIC,
};

static const char *const shader_name[GA10B_MNIST_N_SHADERS] = {
    "conv2d_fp32_direct", "add_bias_relu_fp32", "maxpool2d_fp32", "gemm_fp32",
};

int ga10b_mnist_sources_embedded(struct ga10b_mnist_sources *src)
{
    if (!src) return -1;
    memset(src, 0, sizeof(*src));

    int rc = oplib_pool_init();
    if (rc != 0) {
        WARN("mnist-gpu: operator library unusable (rc=%d)", rc);
        return -1;
    }
    for (unsigned i = 0; i < GA10B_MNIST_N_SHADERS; i++) {
        const uint8_t *sass = NULL;
        size_t size = 0;
        rc = oplib_pool_lookup(shader_op_kind[i], SLM_GPU_TIER_SIMT,
                               SLM_GPU_DTYPE_FP32, &sass, &size);
        if (rc != 0 || !sass || size == 0u || size > GA10B_MNIST_SHADER_BUF_BYTES) {
            WARN("mnist-gpu: %s SASS (SIMT FP32) not in the operator library "
                 "(rc=%d size=%lu); copy scripts/cuda/operator_library/*.sass "
                 "from a Jetson and rebuild", shader_name[i], rc,
                 (unsigned long)size);
            return -1;
        }
        src->sass[i] = sass;
        src->sass_bytes[i] = (uint32_t)size;
    }

    size_t onnx_len = (size_t)(mnist_onnx_end - mnist_onnx_start);
    rc = ga10b_mnist_weights_from_onnx(mnist_onnx_start, onnx_len, src);
    if (rc != GA10B_ONNX_OK) {
        WARN("mnist-gpu: weights not found in embedded mnist.onnx (rc=%d, %lu B)",
             rc, (unsigned long)onnx_len);
        return -1;
    }

    size_t in_len = (size_t)(mnist_input_synth_end - mnist_input_synth_start);
    src->input = (in_len == GA10B_MNIST_INPUT_BYTES) ? mnist_input_synth_start : NULL;
    return 0;
}

static struct ga10b_mnist_pipeline cold_mnist;

int ga10b_mnist_cold_install(const struct ga10b_gpu_ops *ops, struct ga10b_chan *ch)
{
    if (!ops || !ch || !ch->enabled) return GA10B_MNIST_ERR_ARG;
    if (cold_mnist.built) {
        WARN("mnist-gpu: pipeline already built");
        return GA10B_MNIST_ERR_ARG;
    }

    struct ga10b_mnist_sources src;
    if (ga10b_mnist_sources_embedded(&src) != 0) return -1;

    int rc = ga10b_mnist_pipeline_build(ops, &ch->vm, &src, &cold_mnist);
    if (rc != GA10B_MNIST_OK) {
        WARN("mnist-gpu: pipeline build failed rc=%d", rc);
        return rc;
    }

    struct ga10b_channel_handoff h;
    ga10b_mnist_pipeline_fill_handoff(&cold_mnist, ch, &h);
    if (ga10b_handoff_is_v7(&h) == false || ga10b_v7_validate_handoff(&h) != GA10B_V7_OK ||
        slm_gpu_install_handoff(ga10b_bringup_state(), &h) != 0) {
        WARN("mnist-gpu: v7 handoff rejected");
        /* The pipeline's pages stay mapped in the channel's VM, so they
         * are leaked rather than freed (no stale PTEs to reused memory). */
        return -1;
    }

    INFO("mnist-gpu: 8-op pipeline installed (v7): ops 0x%lx, input 0x%lx, "
         "logits 0x%lx, QMD pool 0x%lx (%u slots), shaders %u/%u/%u/%u B",
         (unsigned long)h.pipeline_ops_phys, (unsigned long)h.input_buf_phys,
         (unsigned long)cold_mnist.out[GA10B_MNIST_N_OPS - 1u].phys,
         (unsigned long)h.qmd_pool_phys, (unsigned)h.qmd_pool_n_slots,
         (unsigned)cold_mnist.sass_bytes[0], (unsigned)cold_mnist.sass_bytes[1],
         (unsigned)cold_mnist.sass_bytes[2], (unsigned)cold_mnist.sass_bytes[3]);
    if (!src.input) {
        WARN("mnist-gpu: embedded synthetic input missing; input buffer starts zeroed");
    }
    return 0;
}
