/*
 * gpu-launch-common.c — Shared implementation for the Linux-userspace
 * compute-kernel launchers. See scripts/gpu-launch-common.h for the
 * design rationale and API. Both scripts/gpu-kernel-launch.c and
 * scripts/gpu-kernel-dot4.c link against this file.
 *
 * Compile (from a per-kernel launcher):
 *   gcc -O2 -Wall -o gpu-kernel-foo \
 *       gpu-kernel-foo.c gpu-launch-common.c
 */

/* `pread` (used in gpu_virt_to_phys) needs glibc's _GNU_SOURCE
 * feature-test to expose the prototype. Defined here, before any
 * include, so the macro applies to this TU only — keeping it out of
 * the header avoids the failure mode where a launcher includes
 * <stdio.h> ahead of "gpu-launch-common.h" and the macro never takes
 * effect. */
#define _GNU_SOURCE

#include "gpu-launch-common.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

/* ============================================================
 * Thin error-checked wrappers
 * ============================================================ */

int gpu_xopen(const char *path, int flags)
{
    int fd = open(path, flags);
    if (fd < 0) { perror(path); exit(1); }
    return fd;
}

void gpu_xioctl(int fd, unsigned long req, void *arg, const char *name)
{
    if (ioctl(fd, req, arg) < 0) {
        fprintf(stderr, "%s: %s (errno=%d)\n", name, strerror(errno), errno);
        exit(1);
    }
}

int gpu_nvmap_alloc_dmabuf(int nvmap_fd, uint32_t size, uint32_t align)
{
    struct nvmap_create_handle cr = { .size64 = size };
    gpu_xioctl(nvmap_fd, NVMAP_IOC_CREATE_64, &cr, "NVMAP_CREATE");
    uint32_t handle = cr.handle64;

    struct nvmap_alloc_handle al = {
        .handle = handle,
        .heap_mask = GPU_LAUNCH_NVMAP_IOVMM_HEAP_MASK,
        .flags = GPU_LAUNCH_NVMAP_CACHEABLE_FLAGS,
        .align = (align < 0x1000) ? 0x1000 : align,
        .numa_nid = -1,
    };
    gpu_xioctl(nvmap_fd, NVMAP_IOC_ALLOC, &al, "NVMAP_ALLOC");

    struct nvmap_create_handle gf = { .handle = handle };
    gpu_xioctl(nvmap_fd, NVMAP_IOC_GET_FD, &gf, "NVMAP_GET_FD");
    return gf.fd;
}

void *gpu_load_file(const char *path, size_t *out_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return NULL; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return NULL; }
    void *buf = malloc(st.st_size);
    if (!buf) { fprintf(stderr, "oom\n"); close(fd); return NULL; }
    if (read(fd, buf, st.st_size) != st.st_size) {
        perror("read"); free(buf); close(fd); return NULL;
    }
    close(fd);
    *out_size = st.st_size;
    return buf;
}

uint64_t gpu_virt_to_phys(void *vaddr)
{
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) { perror("pagemap"); return 0; }
    uint64_t vpage = (uint64_t)vaddr / 4096;
    uint64_t entry;
    if (pread(fd, &entry, 8, vpage * 8) != 8) {
        perror("pagemap read"); close(fd); return 0;
    }
    close(fd);
    if (!(entry & (1ULL << 63))) return 0;
    uint64_t pfn = entry & ((1ULL << 55) - 1);
    return pfn * 4096 + ((uint64_t)vaddr & 0xFFF);
}

/* gpu_qmd_set_bits is now `static inline` in scripts/gpu-qmd-bits.h
 * so the host-test harness can pick it up directly. See that header
 * for the implementation + UB-shift-guard rationale. */

/* ============================================================
 * Channel setup + buffer allocation
 *
 * Consolidates the ~150 lines of nvgpu ioctl sequencing both
 * launchers used to open (and still open) identically.
 * ============================================================ */

void gpu_launch_setup(struct gpu_launch_ctx *ctx,
                      const char *shader_path,
                      size_t *out_shader_size)
{
    size_t shader_size = 0;
    void *shader_bytes = gpu_load_file(shader_path, &shader_size);
    if (!shader_bytes) {
        fprintf(stderr, "shader blob missing at %s\n", shader_path);
        exit(2);
    }
    ctx->shader_size_bytes = shader_size;
    if (out_shader_size) *out_shader_size = shader_size;

    ctx->nvmap_fd = gpu_xopen("/dev/nvmap", O_RDWR);

    ctx->ctrl_fd = open("/dev/nvgpu/igpu0/ctrl", O_RDWR);
    if (ctx->ctrl_fd < 0)
        ctx->ctrl_fd = gpu_xopen("/dev/nvhost-ctrl-gpu", O_RDWR);

    struct nvgpu_alloc_as_args as_args;
    memset(&as_args, 0, sizeof(as_args));
    as_args.va_range_start = 0x4000000ULL;
    as_args.va_range_end   = 0x2000000000ULL;
    gpu_xioctl(ctx->ctrl_fd, NVGPU_GPU_IOCTL_ALLOC_AS, &as_args,
               "ALLOC_AS");
    ctx->as_fd = as_args.as_fd;

    struct nvgpu_gpu_open_tsg_args tsg_args;
    memset(&tsg_args, 0, sizeof(tsg_args));
    gpu_xioctl(ctx->ctrl_fd, NVGPU_GPU_IOCTL_OPEN_TSG, &tsg_args,
               "OPEN_TSG");
    ctx->tsg_fd = tsg_args.tsg_fd;

    struct nvgpu_gpu_open_channel_args ch_args;
    memset(&ch_args, 0, sizeof(ch_args));
    ch_args.in.runlist_id = -1;
    gpu_xioctl(ctx->ctrl_fd, NVGPU_GPU_IOCTL_OPEN_CHANNEL, &ch_args,
               "OPEN_CHANNEL");
    ctx->ch_fd = ch_args.out.channel_fd;

    struct nvgpu_as_bind_channel_args bind_as = { .channel_fd = ctx->ch_fd };
    gpu_xioctl(ctx->as_fd, NVGPU_AS_IOCTL_BIND_CHANNEL, &bind_as, "AS_BIND");

    struct nvgpu_tsg_create_subcontext_args subctx;
    memset(&subctx, 0, sizeof(subctx));
    subctx.type  = NVGPU_TSG_SUBCONTEXT_TYPE_ASYNC;
    subctx.as_fd = ctx->as_fd;
    gpu_xioctl(ctx->tsg_fd, NVGPU_TSG_IOCTL_CREATE_SUBCONTEXT, &subctx,
               "TSG_CREATE_SUBCONTEXT");

    struct nvgpu_tsg_bind_channel_ex_args bce;
    memset(&bce, 0, sizeof(bce));
    bce.channel_fd    = ctx->ch_fd;
    bce.subcontext_id = subctx.veid;
    gpu_xioctl(ctx->tsg_fd, NVGPU_TSG_IOCTL_BIND_CHANNEL_EX, &bce,
               "TSG_BIND_CHANNEL_EX");

    struct nvgpu_set_nvmap_fd_args nvm = { .fd = ctx->nvmap_fd };
    gpu_xioctl(ctx->ch_fd, NVGPU_IOCTL_CHANNEL_SET_NVMAP_FD, &nvm,
               "SET_NVMAP");

    struct nvgpu_channel_wdt_args wdt = {
        .wdt_status = NVGPU_IOCTL_CHANNEL_DISABLE_WDT |
                      NVGPU_IOCTL_CHANNEL_WDT_FLAG_SET_TIMEOUT,
        .timeout_ms = 0xFFFFFFFFu,
    };
    gpu_xioctl(ctx->ch_fd, NVGPU_IOCTL_CHANNEL_WDT, &wdt, "WDT_DISABLE");

    ctx->userd_dmabuf  = gpu_nvmap_alloc_dmabuf(ctx->nvmap_fd, 4096, 4096);
    /* GPFIFO sized to fit in ONE 4 KB page so the IOVMM allocation is
     * physically contiguous. Why: the helper's nvmap allocator uses
     * NVMAP_HEAP_IOVMM (0x40000000), which scatters multi-page
     * allocations across non-contiguous physical pages — fine for
     * device access via SMMU but breaks SLM-OS's post-kexec direct-
     * physical writes (Jetson EL2 identity DRAM mapping). With an
     * 8 KB GPFIFO, SLM-OS would write entries 512..1023 to
     * `gpfifo_phys + 4096..8191` which lands at *wherever the second
     * physical page happens to be*, NOT the second page of the IOVMM
     * allocation. PBDMA reads via the SMMU-mapped GPU VA and finds
     * stale entries → silent dispatch failure → wedge at GPFIFO index
     * 512. See #601 for the full investigation chain.
     *
     * One 4 KB page = 512 entries (8 bytes each). Plenty for our
     * single-channel workload — peak observed steady-state is ~10
     * outstanding entries before the next poll. */
    ctx->gpfifo_dmabuf = gpu_nvmap_alloc_dmabuf(ctx->nvmap_fd, 4096, 4096);

    struct nvgpu_channel_setup_bind_args sb;
    memset(&sb, 0, sizeof(sb));
    sb.num_gpfifo_entries = 512;
    sb.flags = NVGPU_CHANNEL_SETUP_BIND_FLAGS_DETERMINISTIC |
               NVGPU_CHANNEL_SETUP_BIND_FLAGS_USERMODE_SUPPORT;
    sb.userd_dmabuf_fd  = ctx->userd_dmabuf;
    sb.gpfifo_dmabuf_fd = ctx->gpfifo_dmabuf;
    gpu_xioctl(ctx->ch_fd, NVGPU_IOCTL_CHANNEL_SETUP_BIND, &sb,
               "SETUP_BIND");
    ctx->work_submit_token = sb.work_submit_token;
    ctx->gpfifo_gpu_va     = sb.gpfifo_gpu_va;
    ctx->gpfifo_entries    = 512;
    printf("[launch] work_submit_token=0x%x\n", sb.work_submit_token);

    struct nvgpu_alloc_obj_ctx_args octx = {
        .class_num = GPU_LAUNCH_CLASS_AMPERE_COMPUTE_B,
    };
    gpu_xioctl(ctx->ch_fd, NVGPU_IOCTL_CHANNEL_ALLOC_OBJ_CTX, &octx,
               "ALLOC_OBJ_CTX");

    struct nvgpu_preemption_mode_args pm = {
        .compute_preempt_mode = NVGPU_COMPUTE_PREEMPTION_MODE_CILP,
    };
    gpu_xioctl(ctx->ch_fd, NVGPU_IOCTL_CHANNEL_SET_PREEMPTION_MODE, &pm,
               "SET_PREEMPT_MODE");

    ctx->notifier_dmabuf = gpu_nvmap_alloc_dmabuf(ctx->nvmap_fd, 4096, 4096);
    struct nvgpu_set_error_notifier en = {
        .offset = 0, .size = 4096, .mem = ctx->notifier_dmabuf,
    };
    gpu_xioctl(ctx->ch_fd, NVGPU_IOCTL_CHANNEL_SET_ERROR_NOTIFIER, &en,
               "SET_ERROR_NOTIFIER");

    /* Common per-kernel buffers. */
    ctx->pb_dmabuf     = gpu_nvmap_alloc_dmabuf(ctx->nvmap_fd, 65536, 4096);
    /* 64 KB shader buffer accommodates the larger kernels in the
     * MNIST plan (MaxPool2D unrolls to ~17 KB SASS, Conv2D direct
     * is ~6 KB). The shader_size field in the handoff still tells
     * SLM-OS the actual code length; the unused tail is just
     * zero-padded. */
    ctx->shader_dmabuf = gpu_nvmap_alloc_dmabuf(ctx->nvmap_fd, 65536, 4096);
    ctx->cbuf_dmabuf   = gpu_nvmap_alloc_dmabuf(ctx->nvmap_fd, 4096, 4096);
    ctx->qmd_dmabuf    = gpu_nvmap_alloc_dmabuf(ctx->nvmap_fd, 4096, 4096);

    struct nvgpu_gpu_register_buffer_args regbuf;
    int reg_fds[] = { ctx->pb_dmabuf, ctx->shader_dmabuf,
                      ctx->cbuf_dmabuf, ctx->qmd_dmabuf };
    for (size_t i = 0; i < sizeof(reg_fds)/sizeof(reg_fds[0]); i++) {
        memset(&regbuf, 0, sizeof(regbuf));
        regbuf.dmabuf_fd = reg_fds[i];
        regbuf.comptags_alloc_control = NVGPU_GPU_COMPTAGS_ALLOC_NONE;
        ioctl(ctx->ctrl_fd, NVGPU_GPU_IOCTL_REGISTER_BUFFER, &regbuf);
    }

    struct nvgpu_as_map_buffer_ex_args map;
    memset(&map, 0, sizeof(map));
    map.compr_kind = -1;
    map.dmabuf_fd = ctx->pb_dmabuf;
    map.page_size = 4096;
    gpu_xioctl(ctx->as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &map, "MAP_PB");
    ctx->pb_gva = map.offset;

    memset(&map, 0, sizeof(map));
    map.compr_kind = -1;
    map.dmabuf_fd = ctx->shader_dmabuf;
    map.page_size = 4096;
    gpu_xioctl(ctx->as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &map, "MAP_SHADER");
    ctx->shader_gva = map.offset;

    memset(&map, 0, sizeof(map));
    map.compr_kind = -1;
    map.dmabuf_fd = ctx->cbuf_dmabuf;
    map.page_size = 4096;
    gpu_xioctl(ctx->as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &map, "MAP_CBUF");
    ctx->cbuf_gva = map.offset;

    memset(&map, 0, sizeof(map));
    map.compr_kind = -1;
    map.dmabuf_fd = ctx->qmd_dmabuf;
    map.page_size = 4096;
    gpu_xioctl(ctx->as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &map, "MAP_QMD");
    ctx->qmd_gva = map.offset;

    gpu_xioctl(ctx->tsg_fd, NVGPU_IOCTL_TSG_ENABLE, NULL, "TSG_ENABLE");

    ctx->userd_va  = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                          ctx->userd_dmabuf, 0);
    ctx->gpfifo_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                          ctx->gpfifo_dmabuf, 0);
    ctx->pb_va     = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED,
                          ctx->pb_dmabuf, 0);
    ctx->shader_va = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED,
                          ctx->shader_dmabuf, 0);
    ctx->cbuf_va   = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                          ctx->cbuf_dmabuf, 0);
    ctx->qmd_va    = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                          ctx->qmd_dmabuf, 0);
    if (ctx->userd_va == MAP_FAILED || ctx->gpfifo_va == MAP_FAILED ||
        ctx->pb_va == MAP_FAILED || ctx->shader_va == MAP_FAILED ||
        ctx->cbuf_va == MAP_FAILED || ctx->qmd_va == MAP_FAILED) {
        perror("mmap common buffers");
        exit(1);
    }

    /* Upload the shader. Buffer is 64 KB; the actual shader size
     * goes into the handoff so the GPU knows where the code ends. */
    if (shader_size > 65536) {
        fprintf(stderr,
                "shader too large: %zu bytes (max 65536)\n",
                shader_size);
        exit(1);
    }
    memset(ctx->shader_va, 0, 65536);
    memcpy(ctx->shader_va, shader_bytes, shader_size);
    msync(ctx->shader_va, 65536, MS_SYNC);
    free(shader_bytes);

    /* Doorbell mmap: the USERMODE doorbell is at offset 0x90 within
     * the 4 KB page that the ctrl fd maps. */
    ctx->doorbell_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                              MAP_SHARED, ctx->ctrl_fd, 0);
    if (ctx->doorbell_page == MAP_FAILED) {
        perror("mmap doorbell");
        exit(1);
    }

    printf("[launch] pb_gva=0x%lx shader_gva=0x%lx cbuf_gva=0x%lx "
           "qmd_gva=0x%lx\n",
           (unsigned long)ctx->pb_gva, (unsigned long)ctx->shader_gva,
           (unsigned long)ctx->cbuf_gva, (unsigned long)ctx->qmd_gva);
}

struct gpu_buffer gpu_alloc_buffer(struct gpu_launch_ctx *ctx,
                                    uint32_t size,
                                    uint32_t align)
{
    struct gpu_buffer buf = {0};

    if (size == 0) {
        fprintf(stderr, "gpu_alloc_buffer: refusing zero-size alloc\n");
        exit(1);
    }

    /* Round size up to page. */
    uint32_t page_size = 4096;
    uint32_t rounded = (size + page_size - 1) & ~(page_size - 1);
    buf.size_bytes = rounded;
    buf.dmabuf_fd = gpu_nvmap_alloc_dmabuf(ctx->nvmap_fd, rounded, align);

    struct nvgpu_gpu_register_buffer_args regbuf;
    memset(&regbuf, 0, sizeof(regbuf));
    regbuf.dmabuf_fd = buf.dmabuf_fd;
    regbuf.comptags_alloc_control = NVGPU_GPU_COMPTAGS_ALLOC_NONE;
    ioctl(ctx->ctrl_fd, NVGPU_GPU_IOCTL_REGISTER_BUFFER, &regbuf);

    struct nvgpu_as_map_buffer_ex_args map;
    memset(&map, 0, sizeof(map));
    map.compr_kind = -1;
    map.dmabuf_fd = buf.dmabuf_fd;
    map.page_size = 4096;
    gpu_xioctl(ctx->as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &map,
               "MAP_EXTRA_BUFFER");
    buf.gpu_va = map.offset;

    buf.cpu_va = mmap(NULL, rounded, PROT_READ | PROT_WRITE, MAP_SHARED,
                      buf.dmabuf_fd, 0);
    if (buf.cpu_va == MAP_FAILED) {
        perror("mmap extra buffer");
        exit(1);
    }
    /* Touch the first byte of every page to force pagemap entries to
     * materialize. gpu_virt_to_phys() reads the pagemap, which
     * returns zero for not-yet-present pages — an nvmap dmabuf that's
     * been mmap'd but never read/written has no PFN assigned yet.
     * The zero-touch doubles as safe default init; callers that want
     * different contents memset again. */
    for (uint32_t off = 0; off < rounded; off += page_size) {
        *((volatile uint8_t *)buf.cpu_va + off) = 0;
    }
    msync(buf.cpu_va, rounded, MS_SYNC);
    buf.phys = gpu_virt_to_phys(buf.cpu_va);
    return buf;
}

struct gpu_buffer gpu_load_shader_buffer(struct gpu_launch_ctx *ctx,
                                          const char *shader_path,
                                          size_t *out_shader_size)
{
    size_t shader_size = 0;
    void *shader_bytes = gpu_load_file(shader_path, &shader_size);
    if (!shader_bytes) {
        fprintf(stderr, "shader missing at %s\n", shader_path);
        exit(2);
    }
    if (shader_size > 65536) {
        fprintf(stderr, "shader %s too large: %zu bytes (max 65536)\n",
                shader_path, shader_size);
        exit(1);
    }
    struct gpu_buffer buf = gpu_alloc_buffer(ctx, 65536, 4096);
    memset(buf.cpu_va, 0, buf.size_bytes);
    memcpy(buf.cpu_va, shader_bytes, shader_size);
    msync(buf.cpu_va, buf.size_bytes, MS_SYNC);
    free(shader_bytes);
    if (out_shader_size) *out_shader_size = shader_size;
    return buf;
}

/* ============================================================
 * QMD population
 * ============================================================ */

void gpu_populate_qmd_at(uint32_t *qmd,
                          uint64_t shader_gpu_va,
                          uint64_t cbuf_gpu_va,
                          uint32_t register_count_v)
{
    memset(qmd, 0, 256);

    /* Version + enum defaults (matches NVK's qmd_init!). */
    gpu_qmd_set_bits(qmd, QMD_MAJOR_VERSION_HI, QMD_MAJOR_VERSION_LO, 3);
    gpu_qmd_set_bits(qmd, QMD_VERSION_HI, QMD_VERSION_LO, 0);
    gpu_qmd_set_bits(qmd, QMD_API_VISIBLE_CALL_LIMIT_BIT,
                     QMD_API_VISIBLE_CALL_LIMIT_BIT, 1);
    gpu_qmd_set_bits(qmd, QMD_SAMPLER_INDEX_BIT,
                     QMD_SAMPLER_INDEX_BIT, 0);

    /* Single CTA, single thread by default — caller overrides. */
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                     QMD_CTA_RASTER_WIDTH_LO, 1);
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                     QMD_CTA_RASTER_HEIGHT_LO, 1);
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_DEPTH_HI,
                     QMD_CTA_RASTER_DEPTH_LO, 1);
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI,
                     QMD_CTA_THREAD_DIM0_LO, 1);
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM1_HI,
                     QMD_CTA_THREAD_DIM1_LO, 1);
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM2_HI,
                     QMD_CTA_THREAD_DIM2_LO, 1);

    /* Shader program address (Ampere: absolute, no shift). */
    gpu_qmd_set_bits(qmd, QMD_PROGRAM_ADDRESS_LOWER_HI,
                     QMD_PROGRAM_ADDRESS_LOWER_LO,
                     shader_gpu_va & 0xFFFFFFFFu);
    gpu_qmd_set_bits(qmd, QMD_PROGRAM_ADDRESS_UPPER_HI,
                     QMD_PROGRAM_ADDRESS_UPPER_LO,
                     (shader_gpu_va >> 32) & 0x1FFFFu);

    /* Registers / shmem / SLM / barriers. */
    gpu_qmd_set_bits(qmd, QMD_REGISTER_COUNT_V_HI,
                     QMD_REGISTER_COUNT_V_LO,
                     register_count_v);
    gpu_qmd_set_bits(qmd, QMD_SHARED_MEMORY_SIZE_HI,
                     QMD_SHARED_MEMORY_SIZE_LO, 0);
    gpu_qmd_set_bits(qmd, QMD_SHADER_LOCAL_MEM_LOW_SIZE_HI,
                     QMD_SHADER_LOCAL_MEM_LOW_SIZE_LO, 0);
    gpu_qmd_set_bits(qmd, QMD_SHADER_LOCAL_MEM_HIGH_SIZE_HI,
                     QMD_SHADER_LOCAL_MEM_HIGH_SIZE_LO, 0);
    gpu_qmd_set_bits(qmd, QMD_BARRIER_COUNT_HI,
                     QMD_BARRIER_COUNT_LO, 0);

    /* Global caching + cache invalidate all. Safer on first dispatch
     * after channel setup. */
    gpu_qmd_set_bits(qmd, QMD_SM_GLOBAL_CACHING_ENABLE_BIT,
                     QMD_SM_GLOBAL_CACHING_ENABLE_BIT, 1);
    gpu_qmd_set_bits(qmd, QMD_INVALIDATE_TEXTURE_HEADER_CACHE_BIT,
                     QMD_INVALIDATE_TEXTURE_HEADER_CACHE_BIT, 1);
    gpu_qmd_set_bits(qmd, QMD_INVALIDATE_TEXTURE_SAMPLER_CACHE_BIT,
                     QMD_INVALIDATE_TEXTURE_SAMPLER_CACHE_BIT, 1);
    gpu_qmd_set_bits(qmd, QMD_INVALIDATE_TEXTURE_DATA_CACHE_BIT,
                     QMD_INVALIDATE_TEXTURE_DATA_CACHE_BIT, 1);
    gpu_qmd_set_bits(qmd, QMD_INVALIDATE_SHADER_DATA_CACHE_BIT,
                     QMD_INVALIDATE_SHADER_DATA_CACHE_BIT, 1);
    gpu_qmd_set_bits(qmd, QMD_INVALIDATE_INSTRUCTION_CACHE_BIT,
                     QMD_INVALIDATE_INSTRUCTION_CACHE_BIT, 1);
    gpu_qmd_set_bits(qmd, QMD_INVALIDATE_SHADER_CONSTANT_CACHE_BIT,
                     QMD_INVALIDATE_SHADER_CONSTANT_CACHE_BIT, 1);

    /* cbuf[0]: CUDA-compatible param area at cbuf_gpu_va. size/16 in
     * the field, VALID bit on. */
    const unsigned cbuf_idx = 0;
    const uint32_t cbuf_size_B = GPU_LAUNCH_CBUF_SIZE_B;
    gpu_qmd_set_bits(qmd,
                     QMD_CBUF_ADDR_LO_BASE + cbuf_idx * 64 + 31,
                     QMD_CBUF_ADDR_LO_BASE + cbuf_idx * 64,
                     cbuf_gpu_va & 0xFFFFFFFFu);
    gpu_qmd_set_bits(qmd,
                     QMD_CBUF_ADDR_HI_BASE + cbuf_idx * 64 + 16,
                     QMD_CBUF_ADDR_HI_BASE + cbuf_idx * 64,
                     (cbuf_gpu_va >> 32) & 0x1FFFFu);
    gpu_qmd_set_bits(qmd,
                     QMD_CBUF_SIZE_SHIFTED4_BASE + cbuf_idx * 64 + 12,
                     QMD_CBUF_SIZE_SHIFTED4_BASE + cbuf_idx * 64,
                     cbuf_size_B >> 4);
    gpu_qmd_set_bits(qmd,
                     QMD_CBUF_VALID_BASE + cbuf_idx,
                     QMD_CBUF_VALID_BASE + cbuf_idx, 1);
}

void gpu_launch_populate_qmd(struct gpu_launch_ctx *ctx)
{
    gpu_populate_qmd_at((uint32_t *)ctx->qmd_va,
                        ctx->shader_gva,
                        ctx->cbuf_gva,
                        GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT);
    msync(ctx->qmd_va, 4096, MS_SYNC);
}

/* ============================================================
 * Pushbuffer builder + submit/poll
 * ============================================================ */

void gpu_write_builtin_dims(struct gpu_launch_ctx *ctx,
                             uint32_t block_x, uint32_t block_y, uint32_t block_z,
                             uint32_t grid_x,  uint32_t grid_y,  uint32_t grid_z)
{
    /* CUDA reserves cbuf[0][0x00..0x14] for built-in dim variables.
     * SASS reads of blockDim.x come from c[0x0][0x0]; blockDim.y
     * from c[0x0][0x4]; etc. */
    uint32_t *p = (uint32_t *)ctx->cbuf_va;
    p[0] = block_x;   /* 0x00 blockDim.x */
    p[1] = block_y;   /* 0x04 blockDim.y */
    p[2] = block_z;   /* 0x08 blockDim.z */
    p[3] = grid_x;    /* 0x0C gridDim.x  */
    p[4] = grid_y;    /* 0x10 gridDim.y  */
    p[5] = grid_z;    /* 0x14 gridDim.z  */
    msync(ctx->cbuf_va, 4096, MS_SYNC);
}

size_t gpu_build_launch_pushbuffer(uint32_t *pb, uint64_t qmd_gpu_va)
{
    uint32_t *p = pb;

    /* SET_OBJECT(COMPUTE_B on subch 1). */
    *p++ = GPU_HDR_INC(1, GPU_LAUNCH_SUBCH_COMPUTE, NVC7C0_SET_OBJECT);
    *p++ = GPU_LAUNCH_CLASS_AMPERE_COMPUTE_B;

    /* Shader memory windows. */
    *p++ = GPU_HDR_INC(2, GPU_LAUNCH_SUBCH_COMPUTE,
                       NVC7C0_SET_SHADER_SHARED_MEMORY_WINDOW_A);
    *p++ = 0;                  /* upper 17 bits */
    *p++ = 0xfe000000;         /* lower 32 bits */

    *p++ = GPU_HDR_INC(2, GPU_LAUNCH_SUBCH_COMPUTE,
                       NVC7C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A);
    *p++ = 0;
    *p++ = 0xff000000;

    /* Cache invalidates (safe on first dispatch). */
    *p++ = GPU_HDR_IMMD(GPU_LAUNCH_SUBCH_COMPUTE,
                        NVC7C0_INVALIDATE_SKED_CACHES, 0);
    *p++ = GPU_HDR_IMMD(GPU_LAUNCH_SUBCH_COMPUTE,
                        NVC7C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0);

    /* INVALIDATE_SHADER_CACHES (bits 0,1,2,4,12 = 0x1017). Required
     * for cross-launch coherency on the same channel — Phase 6
     * hardware verification confirmed per-dispatch QMD rotation alone
     * leaves shader-side cache staleness across launches. Mirrors the
     * kernel's ga10b_build_launch_kernel_pushbuffer. */
    *p++ = GPU_HDR_IMMD(GPU_LAUNCH_SUBCH_COMPUTE,
                        NVC7C0_INVALIDATE_SHADER_CACHES,
                        NVC7C0_INVALIDATE_SHADER_CACHES_ALL);

    /* SEND_PCAS_A: QMD address shifted right 8 (QMD is 256 B aligned). */
    *p++ = GPU_HDR_INC(1, GPU_LAUNCH_SUBCH_COMPUTE, NVC7C0_SEND_PCAS_A);
    *p++ = (uint32_t)(qmd_gpu_va >> 8);

    /* Ampere dispatches via PCAS2_B with INVALIDATE_COPY_SCHEDULE.
     * See header-comment for the arch-split rationale. */
    *p++ = GPU_HDR_IMMD(GPU_LAUNCH_SUBCH_COMPUTE,
                        NVC7C0_SEND_SIGNALING_PCAS2_B,
                        NVC7C0_SEND_SIGNALING_PCAS2_B_ACTION_INVALIDATE_COPY_SCHEDULE);

    return (size_t)(p - pb);
}

int gpu_submit_and_poll(struct gpu_launch_ctx *ctx,
                         const uint32_t *pb_buf,
                         size_t pb_dwords,
                         volatile uint32_t *poll_va,
                         uint32_t expected_payload,
                         uint32_t timeout_ms)
{
    /* GPFIFO entry encodes the pushbuffer length at gp_e1[31:10] —
     * 22 bits, max 0x3FFFFF dwords. Past that the high bits get
     * silently truncated and PBDMA reads a too-short pushbuffer.
     * All current launchers stay well under this (largest is 14
     * dwords), but a fail-fast guard surfaces the failure mode at
     * its source if a future caller balloons the pb. */
    if (pb_dwords >= (1u << 22)) {
        fprintf(stderr,
                "gpu_submit_and_poll: pb_dwords=%zu exceeds GPFIFO "
                "entry size field (max %u)\n",
                pb_dwords, (1u << 22) - 1u);
        exit(1);
    }

    /* Copy pushbuffer into the mapped ring region.
     *
     * Single-page pb buffer is reused across submits — fine because
     * the polling loop below ensures the prior dispatch finished
     * before we overwrite. */
    uint32_t *pb32 = (uint32_t *)ctx->pb_va;
    memcpy(pb32, pb_buf, pb_dwords * sizeof(uint32_t));
    msync(ctx->pb_va, pb_dwords * 4, MS_SYNC);

    /* Multi-submit support: read current GP_PUT, post the new
     * GPFIFO entry to slot `cur & mask`, advance GP_PUT to
     * `cur + 1`. The first call sees GP_PUT=0 and writes slot 0;
     * each subsequent call advances to the next slot. */
    volatile uint32_t *userd = (volatile uint32_t *)ctx->userd_va;
    uint32_t cur_gp_put = userd[GPU_LAUNCH_USERD_GP_PUT_WORD];
    /* gpu_launch_setup pins gpfifo_entries to 512 (a power of two
     * fitting in one 4 KB page — see #601 for why a multi-page
     * GPFIFO breaks SLM-OS's post-kexec direct-physical writes);
     * the mask works as long as that contract holds. */
    uint32_t mask = ctx->gpfifo_entries - 1u;
    uint32_t slot = cur_gp_put & mask;

    uint64_t pb_gva = ctx->pb_gva;
    uint32_t gp_e0 = (uint32_t)(pb_gva & 0xFFFFFFFCu);
    uint32_t gp_e1 = (uint32_t)((pb_gva >> 32) & 0xFFu) |
                     ((uint32_t)pb_dwords << 10);
    ((uint32_t *)ctx->gpfifo_va)[slot * 2 + 0] = gp_e0;
    ((uint32_t *)ctx->gpfifo_va)[slot * 2 + 1] = gp_e1;
    msync(ctx->gpfifo_va, ctx->gpfifo_entries * 8, MS_SYNC);

    uint32_t new_gp_put = ga10b_next_gp_put(cur_gp_put, ctx->gpfifo_entries);
    userd[GPU_LAUNCH_USERD_GP_PUT_WORD] = new_gp_put;
    msync(ctx->userd_va, 4096, MS_SYNC);

    /* Doorbell at ctrl-fd mmap + 0x90. */
    volatile uint32_t *doorbell =
        (volatile uint32_t *)((char *)ctx->doorbell_page + 0x90);
    *doorbell = ctx->work_submit_token;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Poll at 10 ms granularity up to timeout_ms.
     *
     * Mode selector (ga10b_poll_match in ga10b_channel_handoff.h):
     *   expected_payload != 0 → exact-match polling.
     *   expected_payload == 0 → "wait for non-zero" — used by multi-
     *     op pipelines where the per-op output value isn't known
     *     ahead of time (FP32 ordering between CPU reference and
     *     GPU FFMA can differ in the low bits, but any non-zero
     *     value indicates the op completed). Caller pre-zeroes
     *     `*poll_va` so the transition is observable. */
    uint32_t iterations = (timeout_ms + 9) / 10;
    uint32_t val = 0;
    for (uint32_t i = 0; i < iterations; i++) {
        msync((void *)poll_va, 4, MS_INVALIDATE | MS_SYNC);
        __asm__ volatile("dsb sy" ::: "memory");
        val = *poll_va;
        if (ga10b_poll_match(val, expected_payload)) {
            /* Multi-CTA dispatch hazard: a single-cell sentinel poll
             * proves at least the polled cell was written, not that
             * all CTAs finished. With imbalanced CTAs (e.g. tile-
             * misaligned shapes where some CTAs have many fewer
             * active threads than others), a fast CTA can write the
             * sentinel before lagging CTAs finish their writes,
             * leaving spurious zeros in those cells.
             *
             * Workaround: 500 ms grace period after sentinel match
             * covers any reasonable kernel running on the small
             * MNIST-shape workloads we care about today.
             *
             * **This is a known throughput cost.** An 8-op pipeline
             * pays 4 seconds in pure sleeps. The proper fix is to
             * append a SEMAPHORE_RELEASE method to the dispatch
             * pushbuffer (NVC7C0 method 0x6098/9C/A0/A4 family on
             * Ampere) so the GPU writes a sentinel only after all
             * SMs have drained. The launcher would then poll that
             * semaphore — exact-match — without needing this sleep.
             * That refactor touches `gpu_build_launch_pushbuffer`
             * + the v5 ops array's poll target convention; track
             * as a follow-up before any benchmark / production
             * workload uses this path. See
             * docs/archive/plans/jetson-gpu-mnist-plan.md §"Risks and open
             * questions" for the design notes. */
            usleep(500000);
            msync((void *)poll_va, 4, MS_INVALIDATE | MS_SYNC);
            __asm__ volatile("dsb sy" ::: "memory");
            return 1;
        }
        usleep(10000);
    }
    return 0;
}

/* ============================================================
 * Handoff v4 writer
 * ============================================================ */

uint64_t gpu_write_handoff_v4(const struct gpu_launch_ctx *ctx,
                               void *handoff_va,
                               uint64_t output_phys,
                               uint64_t output_gpu_va,
                               uint32_t expected_payload)
{
    /* The memset both zero-inits the page and forces its pagemap
     * entry to materialize so the next gpu_virt_to_phys() returns a
     * non-zero PFN. This works only because the handoff fits in a
     * single 4 KB page; a future multi-page handoff would need the
     * per-page touch loop seen in gpu_alloc_buffer. The
     * sizeof-handoff static_assert at 200 B keeps that invariant
     * easy to spot. */
    memset(handoff_va, 0, 4096);
    uint64_t handoff_phys = gpu_virt_to_phys(handoff_va);

    /* Resolve physical addresses for the core channel buffers. */
    uint64_t userd_phys  = gpu_virt_to_phys(ctx->userd_va);
    uint64_t gpfifo_phys = gpu_virt_to_phys(ctx->gpfifo_va);
    uint64_t pb_phys     = gpu_virt_to_phys(ctx->pb_va);
    uint64_t shader_phys = gpu_virt_to_phys(ctx->shader_va);
    uint64_t cbuf_phys   = gpu_virt_to_phys(ctx->cbuf_va);
    uint64_t qmd_phys    = gpu_virt_to_phys(ctx->qmd_va);

    struct ga10b_channel_handoff hoff = {
        .magic              = GA10B_CHANNEL_HANDOFF_MAGIC,
        .version            = 4,
        .channel_id         = 0,
        .tsg_id             = 0,
        .userd_phys         = userd_phys,
        .userd_gp_put_offset = GPU_LAUNCH_USERD_GP_PUT_WORD * 4u,
        .userd_gp_get_offset = GPU_LAUNCH_USERD_GP_GET_WORD * 4u,
        .gpfifo_phys        = gpfifo_phys,
        .gpfifo_gpu_va      = ctx->gpfifo_gpu_va,
        .gpfifo_entries     = ctx->gpfifo_entries,
        .gpfifo_entry_size  = 8,
        .pushbuf_phys       = pb_phys,
        .pushbuf_gpu_va     = ctx->pb_gva,
        .pushbuf_size       = 65536,
        /* Alias semaphore_* onto the output buffer — Phase 7 sema
         * release is a no-op in the launch_kernel path but SLM-OS's
         * ga10b_validate_handoff requires a non-null semaphore
         * phys, and the kernel writes its payload to output_phys
         * anyway. */
        .semaphore_phys     = output_phys,
        .semaphore_gpu_va   = output_gpu_va,
        .inst_block_phys    = 0,
        .initial_gp_put     = ((volatile uint32_t *)ctx->userd_va)
                                [GPU_LAUNCH_USERD_GP_PUT_WORD],
        .initial_gp_get     = ((volatile uint32_t *)ctx->userd_va)
                                [GPU_LAUNCH_USERD_GP_GET_WORD],
        .work_submit_token  = ctx->work_submit_token,
        /* v3 extension: compute-kernel state. */
        .shader_phys        = shader_phys,
        .shader_gpu_va      = ctx->shader_gva,
        .cbuf_phys          = cbuf_phys,
        .cbuf_gpu_va        = ctx->cbuf_gva,
        .qmd_phys           = qmd_phys,
        .qmd_gpu_va         = ctx->qmd_gva,
        .output_phys        = output_phys,
        .output_gpu_va      = output_gpu_va,
        .shader_size        = (uint32_t)ctx->shader_size_bytes,
        .cbuf_size          = GPU_LAUNCH_CBUF_SIZE_B,
        /* v4 extension: expected payload for generic launch_kernel. */
        .expected_payload   = expected_payload,
    };
    memcpy(handoff_va, &hoff, sizeof(hoff));
    msync(handoff_va, 4096, MS_SYNC);

    return handoff_phys;
}

uint64_t gpu_write_handoff_v5(const struct gpu_launch_ctx *ctx,
                               void *handoff_va,
                               uint64_t output_phys,
                               uint64_t output_gpu_va,
                               uint32_t expected_payload,
                               uint32_t pipeline_n_ops,
                               uint64_t pipeline_ops_phys)
{
    /* Same single-page invariant as v4. */
    memset(handoff_va, 0, 4096);
    uint64_t handoff_phys = gpu_virt_to_phys(handoff_va);

    uint64_t userd_phys  = gpu_virt_to_phys(ctx->userd_va);
    uint64_t gpfifo_phys = gpu_virt_to_phys(ctx->gpfifo_va);
    uint64_t pb_phys     = gpu_virt_to_phys(ctx->pb_va);

    /* v3 dispatch fields (qmd_gpu_va, output_phys, etc.) are
     * intentionally left zero in a v5 handoff. The actual op state
     * lives in the per-op pipeline_ops array; carrying stale "first
     * op" addresses in the v3 fields would let a future bug that
     * lost pipeline_n_ops silently fall through to the v3 single-
     * shot path and dispatch op 0 alone. With the v3 fields zeroed,
     * the launch_kernel pre-pipeline check `qmd_gpu_va == 0` fails
     * loudly instead. The v2 channel fields (userd/gpfifo/pushbuf/
     * semaphore/work_submit_token) stay populated because
     * ga10b_validate_handoff requires them non-zero. */
    struct ga10b_channel_handoff hoff = {
        .magic              = GA10B_CHANNEL_HANDOFF_MAGIC,
        .version            = 5,
        .channel_id         = 0,
        .tsg_id             = 0,
        .userd_phys         = userd_phys,
        .userd_gp_put_offset = GPU_LAUNCH_USERD_GP_PUT_WORD * 4u,
        .userd_gp_get_offset = GPU_LAUNCH_USERD_GP_GET_WORD * 4u,
        .gpfifo_phys        = gpfifo_phys,
        .gpfifo_gpu_va      = ctx->gpfifo_gpu_va,
        .gpfifo_entries     = ctx->gpfifo_entries,
        .gpfifo_entry_size  = 8,
        .pushbuf_phys       = pb_phys,
        .pushbuf_gpu_va     = ctx->pb_gva,
        .pushbuf_size       = 65536,
        /* semaphore_phys must be non-zero for the validator;
         * caller provides a meaningful (final-op) target. */
        .semaphore_phys     = output_phys,
        .semaphore_gpu_va   = output_gpu_va,
        .inst_block_phys    = 0,
        .initial_gp_put     = ((volatile uint32_t *)ctx->userd_va)
                                [GPU_LAUNCH_USERD_GP_PUT_WORD],
        .initial_gp_get     = ((volatile uint32_t *)ctx->userd_va)
                                [GPU_LAUNCH_USERD_GP_GET_WORD],
        .work_submit_token  = ctx->work_submit_token,
        /* v3 dispatch fields zeroed (see comment above). */
        .shader_phys        = 0,
        .shader_gpu_va      = 0,
        .cbuf_phys          = 0,
        .cbuf_gpu_va        = 0,
        .qmd_phys           = 0,
        .qmd_gpu_va         = 0,
        .output_phys        = 0,
        .output_gpu_va      = 0,
        .shader_size        = 0,
        .cbuf_size          = 0,
        .expected_payload   = expected_payload,
        /* v5 extension. */
        .pipeline_n_ops     = pipeline_n_ops,
        .pipeline_ops_phys  = pipeline_ops_phys,
    };
    memcpy(handoff_va, &hoff, sizeof(hoff));
    msync(handoff_va, 4096, MS_SYNC);

    return handoff_phys;
}

uint64_t gpu_write_handoff_v6(const struct gpu_launch_ctx *ctx,
                               void *handoff_va,
                               uint64_t output_phys,
                               uint64_t output_gpu_va,
                               uint32_t expected_payload,
                               uint32_t pipeline_n_ops,
                               uint64_t pipeline_ops_phys,
                               uint64_t input_buf_phys,
                               uint32_t input_buf_size,
                               uint32_t pipeline_kind)
{
    /* Same single-page invariant as v4/v5. Force-fault the page in
     * via memset before reading the phys, then write the struct. */
    memset(handoff_va, 0, 4096);
    uint64_t handoff_phys = gpu_virt_to_phys(handoff_va);

    uint64_t userd_phys  = gpu_virt_to_phys(ctx->userd_va);
    uint64_t gpfifo_phys = gpu_virt_to_phys(ctx->gpfifo_va);
    uint64_t pb_phys     = gpu_virt_to_phys(ctx->pb_va);

    /* v6 = v5 + input_buf_phys/size. v3 dispatch fields stay zero
     * (same defensive zeroing as v5 — forces the v5 pipeline path
     * if pipeline_n_ops survives but other fields don't). */
    struct ga10b_channel_handoff hoff = {
        .magic              = GA10B_CHANNEL_HANDOFF_MAGIC,
        .version            = 6,
        .channel_id         = 0,
        .tsg_id             = 0,
        .userd_phys         = userd_phys,
        .userd_gp_put_offset = GPU_LAUNCH_USERD_GP_PUT_WORD * 4u,
        .userd_gp_get_offset = GPU_LAUNCH_USERD_GP_GET_WORD * 4u,
        .gpfifo_phys        = gpfifo_phys,
        .gpfifo_gpu_va      = ctx->gpfifo_gpu_va,
        .gpfifo_entries     = ctx->gpfifo_entries,
        .gpfifo_entry_size  = 8,
        .pushbuf_phys       = pb_phys,
        .pushbuf_gpu_va     = ctx->pb_gva,
        .pushbuf_size       = 65536,
        .semaphore_phys     = output_phys,
        .semaphore_gpu_va   = output_gpu_va,
        .inst_block_phys    = 0,
        .initial_gp_put     = ((volatile uint32_t *)ctx->userd_va)
                                [GPU_LAUNCH_USERD_GP_PUT_WORD],
        .initial_gp_get     = ((volatile uint32_t *)ctx->userd_va)
                                [GPU_LAUNCH_USERD_GP_GET_WORD],
        .work_submit_token  = ctx->work_submit_token,
        /* v3 dispatch fields zeroed. */
        .shader_phys        = 0,
        .shader_gpu_va      = 0,
        .cbuf_phys          = 0,
        .cbuf_gpu_va        = 0,
        .qmd_phys           = 0,
        .qmd_gpu_va         = 0,
        .output_phys        = 0,
        .output_gpu_va      = 0,
        .shader_size        = 0,
        .cbuf_size          = 0,
        .expected_payload   = expected_payload,
        /* v5 extension. */
        .pipeline_n_ops     = pipeline_n_ops,
        .pipeline_ops_phys  = pipeline_ops_phys,
        /* v6 extension. */
        .input_buf_phys     = input_buf_phys,
        .input_buf_size     = input_buf_size,
        /* PR-3 extension: pipeline-kind discriminator (was _pad4). */
        .pipeline_kind      = pipeline_kind,
    };
    memcpy(handoff_va, &hoff, sizeof(hoff));
    msync(handoff_va, 4096, MS_SYNC);

    return handoff_phys;
}

void gpu_alloc_qmd_pool(struct gpu_launch_ctx *ctx, uint32_t n_slots)
{
    if (n_slots == 0u) {
        fprintf(stderr,
                "gpu_alloc_qmd_pool: refusing zero-slot pool\n");
        exit(1);
    }

    /* Release any prior pool's CPU mmap and dmabuf fd if a second
     * call occurs. The GMMU mapping is left implicit — closing the
     * dmabuf relies on nvgpu's refcount to drop the GPU VA. The
     * docstring in gpu-launch-common.h spells out the single-use
     * contract; this cleanup is adequate for current call sites
     * and a paranoia hedge against accidental re-invocation.
     *
     * Sentinel for "not allocated" is qmd_pool_dmabuf == 0. The
     * `struct gpu_launch_ctx` is zero-initialised by callers (see
     * the `struct gpu_launch_ctx ctx = {0};` pattern in
     * gpu-kernel-mnist.c and friends), and gpu_nvmap_alloc_dmabuf
     * never returns fd 0 in practice (fd 0 == stdin, always open
     * in any process). Using `!= 0` rather than `> 0` so a fd of
     * exactly 0 — which would be invalid as a sentinel anyway —
     * still triggers a close attempt rather than leaking it. */
    if (ctx->qmd_pool_va != NULL && ctx->qmd_pool_size_bytes > 0u) {
        munmap(ctx->qmd_pool_va, ctx->qmd_pool_size_bytes);
    }
    if (ctx->qmd_pool_dmabuf != 0) {
        close(ctx->qmd_pool_dmabuf);
    }

    /* Round to page so pagemap returns a sensible PFN. */
    const uint32_t page_size = 4096u;
    uint64_t bytes_u64 = (uint64_t)n_slots * 256u;
    if (bytes_u64 > 0xFFFFFFFFull) {
        fprintf(stderr,
                "gpu_alloc_qmd_pool: pool size %llu B overflows uint32_t\n",
                (unsigned long long)bytes_u64);
        exit(1);
    }
    uint32_t rounded = ((uint32_t)bytes_u64 + page_size - 1u)
                       & ~(page_size - 1u);

    /* gpu_alloc_buffer handles dmabuf alloc + register + map + mmap
     * + pagemap touch. Same path the existing per-buffer allocations
     * use — staying on the established pattern keeps the SMMU /
     * IOVMM behaviour identical to the helper-baked QMDs. */
    struct gpu_buffer pool = gpu_alloc_buffer(ctx, rounded, page_size);

    ctx->qmd_pool_dmabuf     = pool.dmabuf_fd;
    ctx->qmd_pool_va         = pool.cpu_va;
    ctx->qmd_pool_gva        = pool.gpu_va;
    ctx->qmd_pool_size_bytes = rounded;
    ctx->qmd_pool_n_slots    = rounded / 256u;

    /* Pre-zero the pool so any slot the SLM-OS dispatch path picks
     * up from a fresh handoff has known content. SLM-OS overwrites
     * the slot bytes before each launch via ga10b_qmd_populate, but
     * a defined initial state simplifies any post-hoc inspection
     * (e.g. `peek` on the slot before the first dispatch). */
    memset(ctx->qmd_pool_va, 0, ctx->qmd_pool_size_bytes);
    msync(ctx->qmd_pool_va, ctx->qmd_pool_size_bytes, MS_SYNC);
}

uint64_t gpu_write_handoff_v7(const struct gpu_launch_ctx *ctx,
                               void *handoff_va,
                               uint64_t output_phys,
                               uint64_t output_gpu_va,
                               uint32_t expected_payload,
                               uint32_t pipeline_n_ops,
                               uint64_t pipeline_ops_phys,
                               uint64_t input_buf_phys,
                               uint32_t input_buf_size,
                               uint32_t pipeline_kind)
{
    /* v7 requires a populated pool; refuse to produce a malformed
     * handoff that SLM-OS would silently fall back from. */
    if (ctx->qmd_pool_n_slots == 0u || ctx->qmd_pool_va == NULL) {
        fprintf(stderr,
                "gpu_write_handoff_v7: no QMD pool — call "
                "gpu_alloc_qmd_pool first\n");
        exit(1);
    }

    uint64_t qmd_pool_phys = gpu_virt_to_phys(ctx->qmd_pool_va);
    if (qmd_pool_phys == 0u) {
        fprintf(stderr,
                "gpu_write_handoff_v7: pagemap returned 0 for QMD "
                "pool — pool not faulted in?\n");
        exit(1);
    }

    /* Same single-page invariant as v3..v6: handoff struct is < 4 KB
     * and the wire format pins sizeof at exactly 256 B. */
    memset(handoff_va, 0, 4096);
    uint64_t handoff_phys = gpu_virt_to_phys(handoff_va);

    uint64_t userd_phys  = gpu_virt_to_phys(ctx->userd_va);
    uint64_t gpfifo_phys = gpu_virt_to_phys(ctx->gpfifo_va);
    uint64_t pb_phys     = gpu_virt_to_phys(ctx->pb_va);

    /* v7 = v6 + qmd_pool_* fields. v3 dispatch fields stay zeroed
     * for the same reason as v5/v6: forces v7 consumers to use the
     * v7 dispatch path even if pipeline_n_ops survives but other
     * fields don't. */
    struct ga10b_channel_handoff hoff = {
        .magic              = GA10B_CHANNEL_HANDOFF_MAGIC,
        .version            = 7,
        .channel_id         = 0,
        .tsg_id             = 0,
        .userd_phys         = userd_phys,
        .userd_gp_put_offset = GPU_LAUNCH_USERD_GP_PUT_WORD * 4u,
        .userd_gp_get_offset = GPU_LAUNCH_USERD_GP_GET_WORD * 4u,
        .gpfifo_phys        = gpfifo_phys,
        .gpfifo_gpu_va      = ctx->gpfifo_gpu_va,
        .gpfifo_entries     = ctx->gpfifo_entries,
        .gpfifo_entry_size  = 8,
        .pushbuf_phys       = pb_phys,
        .pushbuf_gpu_va     = ctx->pb_gva,
        .pushbuf_size       = 65536,
        .semaphore_phys     = output_phys,
        .semaphore_gpu_va   = output_gpu_va,
        .inst_block_phys    = 0,
        .initial_gp_put     = ((volatile uint32_t *)ctx->userd_va)
                                [GPU_LAUNCH_USERD_GP_PUT_WORD],
        .initial_gp_get     = ((volatile uint32_t *)ctx->userd_va)
                                [GPU_LAUNCH_USERD_GP_GET_WORD],
        .work_submit_token  = ctx->work_submit_token,
        /* v3 dispatch fields zeroed — see v5/v6 comment. */
        .shader_phys        = 0,
        .shader_gpu_va      = 0,
        .cbuf_phys          = 0,
        .cbuf_gpu_va        = 0,
        .qmd_phys           = 0,
        .qmd_gpu_va         = 0,
        .output_phys        = 0,
        .output_gpu_va      = 0,
        .shader_size        = 0,
        .cbuf_size          = 0,
        .expected_payload   = expected_payload,
        /* v5 extension. */
        .pipeline_n_ops     = pipeline_n_ops,
        .pipeline_ops_phys  = pipeline_ops_phys,
        /* v6 extension. */
        .input_buf_phys     = input_buf_phys,
        .input_buf_size     = input_buf_size,
        /* PR-3 extension: pipeline-kind discriminator. */
        .pipeline_kind      = pipeline_kind,
        /* v7 extension: per-dispatch QMD pool descriptor. */
        .qmd_pool_phys      = qmd_pool_phys,
        .qmd_pool_gpu_va    = ctx->qmd_pool_gva,
        .qmd_pool_size_bytes = ctx->qmd_pool_size_bytes,
        .qmd_pool_n_slots   = ctx->qmd_pool_n_slots,
    };
    memcpy(handoff_va, &hoff, sizeof(hoff));
    msync(handoff_va, 4096, MS_SYNC);

    return handoff_phys;
}
