/*
 * gpu-launch-common.h — Shared scaffolding for the Linux-userspace
 * compute-kernel launchers on Jetson GA10B (scripts/gpu-kernel-launch.c,
 * scripts/gpu-kernel-dot4.c, and any future per-kernel launcher).
 *
 * The two existing launchers started as copy-paste of each other and
 * drifted into ~85% duplication. This header + its companion .c file
 * consolidate the common pieces — channel setup, QMD population,
 * pushbuffer building, GPFIFO submit + poll, handoff write — so a
 * fix applied to the submit path lands once, not per-kernel.
 *
 * Per-kernel launchers should keep:
 *   - The kernel's shader path + expected-output payload.
 *   - Kernel-specific cbuf layout (which pointers go where in
 *     cbuf[0][0x160..]).
 *   - Kernel-specific extra buffers (dot4 has `a`, `b` in addition
 *     to `out`; write_cafe has only `out`).
 *   - main() orchestration.
 *
 * Everything else — channel bringup, the 13-dword dispatch
 * pushbuffer, the GPFIFO + doorbell + poll loop, handoff serialization
 * — lives here.
 */
#ifndef GPU_LAUNCH_COMMON_H
#define GPU_LAUNCH_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "/usr/src/nvidia/nvgpu/include/uapi/linux/nvgpu.h"
#include "/usr/src/nvidia/nvgpu/include/uapi/linux/nvgpu-as.h"
#include "/usr/src/nvidia/nvgpu/include/uapi/linux/nvgpu-ctrl.h"
#include "/usr/src/nvidia/nvidia-oot/include/uapi/linux/nvmap.h"

#include "../kernel/gpu/nvidia/ga10b_channel_handoff.h"
#include "gpu-qmd-bits.h"  /* gpu_qmd_set_bits — pure-logic, host-testable */

/* ============================================================
 * Class + subchannel mapping (per Mesa NVK's nv_push.h — compute
 * classes on subch 1, graphics classes on subch 0)
 * ============================================================ */
#define GPU_LAUNCH_CLASS_AMPERE_COMPUTE_B  0xC7C0
#define GPU_LAUNCH_SUBCH_COMPUTE           1

/* ============================================================
 * USERD layout (Ampere, from hw_ram_ga10b.h). Declared once so
 * the offsets baked into the handoff and the direct dword-array
 * reads use the same constants.
 * ============================================================ */
#define GPU_LAUNCH_USERD_GP_PUT_WORD  35u
#define GPU_LAUNCH_USERD_GP_GET_WORD  34u

/* ============================================================
 * nvmap allocation knobs (captured from CUDA's ioctl stream via
 * LD_PRELOAD snoop on L4T r36.4.7). Must stay in lock-step with
 * scripts/gpu-channel-helper.c — SLM-OS inherits channels from
 * either helper and expects nvmap buffers with the same caching
 * properties either way.
 * ============================================================ */
#define GPU_LAUNCH_NVMAP_IOVMM_HEAP_MASK  0x40000000u
#define GPU_LAUNCH_NVMAP_CACHEABLE_FLAGS  0x8000003u

/* ============================================================
 * NVC7C0 method offsets used by the dispatch pushbuffer.
 * ============================================================ */
#define NVC7C0_SET_OBJECT                             0x0000
#define NVC7C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI 0x0244
#define NVC7C0_INVALIDATE_SKED_CACHES                 0x0298
#define NVC7C0_SET_SHADER_SHARED_MEMORY_WINDOW_A      0x02a0
#define NVC7C0_SET_SHADER_SHARED_MEMORY_WINDOW_B      0x02a4
#define NVC7C0_SEND_PCAS_A                            0x02b4
/* Ampere (cls_compute > TURING_COMPUTE_A) dispatches via PCAS2_B
 * with a composite action rather than PCAS_B's two-bit
 * invalidate/schedule. See docs/reference/mesa-nvk_cmd_dispatch.c:
 * 322-340 — Ampere branch emits SEND_SIGNALING_PCAS2_B with
 * action=INVALIDATE_COPY_SCHEDULE (0xA). Using PCAS_B on GA10B
 * silently no-ops the dispatch even though the pushbuffer is
 * consumed; pinned by host test
 * test_launch_kernel_pb_uses_ampere_pcas2_b. */
#define NVC7C0_SEND_SIGNALING_PCAS2_B                 0x02c0
#define NVC7C0_SEND_SIGNALING_PCAS2_B_ACTION_INVALIDATE_COPY_SCHEDULE 0xA
#define NVC7C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A       0x07b0
#define NVC7C0_SET_SHADER_LOCAL_MEMORY_WINDOW_B       0x07b4

/* ============================================================
 * Pushbuffer header helpers (inline — tiny, pure-bit-twiddling).
 * ============================================================ */
#define GPU_HDR_INC(count, subch, byte_off)                          \
    ((1u << 29) | (((uint32_t)(count) & 0x1FFF) << 16) |             \
     (((uint32_t)(subch) & 0x7) << 13) |                             \
     (((uint32_t)(byte_off) >> 2) & 0x1FFF))

#define GPU_HDR_IMMD(subch, byte_off, data)                          \
    ((4u << 29) | (((uint32_t)(data) & 0x1FFF) << 16) |              \
     (((uint32_t)(subch) & 0x7) << 13) |                             \
     (((uint32_t)(byte_off) >> 2) & 0x1FFF))

/* ============================================================
 * QMDV03_00 field bit ranges (Ampere), from
 * docs/reference/mesa-clc7c0qmd.h.
 * ============================================================ */
#define QMD_MAJOR_VERSION_HI              583
#define QMD_MAJOR_VERSION_LO              580
#define QMD_VERSION_HI                    579
#define QMD_VERSION_LO                    576
#define QMD_API_VISIBLE_CALL_LIMIT_BIT    378
#define QMD_SAMPLER_INDEX_BIT             382
#define QMD_BARRIER_COUNT_HI              767
#define QMD_BARRIER_COUNT_LO              763
#define QMD_CTA_RASTER_WIDTH_HI           415
#define QMD_CTA_RASTER_WIDTH_LO           384
#define QMD_CTA_RASTER_HEIGHT_HI          431
#define QMD_CTA_RASTER_HEIGHT_LO          416
#define QMD_CTA_RASTER_DEPTH_HI           463
#define QMD_CTA_RASTER_DEPTH_LO           448
#define QMD_CTA_THREAD_DIM0_HI            607
#define QMD_CTA_THREAD_DIM0_LO            592
#define QMD_CTA_THREAD_DIM1_HI            623
#define QMD_CTA_THREAD_DIM1_LO            608
#define QMD_CTA_THREAD_DIM2_HI            639
#define QMD_CTA_THREAD_DIM2_LO            624
#define QMD_REGISTER_COUNT_V_HI           656
#define QMD_REGISTER_COUNT_V_LO           648
#define QMD_SHARED_MEMORY_SIZE_HI         561
#define QMD_SHARED_MEMORY_SIZE_LO         544
#define QMD_SHADER_LOCAL_MEM_LOW_SIZE_HI  759
#define QMD_SHADER_LOCAL_MEM_LOW_SIZE_LO  736
#define QMD_SHADER_LOCAL_MEM_HIGH_SIZE_HI 1623
#define QMD_SHADER_LOCAL_MEM_HIGH_SIZE_LO 1600
#define QMD_PROGRAM_ADDRESS_LOWER_HI      1567
#define QMD_PROGRAM_ADDRESS_LOWER_LO      1536
#define QMD_PROGRAM_ADDRESS_UPPER_HI      1584
#define QMD_PROGRAM_ADDRESS_UPPER_LO      1568
#define QMD_CBUF_ADDR_LO_BASE             1024  /* per-idx: + idx*64 */
#define QMD_CBUF_ADDR_HI_BASE             1056  /* + idx*64, width 17 */
#define QMD_CBUF_SIZE_SHIFTED4_BASE       1075  /* + idx*64, width 13 */
#define QMD_CBUF_VALID_BASE               640   /* + idx*1 */
#define QMD_CBUF_PREFETCH_POST_BASE       1073  /* + idx*64 */
#define QMD_CBUF_INVALIDATE_BASE          1074  /* + idx*64 */
#define QMD_INVALIDATE_TEXTURE_HEADER_CACHE_BIT   186
#define QMD_INVALIDATE_TEXTURE_SAMPLER_CACHE_BIT  187
#define QMD_INVALIDATE_TEXTURE_DATA_CACHE_BIT     188
#define QMD_INVALIDATE_SHADER_DATA_CACHE_BIT      189
#define QMD_INVALIDATE_INSTRUCTION_CACHE_BIT      190
#define QMD_INVALIDATE_SHADER_CONSTANT_CACHE_BIT  191
#define QMD_SM_GLOBAL_CACHING_ENABLE_BIT          134

/* Default CBUF[0] size for kernels launched via this scaffolding.
 * 512 B is enough for CUDA's param region up to offset 0x1F8 — every
 * kernel shipped so far (write_cafe's 1-pointer arg, dot4's 3-pointer
 * arg list ending at 0x178) fits. If a kernel needs more, override
 * CBUF_VALID / CBUF_SIZE_SHIFTED4 after gpu_launch_populate_qmd(). */
#define GPU_LAUNCH_CBUF_SIZE_B  512u

/* REGISTER_COUNT_V default. CUDA cubins for write_cafe and dot4 both
 * report REGCOUNT=126 in .nv.info; 128 is a round upper bound. Every
 * launcher may override after gpu_launch_populate_qmd() if its
 * kernel needs more or fewer registers. */
#define GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT  128u

/* ============================================================
 * Opaque bundle of state produced by gpu_launch_setup(). Launchers
 * treat this as read-only after setup; only g_handoff fields are
 * written post-setup (handoff path) and USERD dwords advance through
 * the submit loop.
 * ============================================================ */
struct gpu_launch_ctx {
    /* nvgpu / nvmap fds. */
    int nvmap_fd;
    int ctrl_fd;
    int as_fd;
    int tsg_fd;
    int ch_fd;
    int notifier_dmabuf;

    /* Dmabuf fds for the six common buffers. */
    int userd_dmabuf;
    int gpfifo_dmabuf;
    int pb_dmabuf;
    int qmd_dmabuf;
    int shader_dmabuf;
    int cbuf_dmabuf;

    /* CPU mappings. */
    void *userd_va;
    void *gpfifo_va;
    void *pb_va;
    void *qmd_va;
    void *shader_va;
    void *cbuf_va;

    /* GPU virtual addresses (post nvgpu AS mapping). */
    uint64_t pb_gva;
    uint64_t qmd_gva;
    uint64_t shader_gva;
    uint64_t cbuf_gva;

    /* Values returned by NVGPU_IOCTL_CHANNEL_SETUP_BIND. */
    uint64_t gpfifo_gpu_va;
    uint32_t work_submit_token;
    uint32_t gpfifo_entries;

    /* Shader size in bytes (from gpu_load_file in setup). */
    size_t shader_size_bytes;

    /* Doorbell page (mmap of the ctrl fd). */
    void *doorbell_page;
};

/* Per-kernel extra buffer (for inputs/outputs beyond the common
 * shader/cbuf/out set — e.g. dot4's a, b arrays). */
struct gpu_buffer {
    int dmabuf_fd;
    void *cpu_va;
    uint64_t gpu_va;
    uint64_t phys;
    uint32_t size_bytes;
};

/* ============================================================
 * Helper API
 * ============================================================ */

/* Die-on-error open / ioctl wrappers. */
int  gpu_xopen(const char *path, int flags);
void gpu_xioctl(int fd, unsigned long req, void *arg, const char *name);

/* nvmap: alloc a dmabuf of at least `size` bytes, aligned to
 * `align` (or 4 KB, whichever is larger). Dies on failure. */
int gpu_nvmap_alloc_dmabuf(int nvmap_fd, uint32_t size, uint32_t align);

/* Read a whole file into a freshly-malloc'd buffer. Caller owns the
 * buffer. NULL on error. */
void *gpu_load_file(const char *path, size_t *out_size);

/* Resolve a process-virtual address to its CPU-physical via
 * /proc/self/pagemap. Returns 0 on failure. */
uint64_t gpu_virt_to_phys(void *vaddr);

/* Full channel bringup: alloc AS, open TSG + channel, bind
 * subcontext, SETUP_BIND, ALLOC_OBJ_CTX(COMPUTE_B),
 * SET_PREEMPT_MODE, SET_ERROR_NOTIFIER, allocate + register + map
 * the six common buffers, upload the shader.
 *
 * On return, `ctx` is fully populated. shader_size is stored both
 * into *ctx->shader_size_bytes and — if non-NULL — into *out_size.
 * Does NOT populate the QMD or cbuf (those are kernel-specific;
 * see gpu_launch_populate_qmd + the per-kernel cbuf setup).
 *
 * Dies on any ioctl / mmap failure. */
void gpu_launch_setup(struct gpu_launch_ctx *ctx,
                      const char *shader_path,
                      size_t *out_shader_size);

/* Allocate + register + map + mmap a kernel-specific extra buffer.
 * size is rounded up to the nearest page, align is passed through
 * to nvmap_alloc_dmabuf. Dies on failure. */
struct gpu_buffer gpu_alloc_buffer(struct gpu_launch_ctx *ctx,
                                    uint32_t size,
                                    uint32_t align);

/* Populate the QMD with version + defaults suitable for a
 * single-thread single-CTA kernel. Specifically:
 *   - QMD_MAJOR_VERSION = 3, QMD_VERSION = 0
 *   - API_VISIBLE_CALL_LIMIT = NO_CHECK, SAMPLER_INDEX = INDEPENDENT
 *   - CTA raster + thread dims all 1
 *   - REGISTER_COUNT_V = GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT
 *   - BARRIER_COUNT = 0, SHARED_MEMORY_SIZE = 0, SLM size = 0
 *   - All INVALIDATE_* bits = 1, SM_GLOBAL_CACHING_ENABLE = 1
 *   - PROGRAM_ADDRESS = ctx->shader_gva
 *   - CBUF[0] at ctx->cbuf_gva, size = GPU_LAUNCH_CBUF_SIZE_B, VALID
 *
 * Caller fills cbuf[0] separately (kernel args at 0x160+).
 * Kernels needing > 1 CTA, > 1 thread, or different register count
 * should call gpu_qmd_set_bits() directly to override after this. */
void gpu_launch_populate_qmd(struct gpu_launch_ctx *ctx);

/* Build the 13-dword dispatch pushbuffer at `pb` (caller provides
 * storage ≥ 13 u32). qmd_gpu_va must be 256 B-aligned. Returns
 * the dword count written. Same shape as the kernel's
 * ga10b_build_launch_kernel_pushbuffer(). */
size_t gpu_build_launch_pushbuffer(uint32_t *pb, uint64_t qmd_gpu_va);

/* Populate CUDA's built-in-variable region of cbuf[0] (offsets
 * 0x00..0x14) with the dispatch's blockDim and gridDim. Without
 * this, kernels that read `blockDim.x` etc. via the SASS sequence
 *   IMAD R0, R0 (CTAID.X), c[0x0][0x0] (blockDim.x), R5 (TID.X)
 * compute their global thread index as if blockDim were 0, so
 * every CTA's threads collapse onto the same range and only
 * CTA(0,0) appears to have run.
 *
 * Hardcoded-constant kernels (matmul4x4, matmul8x8_grid) don't need
 * this because their CTA dims are folded into the SASS as literals;
 * any kernel parameterized over blockDim/gridDim must call this
 * after writing its own kernel args at cbuf[0][0x160+]. */
void gpu_write_builtin_dims(struct gpu_launch_ctx *ctx,
                             uint32_t block_x, uint32_t block_y, uint32_t block_z,
                             uint32_t grid_x,  uint32_t grid_y,  uint32_t grid_z);

/* Copy the built pushbuffer into ctx->pb_va, post a GPFIFO entry,
 * advance GP_PUT, ring the doorbell, and poll `*poll_va` for exact
 * equality with `expected_payload`. Returns 1 on match, 0 on timeout
 * (up to timeout_ms milliseconds, rounded to 10 ms granularity).
 *
 * Caller's responsibility: ensure `*poll_va != expected_payload` at
 * entry. Typical pattern is `memset(out, 0, ...) + msync` plus a
 * non-zero `expected_payload`. There is NO special-case for
 * `expected_payload == 0`: passing 0 against a freshly-zeroed buffer
 * returns 1 on the first iteration without verifying that the GPU
 * dispatched anything. */
int gpu_submit_and_poll(struct gpu_launch_ctx *ctx,
                         const uint32_t *pb_buf,
                         size_t pb_dwords,
                         volatile uint32_t *poll_va,
                         uint32_t expected_payload,
                         uint32_t timeout_ms);

/* Serialize a v4 channel handoff into handoff_va (4 KB page).
 *
 * The handoff conveys enough state for SLM-OS's `nvgpu channel` +
 * `nvgpu launch-kernel` to re-dispatch the same kernel after kexec.
 * `output_phys` and `output_gpu_va` describe the kernel's output
 * buffer (SLM-OS polls CPU-physical, GPU writes GPU-VA). The
 * `semaphore_*` fields alias the output buffer because Phase 7
 * host-family SEMAPHORE_RELEASE also targets whatever the caller
 * zeroed pre-submit.
 *
 * `expected_payload` is the v4 addition — SLM-OS polls for this
 * value rather than the hard-coded GA10B_SMOKETEST_SEM_PAYLOAD,
 * letting any kernel (write_cafe = 0xCAFE, dot4 = 300, future
 * GEMM = kernel-specific result) go through the same dispatch
 * path.
 *
 * Returns the CPU-physical address of the handoff block (what
 * SLM-OS's magic scan will find in DRAM). */
uint64_t gpu_write_handoff_v4(const struct gpu_launch_ctx *ctx,
                               void *handoff_va,
                               uint64_t output_phys,
                               uint64_t output_gpu_va,
                               uint32_t expected_payload);

/* Serialize a v5 channel handoff (pipeline of N ops). v5 extends v4
 * with `pipeline_n_ops` + `pipeline_ops_phys`. SLM-OS dispatches the
 * N ops in sequence, polling each op's `output_phys` for its
 * `expected_payload` before advancing to the next.
 *
 * Memory contract: `pipeline_ops` is an array of N
 * `struct ga10b_pipeline_op` (24 bytes each) sitting on its own
 * DRAM page. The launcher allocates that page via nvmap (so SLM-OS
 * can read it post-kexec) and passes the page's physical address.
 *
 * `output_phys` / `output_gpu_va` of the v4-compat fields point at
 * the LAST op's output (SLM-OS's pre-pipeline single-shot path is
 * never taken when `pipeline_n_ops > 0`, but populating them keeps
 * the validator's non-zero requirement happy). */
uint64_t gpu_write_handoff_v5(const struct gpu_launch_ctx *ctx,
                               void *handoff_va,
                               uint64_t output_phys,
                               uint64_t output_gpu_va,
                               uint32_t expected_payload,
                               uint32_t pipeline_n_ops,
                               uint64_t pipeline_ops_phys);

#endif /* GPU_LAUNCH_COMMON_H */
