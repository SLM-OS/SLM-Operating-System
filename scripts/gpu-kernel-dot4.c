/*
 * gpu-kernel-dot4.c — Launch a 4-element integer dot product on
 * Jetson GA10B via raw nvgpu. First "real arithmetic" kernel in the
 * tree (4 multiplies + 3 adds + 8 global loads, single-thread /
 * single-CTA).
 *
 * Purpose: exercises genuine GPU compute beyond the write_cafe
 * scalar store, with a known-correct expected output (300 from
 * a = {1,2,3,4}, b = {10,20,30,40}) that can be verified Linux-side
 * AND, with handoff v4, re-dispatched from SLM-OS post-kexec — the
 * launcher writes `expected_payload = 300` into the handoff so
 * SLM-OS's `nvgpu launch-kernel` polls for 300 instead of the
 * hard-coded 0xCAFE.
 *
 * Channel setup / QMD / dispatch scaffolding lives in
 * scripts/gpu-launch-common.{h,c}; this file is the dot4-specific
 * orchestration (three-pointer cbuf, pre-fill inputs, poll output
 * for 300, optionally preserve channel for kexec).
 *
 * CUDA source: scripts/cuda/dot4.cu. SASS extraction recipe:
 *   nvcc -arch=sm_87 -o dot4 dot4.cu
 *   cuobjdump --extract-elf all dot4
 *   dd if=dot4.2.sm_87.cubin of=dot4_shader.sass \
 *      bs=1 skip=$((0x600)) count=$((0x380))
 *
 * SASS cbuf layout (from manual cuobjdump inspection):
 *   cbuf[0][0x160..0x167] = a   (64-bit GPU VA)
 *   cbuf[0][0x168..0x16F] = b   (64-bit GPU VA)
 *   cbuf[0][0x170..0x177] = out (64-bit GPU VA)
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-dot4 \
 *       gpu-kernel-dot4.c gpu-launch-common.c
 *
 * Usage: sudo ./gpu-kernel-dot4 [--preserve-for-kexec]
 *                               [--timeout-secs N]
 *                               [path/to/shader.sass]
 */
#include "gpu-launch-common.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>

/* Expected dot-product result. a[i]*b[i] summed over i=0..3 for the
 * pinned test inputs below. SLM-OS polls `output_phys` for exactly
 * this value via the v4 handoff's expected_payload field. */
#define DOT4_EXPECTED 300u

/* Test inputs baked into the launcher so Linux-side validation is
 * self-contained. A fluke "GPU returned a cached value" would have
 * to return exactly 300, which is uniquely `1*10 + 2*20 + 3*30 +
 * 4*40` — no coincidence. */
static const int32_t DOT4_A[4] = { 1, 2, 3, 4 };
static const int32_t DOT4_B[4] = { 10, 20, 30, 40 };

static volatile sig_atomic_t g_shutdown;
static void on_term(int sig) { (void)sig; g_shutdown = 1; }

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    const char *shader_path = "./dot4_shader.sass";
    bool preserve = false;
    int timeout_secs = 900;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--preserve-for-kexec") == 0) {
            preserve = true;
        } else if (strcmp(argv[i], "--timeout-secs") == 0 && i + 1 < argc) {
            timeout_secs = atoi(argv[++i]);
            if (timeout_secs <= 0) timeout_secs = 900;
        } else if (argv[i][0] != '-') {
            shader_path = argv[i];
        }
    }

    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Channel + common buffers (userd, gpfifo, pb, qmd, shader,
     * cbuf), shader uploaded. */
    struct gpu_launch_ctx ctx = {0};
    size_t shader_size = 0;
    gpu_launch_setup(&ctx, shader_path, &shader_size);
    printf("[launch] shader %zu bytes\n", shader_size);

    /* Kernel-specific extra buffers: A, B (inputs), OUT (result). */
    struct gpu_buffer a   = gpu_alloc_buffer(&ctx, 4096, 4096);
    struct gpu_buffer b   = gpu_alloc_buffer(&ctx, 4096, 4096);
    struct gpu_buffer out = gpu_alloc_buffer(&ctx, 4096, 4096);

    /* Pre-fill inputs. Both a[] and b[] sit at buffer offset 0;
     * the GPU dereferences a[i] / b[i] as `(int *)a.gpu_va + i`
     * which maps to CPU offset i*4 in a.cpu_va / b.cpu_va. */
    memset(a.cpu_va, 0, a.size_bytes);
    memset(b.cpu_va, 0, b.size_bytes);
    memcpy(a.cpu_va, DOT4_A, sizeof(DOT4_A));
    memcpy(b.cpu_va, DOT4_B, sizeof(DOT4_B));
    msync(a.cpu_va, a.size_bytes, MS_SYNC);
    msync(b.cpu_va, b.size_bytes, MS_SYNC);

    memset(out.cpu_va, 0, out.size_bytes);
    msync(out.cpu_va, out.size_bytes, MS_SYNC);

    /* Populate cbuf[0]. dot4 takes three 64-bit pointer args at the
     * offsets verified in the SASS dump. */
    memset(ctx.cbuf_va, 0, 4096);
    *(uint64_t *)((char *)ctx.cbuf_va + 0x160) = a.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x168) = b.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x170) = out.gpu_va;
    msync(ctx.cbuf_va, 4096, MS_SYNC);

    /* Populate QMD with defaults. dot4 fits them (single CTA, single
     * thread, 128-register budget, no shmem/SLM, cbuf[0] default size). */
    gpu_launch_populate_qmd(&ctx);

    /* Build + dispatch the 13-dword compute pushbuffer. */
    uint32_t pb_buf[32];
    size_t pb_dwords = gpu_build_launch_pushbuffer(pb_buf, ctx.qmd_gva);
    printf("[launch] pushbuffer %zu dwords\n", pb_dwords);

    int ok = gpu_submit_and_poll(&ctx, pb_buf, pb_dwords,
                                  (volatile uint32_t *)out.cpu_va,
                                  DOT4_EXPECTED,
                                  2000);

    uint32_t val = *(volatile uint32_t *)out.cpu_va;
    uint32_t gp_get = ((volatile uint32_t *)ctx.userd_va)
                        [GPU_LAUNCH_USERD_GP_GET_WORD];
    printf("[launch] result: out=%u (want %u) GP_GET=%u (want 1)\n",
           val, DOT4_EXPECTED, gp_get);
    if (ok) {
        printf("[launch] SUCCESS: kernel computed %u = DOT4_EXPECTED\n",
               val);
    } else if (val != 0) {
        printf("[launch] FAIL: kernel ran but produced wrong value "
               "%u (want %u)\n", val, DOT4_EXPECTED);
    } else if (gp_get == 1) {
        printf("[launch] PARTIAL: pushbuffer consumed, kernel did not run\n");
    } else {
        printf("[launch] FAIL: PBDMA did not advance\n");
    }

    if (!preserve) {
        return ok ? 0 : 1;
    }

    if (!ok) {
        fprintf(stderr, "[launch] kernel failed pre-kexec; refusing to "
                "preserve (channel state is suspect)\n");
        return 1;
    }

    /* --- --preserve-for-kexec path ---
     * Write a v4 handoff so SLM-OS's `nvgpu launch-kernel` can
     * re-dispatch dot4 post-kexec. The critical v4 field is
     * `expected_payload = 300` — without it SLM-OS polls for
     * 0xCAFE and times out even though the kernel fires correctly. */
    int handoff_dmabuf = gpu_nvmap_alloc_dmabuf(ctx.nvmap_fd, 4096, 4096);
    void *handoff_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, handoff_dmabuf, 0);
    if (handoff_va == MAP_FAILED) { perror("mmap handoff"); return 1; }

    uint64_t handoff_phys = gpu_write_handoff_v4(&ctx, handoff_va,
                                                  out.phys, out.gpu_va,
                                                  DOT4_EXPECTED);

    printf("[launch] Handoff at phys 0x%llx (version=4)\n",
           (unsigned long long)handoff_phys);
    printf("[launch]   shader_phys=0x%llx  shader_gva=0x%lx (%zu B)\n",
           (unsigned long long)gpu_virt_to_phys(ctx.shader_va),
           (unsigned long)ctx.shader_gva, shader_size);
    printf("[launch]   cbuf_phys=0x%llx  cbuf_gva=0x%lx\n",
           (unsigned long long)gpu_virt_to_phys(ctx.cbuf_va),
           (unsigned long)ctx.cbuf_gva);
    printf("[launch]   qmd_phys=0x%llx  qmd_gva=0x%lx\n",
           (unsigned long long)gpu_virt_to_phys(ctx.qmd_va),
           (unsigned long)ctx.qmd_gva);
    printf("[launch]   a_phys=0x%llx  a_gva=0x%lx\n",
           (unsigned long long)a.phys, (unsigned long)a.gpu_va);
    printf("[launch]   b_phys=0x%llx  b_gva=0x%lx\n",
           (unsigned long long)b.phys, (unsigned long)b.gpu_va);
    printf("[launch]   output_phys=0x%llx  output_gva=0x%lx  "
           "expected_payload=%u\n",
           (unsigned long long)out.phys,
           (unsigned long)out.gpu_va, DOT4_EXPECTED);
    printf("[launch] Sleeping up to %d s — kexec now:\n", timeout_secs);
    printf("[launch]   sudo slmos-kexec --no-gpu-suspend /tmp/slmos.elf\n");

    for (int remaining = timeout_secs; remaining > 0 && !g_shutdown; ) {
        int slice = remaining > 60 ? 60 : remaining;
        sleep(slice);
        remaining -= slice;
    }
    printf("[launch] Exiting (%s) — channel releasing.\n",
           g_shutdown ? "signal" : "timeout");

    return 0;
}
