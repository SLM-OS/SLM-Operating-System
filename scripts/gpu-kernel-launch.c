/*
 * gpu-kernel-launch.c — Launch a trivial compute kernel via the raw
 * nvgpu interface on Jetson GA10B. No libcuda.
 *
 * STATUS: SCAFFOLD. Channel setup is proven (shared pattern with
 * scripts/gpu-channel-helper.c). QMD population follows Mesa NAK's
 * fill_qmd sequence verbatim (docs/reference/mesa-nak_qmd.rs). The
 * pushbuffer dispatch uses NVC7C0 SEND_PCAS_A + SEND_SIGNALING_PCAS_B,
 * per mesa-nvk_cmd_dispatch.c. Expected to need hardware iteration:
 *
 *   1. Shader binary is a CUDA-compiled cubin .text section (640 B
 *      for the trivial "write 0xCAFE to *out" kernel). The kernel
 *      reads `out` from cbuf[0] at CUDA's fixed param offset 0x160.
 *      If NAK-compiled shaders use a different cbuf layout, the
 *      param placement here is wrong.
 *   2. REGISTER_COUNT_V needs to be ≥ the count baked into the
 *      cubin's .nv.info (126 for this specific CUDA build).
 *   3. SASS_VERSION may need to be non-zero per sm_87.
 *   4. The first launch on an untouched channel may still need more
 *      of NVK's dispatch state init than the minimal set #291
 *      empirically requires — SET_OBJECT(COMPUTE_B) works for sema
 *      release but compute kernels might trip on a missing
 *      SET_SHADER_LOCAL_MEMORY_WINDOW or SET_SHADER_SHARED_MEMORY_WINDOW.
 *
 * Compile: gcc -O2 -Wall -o gpu-kernel-launch gpu-kernel-launch.c
 * Shader blob must be co-located at ./write_cafe_shader.sass (640 B
 * extracted from a CUDA-compiled cuda_write cubin, checked in at
 * docs/reference/cuda_write_shader.sass). On Jetson:
 *   scp docs/reference/cuda_write_shader.sass nano:~/
 *   # then rename to write_cafe_shader.sass next to the binary
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>
#include <errno.h>
#include <linux/types.h>

#include "/usr/src/nvidia/nvgpu/include/uapi/linux/nvgpu.h"
#include "/usr/src/nvidia/nvgpu/include/uapi/linux/nvgpu-as.h"
#include "/usr/src/nvidia/nvgpu/include/uapi/linux/nvgpu-ctrl.h"
#include "/usr/src/nvidia/nvidia-oot/include/uapi/linux/nvmap.h"

#include "../kernel/gpu/nvidia/ga10b_channel_handoff.h"
#include <signal.h>

#define SEM_PAYLOAD 0x0000CAFEu

#define CLASS_AMPERE_COMPUTE_B 0xC7C0
#define SUBCH_COMPUTE 1

/* Ampere USERD layout: GP_PUT at dword 35, GP_GET at dword 34 (from
 * hw_ram_ga10b.h). Declared once at file scope so the offsets baked
 * into the v3 handoff and the direct dword-array reads below use the
 * same constants — drift would make the handoff's stored offsets
 * disagree with what this helper actually read. */
#define USERD_GP_PUT_WORD 35u
#define USERD_GP_GET_WORD 34u

/* NVC7C0 methods we use (clc7c0.h offsets). */
#define NVC7C0_SET_OBJECT                             0x0000
#define NVC7C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI 0x0244
#define NVC7C0_INVALIDATE_SKED_CACHES                 0x0298
#define NVC7C0_SET_SHADER_SHARED_MEMORY_WINDOW_A      0x02a0
#define NVC7C0_SET_SHADER_SHARED_MEMORY_WINDOW_B      0x02a4
#define NVC7C0_SEND_PCAS_A                            0x02b4
#define NVC7C0_SEND_SIGNALING_PCAS_B                  0x02bc
#define NVC7C0_SEND_SIGNALING_PCAS_B_INVALIDATE_TRUE  0x1
#define NVC7C0_SEND_SIGNALING_PCAS_B_SCHEDULE_TRUE    0x2
/* Ampere (cls_compute > TURING_COMPUTE_A) uses PCAS2_B with a
 * composite action rather than PCAS_B's two-bit invalidate/schedule.
 * See mesa-nvk_cmd_dispatch.c:322-340 — Ampere branch emits
 * SEND_SIGNALING_PCAS2_B with action=INVALIDATE_COPY_SCHEDULE (0xA).
 * Getting this wrong silently no-ops the dispatch on GA10B even
 * though the pushbuffer is consumed. */
#define NVC7C0_SEND_SIGNALING_PCAS2_B                 0x02c0
#define NVC7C0_SEND_SIGNALING_PCAS2_B_ACTION_INVALIDATE_COPY_SCHEDULE  0xA
#define NVC7C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A       0x07b0
#define NVC7C0_SET_SHADER_LOCAL_MEMORY_WINDOW_B       0x07b4

/* Pushbuffer header helpers. */
#define HDR_INC(count, subch, byte_off)                              \
    ((1u << 29) | (((uint32_t)(count) & 0x1FFF) << 16) |             \
     (((uint32_t)(subch) & 0x7) << 13) |                             \
     (((uint32_t)(byte_off) >> 2) & 0x1FFF))

#define HDR_IMMD(subch, byte_off, data)                              \
    ((4u << 29) | (((uint32_t)(data) & 0x1FFF) << 16) |              \
     (((uint32_t)(subch) & 0x7) << 13) |                             \
     (((uint32_t)(byte_off) >> 2) & 0x1FFF))

/* ---- QMD bit-range setters for QMDV03_00 (Ampere) ---------------
 *
 * QMD is 256 bytes = 64 dwords. Fields are specified as bit ranges
 * into this multi-word structure. All MW(hi:lo) sets/clears.
 * Ranges must not cross a 32-bit dword boundary for the 32-bit
 * setter to work; fields that do (PROGRAM_ADDRESS_UPPER, cbuf addr
 * hi on some alignments) need the 64-bit setter.
 */
static void qmd_set_bits(uint32_t *qmd, unsigned hi, unsigned lo, uint64_t val)
{
    unsigned nbits = hi - lo + 1;
    uint64_t mask = (nbits >= 64) ? ~0ULL : ((1ULL << nbits) - 1ULL);
    val &= mask;

    /* Range may span two 32-bit words; handle the single-word common
     * case first, then the two-word spill. */
    unsigned word_lo = lo / 32;
    unsigned word_hi = hi / 32;
    unsigned shift_lo = lo % 32;

    if (word_lo == word_hi) {
        uint32_t wmask = (uint32_t)(mask) << shift_lo;
        qmd[word_lo] = (qmd[word_lo] & ~wmask) |
                       (((uint32_t)val << shift_lo) & wmask);
    } else {
        /* Two words: low bits of val go to word_lo[shift_lo..31];
         * high bits to word_hi[0..(hi%32)]. */
        unsigned bits_in_lo = 32 - shift_lo;
        uint32_t lo_mask = (0xFFFFFFFFu << shift_lo);
        qmd[word_lo] = (qmd[word_lo] & ~lo_mask) |
                       (((uint32_t)val << shift_lo) & lo_mask);

        unsigned bits_in_hi = nbits - bits_in_lo;
        uint32_t hi_mask = (bits_in_hi >= 32) ? 0xFFFFFFFFu :
                           ((1u << bits_in_hi) - 1u);
        uint64_t hi_val = val >> bits_in_lo;
        qmd[word_hi] = (qmd[word_hi] & ~hi_mask) |
                       ((uint32_t)hi_val & hi_mask);
    }
}

/* QMDV03_00 field bit ranges from docs/reference/mesa-clc7c0qmd.h
 * (the AUTO GENERATED comment is NVIDIA's; we mirror the field IDs). */
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

static int xopen(const char *path, int flags)
{
    int fd = open(path, flags);
    if (fd < 0) { perror(path); exit(1); }
    return fd;
}

static void xioctl(int fd, unsigned long req, void *arg, const char *name)
{
    if (ioctl(fd, req, arg) < 0) {
        fprintf(stderr, "%s: %s (errno=%d)\n", name, strerror(errno), errno);
        exit(1);
    }
}

static int nvmap_alloc_dmabuf(int nvmap_fd, uint32_t size, uint32_t align)
{
    struct nvmap_create_handle cr = { .size64 = size };
    xioctl(nvmap_fd, NVMAP_IOC_CREATE_64, &cr, "NVMAP_CREATE");
    uint32_t handle = cr.handle64;

    struct nvmap_alloc_handle al = {
        .handle = handle,
        .heap_mask = 0x40000000,
        .flags = 0x8000003,
        .align = (align < 0x1000) ? 0x1000 : align,
        .numa_nid = -1,
    };
    xioctl(nvmap_fd, NVMAP_IOC_ALLOC, &al, "NVMAP_ALLOC");

    struct nvmap_create_handle gf = { .handle = handle };
    xioctl(nvmap_fd, NVMAP_IOC_GET_FD, &gf, "NVMAP_GET_FD");
    return gf.fd;
}

/* Resolve a process-virtual address to its physical via /proc/self/pagemap. */
static uint64_t virt_to_phys(void *vaddr)
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

static volatile sig_atomic_t g_shutdown;
static void on_term(int sig) { (void)sig; g_shutdown = 1; }

static void *load_file(const char *path, size_t *out_size)
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

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    const char *shader_path = "./write_cafe_shader.sass";
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

    size_t shader_size;
    void *shader_bytes = load_file(shader_path, &shader_size);
    if (!shader_bytes) {
        fprintf(stderr, "shader blob missing at %s (hint: extract .text from "
                "a CUDA cubin: cuobjdump --extract-elf all cuda_write then "
                "dd if=cuda_write.2.sm_87.cubin of=write_cafe_shader.bin "
                "bs=1 skip=$((0x600)) count=$((0x280)))\n", shader_path);
        return 2;
    }
    printf("[launch] shader %zu bytes\n", shader_size);

    /* ---- Channel setup (same as scripts/gpu-channel-helper.c) ---- */
    int nvmap_fd = xopen("/dev/nvmap", O_RDWR);

    int ctrl_fd = open("/dev/nvgpu/igpu0/ctrl", O_RDWR);
    if (ctrl_fd < 0) ctrl_fd = xopen("/dev/nvhost-ctrl-gpu", O_RDWR);

    struct nvgpu_alloc_as_args as_args;
    memset(&as_args, 0, sizeof(as_args));
    as_args.va_range_start = 0x4000000ULL;
    as_args.va_range_end   = 0x2000000000ULL;
    xioctl(ctrl_fd, NVGPU_GPU_IOCTL_ALLOC_AS, &as_args, "ALLOC_AS");
    int as_fd = as_args.as_fd;

    struct nvgpu_gpu_open_tsg_args tsg_args;
    memset(&tsg_args, 0, sizeof(tsg_args));
    xioctl(ctrl_fd, NVGPU_GPU_IOCTL_OPEN_TSG, &tsg_args, "OPEN_TSG");
    int tsg_fd = tsg_args.tsg_fd;

    struct nvgpu_gpu_open_channel_args ch_args;
    memset(&ch_args, 0, sizeof(ch_args));
    ch_args.in.runlist_id = -1;
    xioctl(ctrl_fd, NVGPU_GPU_IOCTL_OPEN_CHANNEL, &ch_args, "OPEN_CHANNEL");
    int ch_fd = ch_args.out.channel_fd;

    struct nvgpu_as_bind_channel_args bind_as = { .channel_fd = ch_fd };
    xioctl(as_fd, NVGPU_AS_IOCTL_BIND_CHANNEL, &bind_as, "AS_BIND");

    struct nvgpu_tsg_create_subcontext_args subctx;
    memset(&subctx, 0, sizeof(subctx));
    subctx.type  = NVGPU_TSG_SUBCONTEXT_TYPE_ASYNC;
    subctx.as_fd = as_fd;
    xioctl(tsg_fd, NVGPU_TSG_IOCTL_CREATE_SUBCONTEXT, &subctx,
           "TSG_CREATE_SUBCONTEXT");

    struct nvgpu_tsg_bind_channel_ex_args bce;
    memset(&bce, 0, sizeof(bce));
    bce.channel_fd    = ch_fd;
    bce.subcontext_id = subctx.veid;
    xioctl(tsg_fd, NVGPU_TSG_IOCTL_BIND_CHANNEL_EX, &bce,
           "TSG_BIND_CHANNEL_EX");

    struct nvgpu_set_nvmap_fd_args nvm = { .fd = nvmap_fd };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_SET_NVMAP_FD, &nvm, "SET_NVMAP");

    struct nvgpu_channel_wdt_args wdt = {
        .wdt_status = NVGPU_IOCTL_CHANNEL_DISABLE_WDT |
                      NVGPU_IOCTL_CHANNEL_WDT_FLAG_SET_TIMEOUT,
        .timeout_ms = 0xFFFFFFFFu,
    };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_WDT, &wdt, "WDT_DISABLE");

    int userd_dmabuf  = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);
    int gpfifo_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 8192, 4096);

    struct nvgpu_channel_setup_bind_args sb;
    memset(&sb, 0, sizeof(sb));
    sb.num_gpfifo_entries = 1024;
    sb.flags = NVGPU_CHANNEL_SETUP_BIND_FLAGS_DETERMINISTIC |
               NVGPU_CHANNEL_SETUP_BIND_FLAGS_USERMODE_SUPPORT;
    sb.userd_dmabuf_fd  = userd_dmabuf;
    sb.gpfifo_dmabuf_fd = gpfifo_dmabuf;
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_SETUP_BIND, &sb, "SETUP_BIND");
    printf("[launch] work_submit_token=0x%x\n", sb.work_submit_token);

    struct nvgpu_alloc_obj_ctx_args octx = {
        .class_num = CLASS_AMPERE_COMPUTE_B,
    };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_ALLOC_OBJ_CTX, &octx, "ALLOC_OBJ_CTX");

    struct nvgpu_preemption_mode_args pm = {
        .compute_preempt_mode = NVGPU_COMPUTE_PREEMPTION_MODE_CILP,
    };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_SET_PREEMPTION_MODE, &pm,
           "SET_PREEMPT_MODE");

    int notifier_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);
    struct nvgpu_set_error_notifier en = {
        .offset = 0, .size = 4096, .mem = notifier_dmabuf,
    };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_SET_ERROR_NOTIFIER, &en,
           "SET_ERROR_NOTIFIER");

    /* ---- Per-kernel allocations ------------------------------------
     * Shader: 4 KB buffer (640 B + padding). Must be page-aligned.
     * Cbuf0: 512 B buffer. CUDA's param offset is 0x160 into cbuf0.
     * Output: 4 KB, zero-initialized; kernel writes 0xCAFE here.
     * QMD: 256 B buffer, must be 256 B aligned, separate allocation.
     * Pushbuffer: 64 KB. */
    int pb_dmabuf     = nvmap_alloc_dmabuf(nvmap_fd, 65536, 4096);
    int shader_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);
    int cbuf_dmabuf   = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);
    int out_dmabuf    = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);
    int qmd_dmabuf    = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);

    struct nvgpu_gpu_register_buffer_args regbuf;
    int reg_fds[] = { pb_dmabuf, shader_dmabuf, cbuf_dmabuf, out_dmabuf,
                      qmd_dmabuf };
    for (size_t i = 0; i < sizeof(reg_fds)/sizeof(reg_fds[0]); i++) {
        memset(&regbuf, 0, sizeof(regbuf));
        regbuf.dmabuf_fd = reg_fds[i];
        regbuf.comptags_alloc_control = NVGPU_GPU_COMPTAGS_ALLOC_NONE;
        ioctl(ctrl_fd, NVGPU_GPU_IOCTL_REGISTER_BUFFER, &regbuf);
    }

    struct nvgpu_as_map_buffer_ex_args map;
    /* Map pushbuffer. */
    memset(&map, 0, sizeof(map));
    map.compr_kind = -1;
    map.dmabuf_fd = pb_dmabuf;
    map.page_size = 4096;
    xioctl(as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &map, "MAP_PB");
    uint64_t pb_gva = map.offset;

    memset(&map, 0, sizeof(map));
    map.compr_kind = -1;
    map.dmabuf_fd = shader_dmabuf;
    map.page_size = 4096;
    /* Shader needs EXECUTE-capable mapping. compr_kind=-1 gets PITCH
     * kind which is readable; NVK uses `NVKMD_MEM_LOCAL` with default
     * kind for shaders and it works. If the kernel traps on instruction
     * fetch, we may need to force a specific kind here. */
    xioctl(as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &map, "MAP_SHADER");
    uint64_t shader_gva = map.offset;

    memset(&map, 0, sizeof(map));
    map.compr_kind = -1;
    map.dmabuf_fd = cbuf_dmabuf;
    map.page_size = 4096;
    xioctl(as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &map, "MAP_CBUF");
    uint64_t cbuf_gva = map.offset;

    memset(&map, 0, sizeof(map));
    map.compr_kind = -1;
    map.dmabuf_fd = out_dmabuf;
    map.page_size = 4096;
    xioctl(as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &map, "MAP_OUT");
    uint64_t out_gva = map.offset;

    memset(&map, 0, sizeof(map));
    map.compr_kind = -1;
    map.dmabuf_fd = qmd_dmabuf;
    map.page_size = 4096;
    xioctl(as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &map, "MAP_QMD");
    uint64_t qmd_gva = map.offset;

    printf("[launch] pb_gva=0x%lx shader_gva=0x%lx cbuf_gva=0x%lx "
           "out_gva=0x%lx qmd_gva=0x%lx\n",
           (unsigned long)pb_gva, (unsigned long)shader_gva,
           (unsigned long)cbuf_gva, (unsigned long)out_gva,
           (unsigned long)qmd_gva);

    xioctl(tsg_fd, NVGPU_IOCTL_TSG_ENABLE, NULL, "TSG_ENABLE");

    /* CPU mappings for initialization. */
    void *userd_va  = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                           userd_dmabuf, 0);
    void *gpfifo_va = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED,
                           gpfifo_dmabuf, 0);
    void *pb_va     = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED,
                           pb_dmabuf, 0);
    void *shader_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                           shader_dmabuf, 0);
    void *cbuf_va   = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                           cbuf_dmabuf, 0);
    void *out_va    = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                           out_dmabuf, 0);
    void *qmd_va    = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                           qmd_dmabuf, 0);
    if (userd_va == MAP_FAILED || gpfifo_va == MAP_FAILED ||
        pb_va == MAP_FAILED || shader_va == MAP_FAILED ||
        cbuf_va == MAP_FAILED || out_va == MAP_FAILED ||
        qmd_va == MAP_FAILED) {
        perror("mmap"); return 1;
    }

    /* Upload shader. */
    memset(shader_va, 0, 4096);
    memcpy(shader_va, shader_bytes, shader_size);
    msync(shader_va, 4096, MS_SYNC);

    /* Populate cbuf0. CUDA kernels read their arguments from cbuf[0]
     * starting at offset 0x160. The "write_cafe" kernel takes a single
     * pointer argument, so put the 64-bit output VA at cbuf[0][0x160]. */
    memset(cbuf_va, 0, 4096);
    *(uint64_t *)((char *)cbuf_va + 0x160) = out_gva;
    msync(cbuf_va, 4096, MS_SYNC);

    /* Clear output buffer. */
    memset(out_va, 0, 4096);
    msync(out_va, 4096, MS_SYNC);

    /* ---- Build QMD (QMDV03_00, 256 B) -------------------------------
     * Mirrors mesa/nak_qmd.rs:fill_qmd<Qmd3_0> for cls_compute=AMPERE_B. */
    uint32_t *qmd = (uint32_t *)qmd_va;
    memset(qmd, 0, 256);

    /* qmd_init: major=3, minor=0, API_VISIBLE_CALL_LIMIT=NO_CHECK(1),
     * SAMPLER_INDEX=INDEPENDENTLY(0). */
    qmd_set_bits(qmd, QMD_MAJOR_VERSION_HI, QMD_MAJOR_VERSION_LO, 3);
    qmd_set_bits(qmd, QMD_VERSION_HI, QMD_VERSION_LO, 0);
    qmd_set_bits(qmd, QMD_API_VISIBLE_CALL_LIMIT_BIT,
                 QMD_API_VISIBLE_CALL_LIMIT_BIT, 1);
    qmd_set_bits(qmd, QMD_SAMPLER_INDEX_BIT, QMD_SAMPLER_INDEX_BIT, 0);

    /* Grid + block dims: 1 CTA, 1 thread. */
    qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI, QMD_CTA_RASTER_WIDTH_LO, 1);
    qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI, QMD_CTA_RASTER_HEIGHT_LO, 1);
    qmd_set_bits(qmd, QMD_CTA_RASTER_DEPTH_HI, QMD_CTA_RASTER_DEPTH_LO, 1);
    qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI, QMD_CTA_THREAD_DIM0_LO, 1);
    qmd_set_bits(qmd, QMD_CTA_THREAD_DIM1_HI, QMD_CTA_THREAD_DIM1_LO, 1);
    qmd_set_bits(qmd, QMD_CTA_THREAD_DIM2_HI, QMD_CTA_THREAD_DIM2_LO, 1);

    /* Program address: absolute 64-bit GPU VA, no shift on Ampere. */
    qmd_set_bits(qmd, QMD_PROGRAM_ADDRESS_LOWER_HI,
                 QMD_PROGRAM_ADDRESS_LOWER_LO,
                 shader_gva & 0xFFFFFFFFu);
    qmd_set_bits(qmd, QMD_PROGRAM_ADDRESS_UPPER_HI,
                 QMD_PROGRAM_ADDRESS_UPPER_LO,
                 (shader_gva >> 32) & 0x1FFFFu);  /* 17 bits */

    /* Register count: generous 128 (the cubin's .nv.info claims 126).
     * Shmem + SLM: 0 (kernel doesn't use either). Barrier: 0. */
    qmd_set_bits(qmd, QMD_REGISTER_COUNT_V_HI, QMD_REGISTER_COUNT_V_LO, 128);
    qmd_set_bits(qmd, QMD_SHARED_MEMORY_SIZE_HI, QMD_SHARED_MEMORY_SIZE_LO, 0);
    qmd_set_bits(qmd, QMD_SHADER_LOCAL_MEM_LOW_SIZE_HI,
                 QMD_SHADER_LOCAL_MEM_LOW_SIZE_LO, 0);
    qmd_set_bits(qmd, QMD_SHADER_LOCAL_MEM_HIGH_SIZE_HI,
                 QMD_SHADER_LOCAL_MEM_HIGH_SIZE_LO, 0);
    qmd_set_bits(qmd, QMD_BARRIER_COUNT_HI, QMD_BARRIER_COUNT_LO, 0);

    /* Enable SM global caching, per NVK's set_enum in qmd_init. */
    qmd_set_bits(qmd, QMD_SM_GLOBAL_CACHING_ENABLE_BIT,
                 QMD_SM_GLOBAL_CACHING_ENABLE_BIT, 1);

    /* Invalidate all caches — safer for first dispatch after channel
     * setup. Individual bits exactly mirror NVC7C0_QMDV03_00_INVALIDATE_*. */
    qmd_set_bits(qmd, QMD_INVALIDATE_TEXTURE_HEADER_CACHE_BIT,
                 QMD_INVALIDATE_TEXTURE_HEADER_CACHE_BIT, 1);
    qmd_set_bits(qmd, QMD_INVALIDATE_TEXTURE_SAMPLER_CACHE_BIT,
                 QMD_INVALIDATE_TEXTURE_SAMPLER_CACHE_BIT, 1);
    qmd_set_bits(qmd, QMD_INVALIDATE_TEXTURE_DATA_CACHE_BIT,
                 QMD_INVALIDATE_TEXTURE_DATA_CACHE_BIT, 1);
    qmd_set_bits(qmd, QMD_INVALIDATE_SHADER_DATA_CACHE_BIT,
                 QMD_INVALIDATE_SHADER_DATA_CACHE_BIT, 1);
    qmd_set_bits(qmd, QMD_INVALIDATE_INSTRUCTION_CACHE_BIT,
                 QMD_INVALIDATE_INSTRUCTION_CACHE_BIT, 1);
    qmd_set_bits(qmd, QMD_INVALIDATE_SHADER_CONSTANT_CACHE_BIT,
                 QMD_INVALIDATE_SHADER_CONSTANT_CACHE_BIT, 1);

    /* Set cbuf[0]: CUDA-compatible param area. Address is 64-bit abs VA
     * (lower 32 + upper 17). Size is shifted 4 (so size_in_qmd =
     * actual_bytes >> 4). NVK's per-cbuf alignment requirement on
     * Ampere is 256 B. We use 512 B. */
    const unsigned cbuf0 = 0;
    const uint32_t cbuf_size_B = 512;
    qmd_set_bits(qmd, QMD_CBUF_ADDR_LO_BASE + cbuf0 * 64 + 31,
                 QMD_CBUF_ADDR_LO_BASE + cbuf0 * 64,
                 cbuf_gva & 0xFFFFFFFFu);
    qmd_set_bits(qmd, QMD_CBUF_ADDR_HI_BASE + cbuf0 * 64 + 16,
                 QMD_CBUF_ADDR_HI_BASE + cbuf0 * 64,
                 (cbuf_gva >> 32) & 0x1FFFFu);
    qmd_set_bits(qmd, QMD_CBUF_SIZE_SHIFTED4_BASE + cbuf0 * 64 + 12,
                 QMD_CBUF_SIZE_SHIFTED4_BASE + cbuf0 * 64,
                 cbuf_size_B >> 4);
    qmd_set_bits(qmd, QMD_CBUF_VALID_BASE + cbuf0,
                 QMD_CBUF_VALID_BASE + cbuf0, 1);

    msync(qmd_va, 4096, MS_SYNC);

    printf("[launch] QMD dwords:\n");
    for (int i = 0; i < 64; i++) {
        printf("  qmd[%02d] = 0x%08x%s", i, qmd[i],
               ((i + 1) % 4 == 0) ? "\n" : "");
    }

    /* ---- Build pushbuffer -----------------------------------------
     * Structure mirrors nvk_push_dispatch_state_init + nvk_cmd_buffer_begin_compute:
     *   1. SET_OBJECT(COMPUTE_B) — bind compute class on subch 1.
     *   2. SET_SHADER_SHARED_MEMORY_WINDOW_A/B = (0, 0xfe000000)
     *      SET_SHADER_LOCAL_MEMORY_WINDOW_A/B  = (0, 0xff000000)
     *      These are the compute pipeline's address-space windows.
     *      Required before first dispatch on Ampere — without them the
     *      kernel silently no-ops (observed: GP_GET advances but QMD
     *      doesn't run).
     *   3. Cache invalidates — NVK emits these on every command-buffer
     *      begin, which implicitly happens once per queue init.
     *   4. SEND_PCAS_A: QMD address shifted 8 (QMD must be 256 B aligned).
     *   5. SEND_SIGNALING_PCAS_B: trigger dispatch with INVALIDATE + SCHEDULE. */
    uint32_t *pb32 = (uint32_t *)pb_va;
    uint32_t *p = pb32;

    *p++ = HDR_INC(1, SUBCH_COMPUTE, NVC7C0_SET_OBJECT);
    *p++ = CLASS_AMPERE_COMPUTE_B;

    *p++ = HDR_INC(2, SUBCH_COMPUTE, NVC7C0_SET_SHADER_SHARED_MEMORY_WINDOW_A);
    *p++ = 0;                  /* upper 17 bits */
    *p++ = 0xfe000000;         /* lower 32 bits */

    *p++ = HDR_INC(2, SUBCH_COMPUTE, NVC7C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A);
    *p++ = 0;
    *p++ = 0xff000000;

    *p++ = HDR_IMMD(SUBCH_COMPUTE, NVC7C0_INVALIDATE_SKED_CACHES, 0);
    *p++ = HDR_IMMD(SUBCH_COMPUTE,
                    NVC7C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0);

    *p++ = HDR_INC(1, SUBCH_COMPUTE, NVC7C0_SEND_PCAS_A);
    *p++ = (uint32_t)(qmd_gva >> 8);

    /* Ampere dispatches via PCAS2_B with INVALIDATE_COPY_SCHEDULE
     * (action 0xA) per NVK's `cls_compute > TURING_COMPUTE_A` branch. */
    *p++ = HDR_IMMD(SUBCH_COMPUTE, NVC7C0_SEND_SIGNALING_PCAS2_B,
                    NVC7C0_SEND_SIGNALING_PCAS2_B_ACTION_INVALIDATE_COPY_SCHEDULE);

    size_t pb_dw = p - pb32;
    msync(pb_va, pb_dw * 4, MS_SYNC);
    printf("[launch] pushbuffer %zu dwords\n", pb_dw);
    for (size_t i = 0; i < pb_dw; i++) {
        printf("  pb[%02zu] = 0x%08x\n", i, pb32[i]);
    }

    /* GPFIFO entry 0 -> pushbuffer. */
    uint32_t gp_e0 = (uint32_t)(pb_gva & 0xFFFFFFFCu);
    uint32_t gp_e1 = (uint32_t)((pb_gva >> 32) & 0xFFu) |
                     ((uint32_t)pb_dw << 10);
    ((uint32_t *)gpfifo_va)[0] = gp_e0;
    ((uint32_t *)gpfifo_va)[1] = gp_e1;
    msync(gpfifo_va, 8, MS_SYNC);

    /* GP_PUT = 1 in USERD. */
    ((uint32_t *)userd_va)[USERD_GP_PUT_WORD] = 1;
    msync(userd_va, 4096, MS_SYNC);

    /* Doorbell. */
    void *doorbell_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_SHARED, ctrl_fd, 0);
    if (doorbell_page == MAP_FAILED) { perror("mmap doorbell"); return 1; }
    volatile uint32_t *doorbell =
        (volatile uint32_t *)((char *)doorbell_page + 0x90);
    *doorbell = sb.work_submit_token;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Poll output for up to 2s. `msync(MS_INVALIDATE)` flushes CPU
     * caches before re-reading so a GPU-written value isn't masked by
     * a stale CPU cacheline — on Jetson's integrated GPU the coherency
     * between GPU writes and CPU reads is not automatic for nvmap
     * IOVMM mappings. */
    uint32_t val = 0;
    for (int i = 0; i < 200; i++) {
        msync(out_va, 4096, MS_INVALIDATE | MS_SYNC);
        __asm__ volatile("dsb sy" ::: "memory");
        val = *(volatile uint32_t *)out_va;
        if (val == SEM_PAYLOAD) break;
        usleep(10000);
    }
    uint32_t gp_get = ((volatile uint32_t *)userd_va)[USERD_GP_GET_WORD];
    printf("[launch] result: out=0x%08x (want 0x%x) GP_GET=%u (want 1)\n",
           val, SEM_PAYLOAD, gp_get);
    if (val == SEM_PAYLOAD) {
        printf("[launch] SUCCESS: kernel ran\n");
    } else if (gp_get == 1) {
        printf("[launch] PARTIAL: pushbuffer consumed, kernel did not run\n");
    } else {
        printf("[launch] FAIL: PBDMA did not advance\n");
    }

    if (!preserve) {
        close(ch_fd);
        close(tsg_fd);
        close(as_fd);
        close(ctrl_fd);
        close(nvmap_fd);
        free(shader_bytes);
        return (val == SEM_PAYLOAD) ? 0 : 1;
    }

    /* --- --preserve-for-kexec path ---
     * Success means the kernel + QMD + channel state is all in working
     * order. Publish a v3 handoff block into DRAM so SLM-OS can find
     * it post-kexec and re-fire the kernel from bare metal. Keep all
     * the nvgpu fds open so the channel + GPU mappings stay live
     * through the kexec. */
    if (val != SEM_PAYLOAD) {
        fprintf(stderr, "[launch] kernel failed pre-kexec; refusing to "
                "preserve (channel state is suspect)\n");
        return 1;
    }

    int handoff_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);
    void *handoff = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                         handoff_dmabuf, 0);
    if (handoff == MAP_FAILED) { perror("mmap handoff"); return 1; }
    memset(handoff, 0, 4096);
    uint64_t handoff_phys = virt_to_phys(handoff);

    /* Resolve physical addresses of the channel / kernel buffers. */
    uint64_t userd_phys  = virt_to_phys(userd_va);
    uint64_t gpfifo_phys = virt_to_phys(gpfifo_va);
    uint64_t pb_phys     = virt_to_phys(pb_va);
    uint64_t shader_phys = virt_to_phys(shader_va);
    uint64_t cbuf_phys   = virt_to_phys(cbuf_va);
    uint64_t qmd_phys    = virt_to_phys(qmd_va);
    uint64_t out_phys    = virt_to_phys(out_va);

    /* Populate handoff via the shared struct (v3 layout).
     * USERD_GP_{PUT,GET}_WORD come from the file-scope macros; the
     * GP_*-snapshot reads below use the same constants so the
     * handoff's stored offsets can't drift from what we observed. */
    struct ga10b_channel_handoff hoff = {
        .magic              = GA10B_CHANNEL_HANDOFF_MAGIC,
        .version            = 3,
        .channel_id         = 0,
        .tsg_id             = 0,
        .userd_phys         = userd_phys,
        .userd_gp_put_offset = USERD_GP_PUT_WORD * 4u,
        .userd_gp_get_offset = USERD_GP_GET_WORD * 4u,
        .gpfifo_phys        = gpfifo_phys,
        .gpfifo_gpu_va      = sb.gpfifo_gpu_va,
        .gpfifo_entries     = 1024,
        .gpfifo_entry_size  = 8,
        .pushbuf_phys       = pb_phys,
        .pushbuf_gpu_va     = pb_gva,
        .pushbuf_size       = 65536,
        /* Semaphore_phys reuses the kernel output buffer — `nvgpu
         * channel` + `nvgpu submit` (host-family SEMAPHORE_RELEASE)
         * writes the sema at this VA, which is the same location the
         * compute kernel writes 0xCAFE into. Both flows coexist. */
        .semaphore_phys     = out_phys,
        .semaphore_gpu_va   = out_gva,
        .inst_block_phys    = 0,
        /* GP_PUT / GP_GET snapshot via the same word indices. */
        .initial_gp_put     = ((volatile uint32_t *)userd_va)[USERD_GP_PUT_WORD],
        .initial_gp_get     = ((volatile uint32_t *)userd_va)[USERD_GP_GET_WORD],
        .work_submit_token  = sb.work_submit_token,
        /* v3 extension: compute-kernel state. */
        .shader_phys        = shader_phys,
        .shader_gpu_va      = shader_gva,
        .cbuf_phys          = cbuf_phys,
        .cbuf_gpu_va        = cbuf_gva,
        .qmd_phys           = qmd_phys,
        .qmd_gpu_va         = qmd_gva,
        .output_phys        = out_phys,
        .output_gpu_va      = out_gva,
        .shader_size        = (uint32_t)shader_size,
        .cbuf_size          = 512,
    };
    memcpy(handoff, &hoff, sizeof(hoff));
    msync(handoff, 4096, MS_SYNC);

    printf("[launch] Handoff at phys 0x%llx (version=3)\n",
           (unsigned long long)handoff_phys);
    printf("[launch]   shader_phys=0x%llx  shader_gva=0x%llx (%zu B)\n",
           (unsigned long long)shader_phys, (unsigned long long)shader_gva,
           shader_size);
    printf("[launch]   cbuf_phys=0x%llx  cbuf_gva=0x%llx\n",
           (unsigned long long)cbuf_phys, (unsigned long long)cbuf_gva);
    printf("[launch]   qmd_phys=0x%llx  qmd_gva=0x%llx\n",
           (unsigned long long)qmd_phys, (unsigned long long)qmd_gva);
    printf("[launch]   output_phys=0x%llx  output_gva=0x%llx\n",
           (unsigned long long)out_phys, (unsigned long long)out_gva);
    printf("[launch] Sleeping up to %d s — kexec now:\n", timeout_secs);
    printf("[launch]   sudo slmos-kexec --no-gpu-suspend /tmp/slmos.elf\n");

    /* Sleep in bounded slices so SIGTERM responsiveness stays snappy. */
    for (int remaining = timeout_secs; remaining > 0 && !g_shutdown; ) {
        int slice = remaining > 60 ? 60 : remaining;
        sleep(slice);
        remaining -= slice;
    }
    printf("[launch] Exiting (%s) — channel releasing.\n",
           g_shutdown ? "signal" : "timeout");

    close(ch_fd);
    close(tsg_fd);
    close(as_fd);
    close(ctrl_fd);
    close(nvmap_fd);
    free(shader_bytes);
    return 0;
}
