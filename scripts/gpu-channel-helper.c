/*
 * gpu-channel-helper.c — Create an nvgpu channel on Jetson and write
 * the handoff metadata for SLM-OS to inherit after kexec.
 *
 * ============================================================
 *   STATUS: pre-kexec isolation test fires sema on Jetson, 2026-04-18
 * ============================================================
 *
 * All 10 ioctls succeed; handoff block (wire v2, carrying
 * work_submit_token) is written to an nvmap dmabuf; the magic value
 * survives kexec and is found by SLM-OS's Phase 6 DRAM scan. Ioctl
 * parameters were reverse-engineered from CUDA via an LD_PRELOAD
 * ioctl-snoop (see commit history).
 *
 * Phase 7 pre-kexec isolation: helper writes a PBDMA host-family
 * SEMAPHORE_RELEASE pushbuffer, rings the USERMODE doorbell at
 * offset 0x90 within the CTRL mmap (BAR0+0xBB0090), and observes
 * the GPU write `0x0000CAFE` at the target VA. Method encoding
 * matches nvgpu's gv11b sema cmdbuf (method_id at [12:0], not
 * byte_off at [11:0] — see kernel/gpu/nvidia/ga10b_bringup.c for
 * the same fix on the SLM-OS side). The SLM-OS-side post-kexec
 * submit using the same encoding is tracked for re-validation in
 * issue #297; PBDMA's GP_GET advance alone is no longer treated
 * as proof of method dispatch after the 2026-04-18 encoding
 * investigation.
 *
 * Usage: sudo ./gpu-channel-helper [--timeout-secs N]
 *
 * This program:
 *   1. Opens a TSG + channel + address space via nvgpu ioctls
 *   2. Allocates GPFIFO, USERD, pushbuffer, and semaphore buffers
 *   3. Sets up the channel (SETUP_BIND with USERMODE_SUPPORT)
 *   4. Maps pushbuffer + semaphore into the GPU address space
 *   5. Writes the channel handoff block to a dmabuf, prints its
 *      physical address, and writes the struct contents via the
 *      shared handoff header so field offsets cannot drift
 *   6. Sleeps for --timeout-secs (default 300) — the kexec helper
 *      runs while this sleeps. SIGTERM cleans up the channel.
 *
 * The channel stays alive in the GPU's CHRAM because we don't close
 * the file descriptors. After kexec with --no-gpu-suspend, PBDMA
 * continues to monitor the channel. SLM-OS writes to USERD GP_PUT
 * to submit work.
 *
 * Compile on Jetson: gcc -O2 -o gpu-channel-helper gpu-channel-helper.c
 * Rollback: rm gpu-channel-helper gpu-channel-helper.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <errno.h>
#include <linux/types.h>

/* Include the nvgpu UAPI headers from the L4T source tree. */
#include "/usr/src/nvidia/nvgpu/include/uapi/linux/nvgpu.h"
#include "/usr/src/nvidia/nvgpu/include/uapi/linux/nvgpu-as.h"
#include "/usr/src/nvidia/nvgpu/include/uapi/linux/nvgpu-ctrl.h"
#include "/usr/src/nvidia/nvidia-oot/include/uapi/linux/nvmap.h"

/* Shared launcher constants + nvgpu UAPI; transitively pulls in the
 * kernel handoff header (../kernel/gpu/nvidia/ga10b_channel_handoff.h),
 * so the struct layout / magic / GPU_LAUNCH_GPFIFO_* sizing constants
 * stay in lock-step with what SLM-OS expects. */
#include "gpu-launch-common.h"
#include <signal.h>

/* Default time to keep the channel alive waiting for kexec. Override
 * with --timeout-secs. */
#define DEFAULT_TIMEOUT_SECS  300

/* Semaphore payload for the pre-kexec isolation test. MUST stay in
 * lockstep with GA10B_SMOKETEST_SEM_PAYLOAD in
 * kernel/gpu/nvidia/ga10b_bringup.c — the SLM-OS-side smoke test uses
 * the same payload, so a divergence would cause the helper's pre-kexec
 * check and SLM-OS's post-kexec check to disagree on "did the method
 * fire?" for no reason. */
#define HELPER_SMOKETEST_SEM_PAYLOAD  0x0000CAFEu

/* Helper: open a device, die on failure. */
static int xopen(const char *path, int flags)
{
    int fd = open(path, flags);
    if (fd < 0) { perror(path); exit(1); }
    return fd;
}

/* Helper: ioctl, die on failure. */
static void xioctl(int fd, unsigned long req, void *arg, const char *name)
{
    if (ioctl(fd, req, arg) < 0) {
        fprintf(stderr, "%s: %s (errno=%d, ioctl=0x%lx, fd=%d)\n",
                name, strerror(errno), errno, req, fd);
        exit(1);
    }
}

/* Forward declare for use by the helper buffer allocator below. */
static int nvmap_alloc_dmabuf(int nvmap_fd, uint64_t size, uint32_t align);
static uint64_t virt_to_phys(void *vaddr);

/* Allocate a single dmabuf, register it with nvgpu, GMMU-map it
 * into the channel's address space, mmap it for CPU access, zero
 * the contents, and resolve its physical address. Used by the
 * QMD-pool / SASS-pool / cbuf allocations below — all share the
 * exact same setup sequence and only differ in size.
 *
 * Returns 0 on success, -1 on failure. On failure the helper
 * returns immediately to its caller, which exits the process
 * (`return 1` from `main`); we deliberately do NOT roll back
 * partial state (close dmabuf fd, munmap, etc.) on intermediate
 * failures because process exit is the cleanup. If a future
 * caller invokes this from a long-running context, it must add
 * its own cleanup-on-error path.
 *
 * `*out_phys` and `*out_gpu_va` are set only on success; the
 * caller MUST treat the buffer as page-rounded — the actual
 * allocation is `round_up(size_bytes, 4 KB)` bytes, which the
 * caller can compute the same way (`(size + 4095u) & ~4095u`).
 * The handoff fields that record buffer size should reflect the
 * rounded value, not the requested `size_bytes`. */
/* `size_bytes` is uint64_t so the W1 weights pool (~1.5 GB design
 * target) can use the same helper. `zero_fill=true` matches the
 * pre-W1 behavior (zero the whole buffer before handoff); pass
 * false for the weights pool so we don't burn ~50 ms memset'ing
 * 1.5 GB that SLM-OS will overwrite with weight bytes anyway.
 *
 * `phys_required=true` aborts the allocation if /proc/self/pagemap
 * can't resolve the first page's PFN. The weights pool can pass
 * `false` here because IOVMM-backed dmabufs that are big enough
 * to span many SMMU-stitched pages don't have a meaningful
 * "first physical page" anyway — SLM-OS uses gpu_va for that
 * pool, and a 0 in `weights_pool_phys` is the sentinel saying
 * "informational only / not usable for offset arithmetic". */
static int alloc_channel_buffer(int nvmap_fd, int ctrl_fd, int as_fd,
                                uint64_t size_bytes, const char *tag,
                                bool zero_fill, bool phys_required,
                                int *out_dmabuf, void **out_cpu_va,
                                uint64_t *out_phys, uint64_t *out_gpu_va)
{
    const uint64_t page_size = 4096u;
    uint64_t rounded = (size_bytes + page_size - 1ull) & ~(page_size - 1ull);

    int dmabuf = nvmap_alloc_dmabuf(nvmap_fd, rounded, (uint32_t)page_size);

    struct nvgpu_gpu_register_buffer_args regbuf;
    memset(&regbuf, 0, sizeof(regbuf));
    regbuf.dmabuf_fd = dmabuf;
    regbuf.comptags_alloc_control = NVGPU_GPU_COMPTAGS_ALLOC_NONE;
    if (ioctl(ctrl_fd, NVGPU_GPU_IOCTL_REGISTER_BUFFER, &regbuf) < 0) {
        fprintf(stderr,
                "[gpu-helper] REGISTER_BUFFER(%s) failed: %s\n",
                tag, strerror(errno));
        return -1;
    }

    struct nvgpu_as_map_buffer_ex_args map;
    memset(&map, 0, sizeof(map));
    map.compr_kind = -1;
    map.incompr_kind = 0;
    map.dmabuf_fd = dmabuf;
    map.mapping_size = 0;
    map.page_size = (uint32_t)page_size;
    if (ioctl(as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &map) < 0) {
        fprintf(stderr,
                "[gpu-helper] MAP_BUFFER_EX(%s) failed: %s\n",
                tag, strerror(errno));
        return -1;
    }

    void *cpu_va = mmap(NULL, (size_t)rounded, PROT_READ | PROT_WRITE,
                        MAP_SHARED, dmabuf, 0);
    if (cpu_va == MAP_FAILED) {
        fprintf(stderr, "[gpu-helper] mmap(%s): %s\n", tag, strerror(errno));
        return -1;
    }
    if (zero_fill) {
        memset(cpu_va, 0, (size_t)rounded);
        msync(cpu_va, (size_t)rounded, MS_SYNC);
    } else {
        /* Skip memset for !zero_fill callers — saves ~150 ms on
         * 1.5 GB pools. SLM-OS's first weight upload will write
         * over each page anyway. */
    }

    uint64_t phys = virt_to_phys(cpu_va);
    if (phys == 0) {
        if (phys_required) {
            fprintf(stderr,
                    "[gpu-helper] virt_to_phys returned 0 for %s\n",
                    tag);
            return -1;
        }
        /* Non-fatal: GB-scale IOVMM dmabufs span many
         * SMMU-stitched pages, so even when the first page is
         * resident /proc/self/pagemap may not expose a usable
         * PFN. The caller (weights pool) doesn't rely on this
         * value — it's informational, and SLM-OS uses gpu_va. */
        fprintf(stderr,
                "[gpu-helper] %s: virt_to_phys unavailable, "
                "publishing phys=0 (gpu_va is authoritative)\n",
                tag);
    }

    *out_dmabuf = dmabuf;
    *out_cpu_va = cpu_va;
    *out_phys = phys;
    *out_gpu_va = map.offset;
    return 0;
}

/* Allocate an nvmap buffer and return a dmabuf fd for it. */
static int nvmap_alloc_dmabuf(int nvmap_fd, uint64_t size, uint32_t align)
{
    /* Create handle */
    struct nvmap_create_handle cr = { .size64 = size };
    xioctl(nvmap_fd, NVMAP_IOC_CREATE_64, &cr, "NVMAP_CREATE");
    uint32_t handle = cr.handle64;

    /* Allocate backing memory. Values captured from CUDA's own
     * invocation via LD_PRELOAD ioctl snoop on L4T r36.4.7. */
    struct nvmap_alloc_handle al = {
        .handle = handle,
        .heap_mask = 0x40000000,  /* IOVMM heap */
        .flags = 0x8000003,       /* cacheable + some nvmap-specific bits */
        .align = (align < 0x1000) ? 0x1000 : align,
        .numa_nid = -1,
    };
    xioctl(nvmap_fd, NVMAP_IOC_ALLOC, &al, "NVMAP_ALLOC");

    /* Get dmabuf fd */
    struct nvmap_create_handle gf = { .handle = handle };
    xioctl(nvmap_fd, NVMAP_IOC_GET_FD, &gf, "NVMAP_GET_FD");
    return gf.fd;
}

/* Get physical address of a page via /proc/self/pagemap. */
static uint64_t virt_to_phys(void *vaddr)
{
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) { perror("pagemap"); return 0; }
    uint64_t vpage = (uint64_t)vaddr / 4096;
    uint64_t entry;
    if (pread(fd, &entry, 8, vpage * 8) != 8) {
        perror("pagemap read");
        close(fd);
        return 0;
    }
    close(fd);
    if (!(entry & (1ULL << 63))) {
        fprintf(stderr, "page not present for %p\n", vaddr);
        return 0;
    }
    uint64_t pfn = entry & ((1ULL << 55) - 1);
    return pfn * 4096 + ((uint64_t)vaddr & 0xFFF);
}

/* Read FECS_CURRENT_CTX via /dev/mem and decode the channel inst-
 * block phys it points to. Used to publish `inst_block_phys` in the
 * handoff so SLM-OS's post-kexec `nvgpu oplib stage` can locate the
 * inherited channel's GMMU root without scanning DRAM.
 *
 * Background (#788 Stage 9): SLM-OS captures FECS_CURRENT_CTX at
 * boot, but by that time the live register may show a *different*
 * channel than the helper's — Xorg / nvgpu's GR-internal channel /
 * etc. were GR-current when kexec landed. Reading FECS here in the
 * helper's process gives us a chance to capture the value while our
 * channel is more likely to be the current GR context — and even if
 * it's wrong, having a concrete address in the handoff lets SLM-OS's
 * existing `if (h->inst_block_phys != 0)` fast path try it before
 * falling back to walk-based discovery.
 *
 * GA10B FECS_CURRENT_CTX encoding (per
 * `~/slmos-ref/nvidia/nvgpu-include-nvgpu-hw-gv11b-hw_gr_gv11b.h`):
 *   bits [27:0]  = inst_block_phys >> 12
 *   bits [29:28] = target aperture
 *                  0 = vid_mem (Tegra has none — treat as invalid)
 *                  2 = sys_mem_coherent
 *                  3 = sys_mem_noncoherent
 *
 * Returns the decoded inst-block phys on success, or 0 if:
 *   - /dev/mem can't be opened (helper isn't running as root)
 *   - mmap fails
 *   - The register reads as poison (`0xbadfXXXX`)
 *   - The target aperture is 0 (no current ctx)
 *
 * On failure the function logs a diagnostic line — caller treats 0
 * the same way SLM-OS does (skip publish, fall back to discovery). */
#define GA10B_BAR0_BASE                  0x17000000ull
#define GA10B_GR_FECS_CURRENT_CTX_OFFSET 0x00409b00u
#define GA10B_INST_BLOCK_PAGE_SIZE       4096u

static uint64_t read_fecs_current_ctx_inst_phys(void)
{
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        fprintf(stderr,
                "[gpu-helper] FECS read: open(/dev/mem) failed: %s "
                "(running as root?). inst_block_phys will be 0; "
                "SLM-OS falls back to walk-based discovery.\n",
                strerror(errno));
        return 0;
    }

    /* mmap the page containing FECS_CURRENT_CTX. Page-align the mmap
     * base since mmap() requires it; index within with the byte
     * offset. The FECS register is 4 bytes at BAR0+0x409b00, which
     * sits inside the 4 KB page at BAR0+0x409000. */
    const uint64_t page_size = 4096u;
    uint64_t reg_abs = GA10B_BAR0_BASE + GA10B_GR_FECS_CURRENT_CTX_OFFSET;
    uint64_t page_base = reg_abs & ~(page_size - 1);
    uint32_t page_off  = (uint32_t)(reg_abs & (page_size - 1));

    void *map = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd,
                     (off_t)page_base);
    close(fd);
    if (map == MAP_FAILED) {
        fprintf(stderr,
                "[gpu-helper] FECS read: mmap @0x%llx failed: %s\n",
                (unsigned long long)page_base, strerror(errno));
        return 0;
    }

    volatile uint32_t *reg_ptr =
        (volatile uint32_t *)((uint8_t *)map + page_off);
    uint32_t reg = *reg_ptr;
    munmap(map, page_size);

    if ((reg & 0xFFFF0000u) == 0xbadf0000u) {
        fprintf(stderr,
                "[gpu-helper] FECS read: register=0x%08x (priv-bad "
                "poison — GPU power-gated?). Publishing inst_block_phys=0.\n",
                reg);
        return 0;
    }

    uint32_t target = (reg >> 28) & 0x3u;
    if (target == 0) {
        fprintf(stderr,
                "[gpu-helper] FECS read: register=0x%08x (target=0 — no "
                "current ctx). Publishing inst_block_phys=0.\n",
                reg);
        return 0;
    }

    uint64_t inst_phys = ((uint64_t)(reg & 0x0FFFFFFFu)) << 12;
    printf("[gpu-helper] FECS_CURRENT_CTX=0x%08x → inst_block_phys=0x%llx "
           "(target=%u, %s). NOTE: this is whatever channel was GR-current "
           "at the read moment — may or may not be this helper's channel; "
           "SLM-OS validates via pushbuf-PA walk before use.\n",
           reg, (unsigned long long)inst_phys, target,
           target == 3 ? "sys_mem_noncoherent" :
           target == 2 ? "sys_mem_coherent" : "unknown");
    return inst_phys;
}

/* SIGTERM handler — on clean shutdown, let the kernel reap us (which
 * releases all nvgpu fds and frees the channel). No explicit cleanup
 * needed because nvgpu's release paths run on fd close. */
static volatile sig_atomic_t g_shutdown;
static void on_term(int sig) { (void)sig; g_shutdown = 1; }

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);  /* unbuffered output for kexec debugging */

    /* Argument parsing.
     *
     * The first three flags are honored:
     *   --timeout-secs N  : how long to keep the channel alive
     *   --qmd-pool        : allocate a QMD pool + emit a v7 handoff so
     *                       SLM-OS's per-dispatch GPU path
     *                       (`slm_oplib_dispatch`) can pick a slot
     *                       round-robin. Default pool is 1024 slots
     *                       (256 KiB).
     *   --qmd-pool-slots N: explicit slot count (implies --qmd-pool).
     *
     * The remaining flags exist purely so this binary is drop-in
     * compatible with the slmos-kexec auto-launcher (`start_one_helper`
     * in scripts/jetson-kexec-slmos.sh). That launcher always passes
     * --preserve-for-kexec, --weights-dir, --shader-dir, and may pass
     * --gemm-tier / --weights-fp16-dir. Channel-only operation has
     * nothing useful to do with weights or shaders, so we accept and
     * ignore them. */
    int timeout_secs = DEFAULT_TIMEOUT_SECS;
    int qmd_pool_slots = 0;
    /* W1 weights pool size in bytes. Zero = no pool (v7 handoff,
     * existing behavior); non-zero = allocate the pool, map into the
     * channel's GMMU, populate the v8 handoff fields. SLM-OS's
     * `slm load` then stages weight tensors into the pool so the
     * forward.rs hybrid path can dispatch against GPU-resident
     * weights. The W1 PoC (PR #766) confirmed nvmap can grant
     * 1.5 GB single-buffer; larger sizes haven't been probed
     * (the design target is 1.5 GB for Qwen2.5-1.5B). */
    uint64_t weights_pool_size = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--timeout-secs") == 0 && i + 1 < argc) {
            timeout_secs = atoi(argv[++i]);
            if (timeout_secs <= 0) timeout_secs = DEFAULT_TIMEOUT_SECS;
        } else if (strcmp(argv[i], "--qmd-pool") == 0) {
            if (qmd_pool_slots == 0) qmd_pool_slots = 1024;
        } else if (strcmp(argv[i], "--qmd-pool-slots") == 0 &&
                   i + 1 < argc) {
            qmd_pool_slots = atoi(argv[++i]);
            if (qmd_pool_slots <= 0) qmd_pool_slots = 1024;
        } else if (strcmp(argv[i], "--weights-pool-size") == 0 &&
                   i + 1 < argc) {
            /* Accept human-friendly suffixes: bare bytes, "K", "M",
             * "G". 1.5 GB → "1610612736" or "1536M" or "1536MB". */
            const char *arg = argv[++i];
            char *end = NULL;
            unsigned long long v = strtoull(arg, &end, 0);
            if (end && *end != '\0') {
                if (*end == 'K' || *end == 'k') v *= 1024ULL;
                else if (*end == 'M' || *end == 'm') v *= 1024ULL * 1024;
                else if (*end == 'G' || *end == 'g') v *= 1024ULL * 1024 * 1024;
                else {
                    fprintf(stderr,
                            "[gpu-helper] --weights-pool-size: bad "
                            "suffix '%c' (use bare bytes or K/M/G)\n",
                            *end);
                    return 1;
                }
            }
            weights_pool_size = (uint64_t)v;
            /* nvmap rounds up to 4 KB; reject obviously-wrong inputs
             * before we burn an ioctl. */
            if (weights_pool_size != 0 && weights_pool_size < 4096) {
                fprintf(stderr,
                        "[gpu-helper] --weights-pool-size %llu too "
                        "small (must be 0 or >= 4096)\n",
                        (unsigned long long)weights_pool_size);
                return 1;
            }
        } else if (strcmp(argv[i], "--preserve-for-kexec") == 0) {
            /* No-op: this helper always preserves the channel by
             * holding fds open through the sleep loop. */
        } else if ((strcmp(argv[i], "--weights-dir") == 0 ||
                    strcmp(argv[i], "--shader-dir") == 0 ||
                    strcmp(argv[i], "--weights-fp16-dir") == 0 ||
                    strcmp(argv[i], "--gemm-tier") == 0) &&
                   i + 1 < argc) {
            i++;
        }
    }

    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    printf("[gpu-helper] Starting channel creation (timeout=%ds)...\n",
           timeout_secs);

    /* Open nvmap for buffer allocation. */
    int nvmap_fd = xopen("/dev/nvmap", O_RDWR);

    /* Open GPU ctrl device. Try new path first, fall back to legacy. */
    int ctrl_fd = open("/dev/nvgpu/igpu0/ctrl", O_RDWR);
    if (ctrl_fd < 0)
        ctrl_fd = xopen("/dev/nvhost-ctrl-gpu", O_RDWR);
    printf("[gpu-helper] ctrl_fd=%d\n", ctrl_fd);

    /* Allocate address space. va_range_start/end must be non-zero —
     * captured from CUDA's invocation on L4T r36.4.7 via LD_PRELOAD
     * ioctl snoop. The validation requires va_end > va_start. */
    struct nvgpu_alloc_as_args as_args;
    memset(&as_args, 0, sizeof(as_args));
    as_args.big_page_size = 0;             /* let kernel pick default */
    as_args.flags = 0;
    as_args.va_range_start = 0x4000000ULL; /* skip the low 64 MB */
    as_args.va_range_end = 0x2000000000ULL; /* 128 GB VA range */
    as_args.va_range_split = 0;
    xioctl(ctrl_fd, NVGPU_GPU_IOCTL_ALLOC_AS, &as_args, "ALLOC_AS");
    int as_fd = as_args.as_fd;
    printf("[gpu-helper] AS fd=%d\n", as_fd);

    /* Open TSG. */
    struct nvgpu_gpu_open_tsg_args tsg_args;
    memset(&tsg_args, 0, sizeof(tsg_args));
    xioctl(ctrl_fd, NVGPU_GPU_IOCTL_OPEN_TSG, &tsg_args, "OPEN_TSG");
    int tsg_fd = tsg_args.tsg_fd;
    printf("[gpu-helper] TSG fd=%d\n", tsg_fd);

    /* Open channel. runlist_id is in the .in sub-struct. */
    struct nvgpu_gpu_open_channel_args ch_args;
    memset(&ch_args, 0, sizeof(ch_args));
    ch_args.in.runlist_id = -1;  /* default: GR runlist */
    xioctl(ctrl_fd, NVGPU_GPU_IOCTL_OPEN_CHANNEL, &ch_args,
           "OPEN_CHANNEL");
    int ch_fd = ch_args.out.channel_fd;
    printf("[gpu-helper] Channel fd=%d\n", ch_fd);

    /* Bind channel to AS. */
    struct nvgpu_as_bind_channel_args bind_as = { .channel_fd = ch_fd };
    xioctl(as_fd, NVGPU_AS_IOCTL_BIND_CHANNEL, &bind_as, "AS_BIND");

    /* Create an async subcontext on the TSG and bind the channel to it
     * via BIND_CHANNEL_EX. CUDA's trace shows this path instead of the
     * simpler BIND_CHANNEL (op 1). Plain BIND_CHANNEL puts the channel
     * on the TSG's default SYNC subcontext — compute channels need
     * ASYNC, otherwise the GR engine accepts pushbuffers but silently
     * no-ops the methods.
     *
     * **Kernel requirement:** `CREATE_SUBCONTEXT` (TSG ioctl op 18)
     * and `BIND_CHANNEL_EX` (op 11) require L4T r36 or newer. On
     * older kernels these ops don't exist and xioctl will abort
     * the helper. The cached UAPI headers under ~/slmos-ref/ are
     * from L4T r36.4.7 (the Jetson Orin Nano dev kit default). */
    struct nvgpu_tsg_create_subcontext_args subctx;
    memset(&subctx, 0, sizeof(subctx));
    subctx.type  = NVGPU_TSG_SUBCONTEXT_TYPE_ASYNC;
    subctx.as_fd = as_fd;
    xioctl(tsg_fd, NVGPU_TSG_IOCTL_CREATE_SUBCONTEXT, &subctx,
           "TSG_CREATE_SUBCONTEXT");
    printf("[gpu-helper] CREATE_SUBCONTEXT OK (type=ASYNC, veid=%u)\n",
           subctx.veid);

    struct nvgpu_tsg_bind_channel_ex_args bce;
    memset(&bce, 0, sizeof(bce));
    bce.channel_fd    = ch_fd;
    bce.subcontext_id = subctx.veid;
    xioctl(tsg_fd, NVGPU_TSG_IOCTL_BIND_CHANNEL_EX, &bce,
           "TSG_BIND_CHANNEL_EX");
    printf("[gpu-helper] BIND_CHANNEL_EX OK (veid=%u)\n", bce.subcontext_id);

    /* Set nvmap fd on channel. */
    struct nvgpu_set_nvmap_fd_args nvm = { .fd = nvmap_fd };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_SET_NVMAP_FD, &nvm, "SET_NVMAP");

    /* Match CUDA's WDT settings: DISABLE | SET_TIMEOUT with
     * timeout_ms = UINT_MAX. Required for DETERMINISTIC channels
     * (nvgpu rejects the combination otherwise). */
    struct nvgpu_channel_wdt_args wdt = {
        .wdt_status = NVGPU_IOCTL_CHANNEL_DISABLE_WDT |
                      NVGPU_IOCTL_CHANNEL_WDT_FLAG_SET_TIMEOUT,
        .timeout_ms = 0xFFFFFFFFu,
    };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_WDT, &wdt, "WDT_DISABLE");

    /* Allocate buffers via nvmap: USERD (4KB), GPFIFO (one 4KB page,
     * 512 entries — see GPU_LAUNCH_GPFIFO_* in gpu-launch-common.h
     * for why a multi-page GPFIFO breaks SLM-OS's post-kexec direct-
     * physical writes; #601). This helper writes channel handoffs
     * that SLM-OS inherits, so its sizing MUST match
     * gpu-launch-common.c. */
    int userd_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);
    int gpfifo_dmabuf = nvmap_alloc_dmabuf(nvmap_fd,
                                           GPU_LAUNCH_GPFIFO_BYTES, 4096);
    printf("[gpu-helper] USERD dmabuf=%d, GPFIFO dmabuf=%d\n",
           userd_dmabuf, gpfifo_dmabuf);

    /* SETUP_BIND — creates GPFIFO ring + binds channel.
     * flags=0xa (DETERMINISTIC | USERMODE_SUPPORT) captured from CUDA
     * via LD_PRELOAD on L4T r36.4.7. */
    struct nvgpu_channel_setup_bind_args sb;
    memset(&sb, 0, sizeof(sb));
    sb.num_gpfifo_entries = GPU_LAUNCH_GPFIFO_ENTRIES;
    sb.num_inflight_jobs = 0;
    sb.flags = NVGPU_CHANNEL_SETUP_BIND_FLAGS_DETERMINISTIC |
               NVGPU_CHANNEL_SETUP_BIND_FLAGS_USERMODE_SUPPORT;
    sb.userd_dmabuf_fd = userd_dmabuf;
    sb.gpfifo_dmabuf_fd = gpfifo_dmabuf;
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_SETUP_BIND, &sb, "SETUP_BIND");
    printf("[gpu-helper] SETUP_BIND OK:\n");
    printf("  work_submit_token = 0x%x\n", sb.work_submit_token);
    printf("  gpfifo_gpu_va     = 0x%llx\n",
           (unsigned long long)sb.gpfifo_gpu_va);
    printf("  userd_gpu_va      = 0x%llx\n",
           (unsigned long long)sb.userd_gpu_va);
    printf("  usermode_mmio_va  = 0x%llx\n",
           (unsigned long long)sb.usermode_mmio_gpu_va);

    /* Bind the compute class to the channel. The class number is
     * chip-specific: CUDA on GA10B (Jetson Orin's iGPU) uses
     * AMPERE_COMPUTE_B (0xC7C0), NOT AMPERE_COMPUTE_A (0xC5C0 — the
     * datacenter GA100 class). Captured via LD_PRELOAD ioctl trace of
     * CUDA's minikick. Using the wrong class causes PBDMA to walk
     * pushbuffer entries but host methods to silently no-op.
     *
     * class_num per GPU family (from NVIDIA open-gpu-kernel-modules):
     *   Pascal (GP10x)   = 0xC1C0
     *   Volta  (GV11B)   = 0xC3C0
     *   Turing (TU10x)   = 0xC5C0  (also GA100 datacenter)
     *   Ampere GA10x/10B = 0xC7C0  */
    struct nvgpu_alloc_obj_ctx_args octx;
    memset(&octx, 0, sizeof(octx));
    octx.class_num = 0xC7C0;   /* AMPERE_COMPUTE_B (GA10B) */
    octx.flags = 0;
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_ALLOC_OBJ_CTX, &octx, "ALLOC_OBJ_CTX");
    printf("[gpu-helper] ALLOC_OBJ_CTX OK (class=0x%04x, obj_id=0x%llx)\n",
           octx.class_num, (unsigned long long)octx.obj_id);

    /* Set compute-channel preemption mode. CUDA's trace shows
     * compute_preempt_mode = CILP (0x04) right after ALLOC_OBJ_CTX.
     * Without this, the compute context appears to not be fully
     * activated: PBDMA walks pushbuffer entries (GP_GET advances)
     * but host-semaphore methods silently no-op. */
    struct nvgpu_preemption_mode_args pm;
    memset(&pm, 0, sizeof(pm));
    pm.graphics_preempt_mode = 0;
    pm.compute_preempt_mode  = NVGPU_COMPUTE_PREEMPTION_MODE_CILP;
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_SET_PREEMPTION_MODE, &pm,
           "SET_PREEMPT_MODE");
    printf("[gpu-helper] SET_PREEMPT_MODE OK (compute=CILP)\n");

    /* Attach an error notifier buffer. CUDA passes a 4KB nvmap dmabuf
     * here — the kernel writes fault info into it on channel errors.
     * A channel without a valid notifier may silently drop methods
     * (rather than faulting) because it has nowhere to report the
     * error. */
    int notifier_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);
    struct nvgpu_set_error_notifier en;
    memset(&en, 0, sizeof(en));
    en.offset = 0;
    en.size   = 4096;
    en.mem    = notifier_dmabuf;
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_SET_ERROR_NOTIFIER, &en,
           "SET_ERROR_NOTIFIER");
    printf("[gpu-helper] SET_ERROR_NOTIFIER OK (dmabuf fd=%d, size=4096)\n",
           notifier_dmabuf);

    /* Allocate pushbuffer + semaphore via nvmap. */
    int pb_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 65536, 4096);  /* 64 KB */
    int sem_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);

    /* Register each dmabuf with the GPU subsystem. CUDA calls this
     * NVGPU_GPU_IOCTL_REGISTER_BUFFER (op 41) 170 times per channel —
     * once per buffer it allocates. nvgpu associates caller-provided
     * metadata with the dmabuf fd; without registration, methods that
     * reference the buffer's VA may silently no-op. We pass empty
     * metadata (the content is opaque nvrm_gpu-private data and not
     * required for correctness — just tracking). */
    struct nvgpu_gpu_register_buffer_args regbuf;
    int reg_fds[] = { pb_dmabuf, sem_dmabuf };
    const char *reg_names[] = { "PB", "SEM" };
    for (size_t i = 0; i < sizeof(reg_fds)/sizeof(reg_fds[0]); i++) {
        memset(&regbuf, 0, sizeof(regbuf));
        regbuf.dmabuf_fd = reg_fds[i];
        regbuf.comptags_alloc_control = NVGPU_GPU_COMPTAGS_ALLOC_NONE;
        regbuf.metadata_addr = 0;
        regbuf.metadata_size = 0;
        regbuf.flags = 0;
        if (ioctl(ctrl_fd, NVGPU_GPU_IOCTL_REGISTER_BUFFER, &regbuf) < 0) {
            fprintf(stderr, "[gpu-helper] REGISTER_BUFFER(%s,fd=%d) failed: "
                    "%s (errno=%d)\n",
                    reg_names[i], reg_fds[i], strerror(errno), errno);
        } else {
            printf("[gpu-helper] REGISTER_BUFFER %s (fd=%d) OK flags=0x%x\n",
                   reg_names[i], reg_fds[i], regbuf.flags);
        }
    }

    /* Map pushbuffer into GPU AS. compr_kind=-1 (invalid), incompr_kind=0
     * means "no compression, use default PTE kind". Per nvgpu docs,
     * at least one of compr_kind/incompr_kind must be != NV_KIND_INVALID. */
    struct nvgpu_as_map_buffer_ex_args pb_map;
    memset(&pb_map, 0, sizeof(pb_map));
    pb_map.compr_kind = -1;       /* NV_KIND_INVALID */
    pb_map.incompr_kind = 0;      /* generic PITCH kind */
    pb_map.dmabuf_fd = pb_dmabuf;
    /* mapping_size=0 when not FIXED_OFFSET (kernel uses dmabuf size). */
    pb_map.mapping_size = 0;
    pb_map.page_size = 4096;
    xioctl(as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &pb_map, "MAP_PB");
    printf("[gpu-helper] Pushbuffer GPU VA = 0x%llx\n",
           (unsigned long long)pb_map.offset);

    /* Map semaphore into GPU AS. */
    struct nvgpu_as_map_buffer_ex_args sem_map;
    memset(&sem_map, 0, sizeof(sem_map));
    sem_map.compr_kind = -1;
    sem_map.incompr_kind = 0;
    sem_map.dmabuf_fd = sem_dmabuf;
    sem_map.mapping_size = 0;
    sem_map.page_size = 4096;
    xioctl(as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &sem_map, "MAP_SEM");
    printf("[gpu-helper] Semaphore GPU VA = 0x%llx\n",
           (unsigned long long)sem_map.offset);

    /* Enable TSG — makes the channel schedulable. */
    xioctl(tsg_fd, NVGPU_IOCTL_TSG_ENABLE, NULL, "TSG_ENABLE");
    printf("[gpu-helper] TSG enabled\n");

    /* mmap the USERD, GPFIFO, pushbuffer, and semaphore to get CPU VAs.
     * Then translate to physical addresses via /proc/self/pagemap. */
    void *userd_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                          MAP_SHARED, userd_dmabuf, 0);
    void *gpfifo_va = mmap(NULL, GPU_LAUNCH_GPFIFO_BYTES,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED, gpfifo_dmabuf, 0);
    void *pb_va = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                       MAP_SHARED, pb_dmabuf, 0);
    void *sem_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                        MAP_SHARED, sem_dmabuf, 0);

    if (userd_va == MAP_FAILED || gpfifo_va == MAP_FAILED ||
        pb_va == MAP_FAILED || sem_va == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* Touch pages to ensure they're faulted in before pagemap lookup. */
    memset(userd_va, 0, 4096);
    memset(gpfifo_va, 0, GPU_LAUNCH_GPFIFO_BYTES);
    memset(pb_va, 0, 65536);
    memset(sem_va, 0, 4096);

    uint64_t userd_phys = virt_to_phys(userd_va);
    uint64_t gpfifo_phys = virt_to_phys(gpfifo_va);
    uint64_t pb_phys = virt_to_phys(pb_va);
    uint64_t sem_phys = virt_to_phys(sem_va);

    printf("[gpu-helper] Physical addresses:\n");
    printf("  USERD:  0x%llx\n", (unsigned long long)userd_phys);
    printf("  GPFIFO: 0x%llx\n", (unsigned long long)gpfifo_phys);
    printf("  PB:     0x%llx\n", (unsigned long long)pb_phys);
    printf("  SEM:    0x%llx\n", (unsigned long long)sem_phys);

    if (userd_phys == 0 || gpfifo_phys == 0 ||
        pb_phys == 0 || sem_phys == 0) {
        fprintf(stderr, "[gpu-helper] Failed to resolve physical addresses\n");
        return 1;
    }

    /* Optional QMD pool for v7 handoff. SLM-OS's per-dispatch GPU
     * path (`slm_oplib_dispatch`, kernel/gpu/oplib_dispatch.c) needs
     * `qmd_pool_gpu_va != 0` in the inherited handoff before it will
     * fire any kernel — without a pool, the dispatcher refuses
     * gracefully ("qmd_pool_gpu_va == 0 — channel handoff missing
     * v7 pool resources"). The pool is sized in 256 B slots; SLM-OS
     * rotates through them round-robin so consecutive dispatches
     * land at distinct GPU VAs (avoids SKED's QMD decode cache
     * serving the prior launch's output, the v6 staleness bug).
     *
     * Allocated inline (mirrors the pb/sem pattern above) rather
     * than via gpu-launch-common.c's `gpu_alloc_qmd_pool` because
     * this helper builds standalone — no link step against
     * gpu-launch-common.c. The header is included for the handoff
     * struct definition only. */
    int      qmd_pool_dmabuf = -1;
    void    *qmd_pool_va     = NULL;
    uint64_t qmd_pool_phys   = 0;
    uint64_t qmd_pool_gva    = 0;
    uint32_t qmd_pool_size_bytes = 0;
    if (qmd_pool_slots > 0) {
        uint64_t bytes = (uint64_t)qmd_pool_slots * 256ull;
        if (bytes > 0xFFFFFFFFull) {
            fprintf(stderr,
                    "[gpu-helper] QMD pool size %llu B overflows uint32_t\n",
                    (unsigned long long)bytes);
            return 1;
        }
        /* Page-round once here so `qmd_pool_size_bytes` matches the
         * actual allocation that goes into the handoff. The helper
         * page-rounds internally too — passing an already-rounded
         * value makes the round a no-op there. */
        qmd_pool_size_bytes = ((uint32_t)bytes + 4095u) & ~4095u;
        if (alloc_channel_buffer(nvmap_fd, ctrl_fd, as_fd,
                                 qmd_pool_size_bytes, "QMD_POOL",
                                 /*zero_fill=*/true,
                                 /*phys_required=*/true,
                                 &qmd_pool_dmabuf, &qmd_pool_va,
                                 &qmd_pool_phys, &qmd_pool_gva) < 0) {
            return 1;
        }
        printf("[gpu-helper] QMD pool: %u slots, %u B, "
               "phys=0x%llx, gpu_va=0x%llx\n",
               (unsigned)(qmd_pool_size_bytes / 256u),
               qmd_pool_size_bytes,
               (unsigned long long)qmd_pool_phys,
               (unsigned long long)qmd_pool_gva);
    }

    /* Pre-stage SASS region + cbuf in the channel's GMMU when v7
     * mode is requested. SLM-OS's `slm_oplib_dispatch` needs both
     * to be pre-mapped in the inherited channel — without them it
     * would have to call `ga10b_gmmu_alloc` post-kexec, which
     * requires discovering the channel's inst_block_phys (FECS_-
     * CURRENT_CTX is unreliable, walk-discovery requires big-page
     * walker support — both are bigger investments than just
     * pre-staging here).
     *
     * SASS region (`shader_*` fields in the v7 handoff): empty
     * GMMU-mapped buffer big enough for the embedded operator
     * library's SASS pool. SLM-OS reads `shader_phys` from the
     * handoff and writes the SASS bytes into it post-kexec via the
     * identity-mapped phys, then sets g_sass_pool_gpu_va = the
     * helper's `shader_gpu_va`.
     *
     * 1 MB is enough for the current 13-entry library (largest
     * single SASS is ~16 KB; 1 MB leaves room for additions).
     *
     * cbuf (`cbuf_*` fields): single 4 KB page for runtime kernel
     * args. Reused serially across dispatches — the dispatcher
     * zeros it, populates from `args`, and submits in a one-at-
     * a-time pattern. Concurrent dispatches would race on these
     * bytes, but slm_oplib_dispatch is synchronous (it polls the
     * trailing semaphore before returning), so serial reuse is
     * correct. */
    int      sass_dmabuf = -1;
    void    *sass_va     = NULL;
    uint64_t sass_phys   = 0;
    uint64_t sass_gva    = 0;
    uint32_t sass_size_bytes = 0;
    int      cbuf_dmabuf = -1;
    void    *cbuf_va     = NULL;
    uint64_t cbuf_phys   = 0;
    uint64_t cbuf_gva    = 0;
    if (qmd_pool_slots > 0) {
        sass_size_bytes = 1u << 20;  /* 1 MB */
        if (alloc_channel_buffer(nvmap_fd, ctrl_fd, as_fd,
                                 sass_size_bytes, "SASS_POOL",
                                 /*zero_fill=*/true,
                                 /*phys_required=*/true,
                                 &sass_dmabuf, &sass_va,
                                 &sass_phys, &sass_gva) < 0) {
            return 1;
        }
        printf("[gpu-helper] SASS pool: %u B, phys=0x%llx, gpu_va=0x%llx\n",
               sass_size_bytes,
               (unsigned long long)sass_phys,
               (unsigned long long)sass_gva);

        if (alloc_channel_buffer(nvmap_fd, ctrl_fd, as_fd,
                                 4096u, "CBUF",
                                 /*zero_fill=*/true,
                                 /*phys_required=*/true,
                                 &cbuf_dmabuf, &cbuf_va,
                                 &cbuf_phys, &cbuf_gva) < 0) {
            return 1;
        }
        printf("[gpu-helper] cbuf: 4096 B, phys=0x%llx, gpu_va=0x%llx\n",
               (unsigned long long)cbuf_phys,
               (unsigned long long)cbuf_gva);
    }

    /* W1 weights pool. Allocated only when --weights-pool-size is
     * non-zero. Mapped into the same channel address space as the
     * SASS pool / pushbuffer so SLM-OS's `slm load` can stage
     * weight tensors and forward.rs hybrid wrappers can dispatch
     * against GPU-resident weights. zero_fill=false because the
     * pool is potentially GB-scale and SLM-OS will overwrite every
     * byte with weight data anyway; zeroing 1.5 GB would burn
     * ~50 ms of pre-kexec time pointlessly. See
     * docs/design/gpu-weights-pool.md. */
    int      weights_pool_dmabuf = -1;
    void    *weights_pool_va     = NULL;
    uint64_t weights_pool_phys   = 0;
    uint64_t weights_pool_gva    = 0;
    if (weights_pool_size > 0) {
        if (alloc_channel_buffer(nvmap_fd, ctrl_fd, as_fd,
                                 weights_pool_size, "WEIGHTS_POOL",
                                 /*zero_fill=*/false,
                                 /*phys_required=*/false,
                                 &weights_pool_dmabuf, &weights_pool_va,
                                 &weights_pool_phys, &weights_pool_gva) < 0) {
            return 1;
        }
        printf("[gpu-helper] Weights pool: %llu B (%.2f GB), "
               "phys=0x%llx, gpu_va=0x%llx\n",
               (unsigned long long)weights_pool_size,
               weights_pool_size / (1024.0 * 1024.0 * 1024.0),
               (unsigned long long)weights_pool_phys,
               (unsigned long long)weights_pool_gva);
    }

    /* #788 Stage 4: walk the weights pool's pages via
     * /proc/self/pagemap, coalesce consecutive physical pages
     * into extents, and publish the list in a separate dmabuf
     * so SLM-OS can re-establish GMMU PTEs covering the entire
     * pool post-kexec.
     *
     * The 1.5 GB IOVMM-stitched pool is physically scattered
     * (CMA backing returns hundreds-to-thousands of separate
     * contiguous physical runs depending on fragmentation).
     * `weights_pool_phys` records only the first page's phys;
     * the rest is invisible without the explicit per-page walk
     * this loop performs.
     *
     * Cap matches `GA10B_WEIGHTS_EXTENTS_MAX` in the shared
     * handoff header (8192 entries × 16 bytes = 128 KB dmabuf).
     * If a future allocation fragments more than that, we
     * truncate and warn; the operator's fix is to bump the cap
     * on both sides in lock-step. */
    int      extents_dmabuf = -1;
    void    *extents_va     = NULL;
    uint64_t extents_phys   = 0;
    uint32_t n_extents      = 0;
    const uint32_t MAX_EXTENTS = 8192u;  /* matches GA10B_WEIGHTS_EXTENTS_MAX */
    const size_t   extents_bytes_total =
        (size_t)MAX_EXTENTS * sizeof(struct ga10b_phys_extent);
    if (weights_pool_size > 0 && weights_pool_va != NULL) {
        extents_dmabuf = nvmap_alloc_dmabuf(nvmap_fd,
                                            (uint64_t)extents_bytes_total,
                                            4096);
        extents_va = mmap(NULL, extents_bytes_total,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED, extents_dmabuf, 0);
        if (extents_va == MAP_FAILED) {
            perror("[gpu-helper] mmap extents dmabuf");
            extents_va = NULL;
        } else {
            memset(extents_va, 0, extents_bytes_total);
            msync(extents_va, extents_bytes_total, MS_SYNC);
            extents_phys = virt_to_phys(extents_va);
            if (extents_phys == 0) {
                fprintf(stderr, "[gpu-helper] extents dmabuf phys "
                        "resolution failed — extents disabled\n");
                extents_va = NULL;
            }
        }
    }

    if (extents_va != NULL) {
        /* First-touch the weights pool to force every page to
         * fault in. NVMAP's IOVMM-backed dmabuf creates a userspace
         * mmap that's lazy — pages aren't bound to physical backing
         * until accessed. Without touching them first,
         * /proc/self/pagemap reports "page not present" for the
         * entire 1.5 GB and the extent walk finds nothing.
         *
         * A single byte read per page is enough to fault it in.
         * 393,216 pages × ~ns-scale read ≈ ~1-2 s on Tegra Orin
         * Nano — a one-time pre-kexec cost; nothing on the hot
         * path. */
        {
            volatile uint8_t *touch = (volatile uint8_t *)weights_pool_va;
            for (size_t pi = 0;
                 pi < weights_pool_size / 4096u;
                 pi++) {
                (void)touch[pi * 4096u];
            }
        }

        /* Walk every 4 KB page in the weights pool, resolving
         * its physical address via /proc/self/pagemap. Coalesce
         * consecutive pages whose physes match `prev_phys +
         * 4 KB` into one extent entry, and flush the current
         * extent when a discontinuity appears or we reach the
         * pool's end. */
        struct ga10b_phys_extent *extents = extents_va;
        size_t   total_pages  = weights_pool_size / 4096u;
        uint64_t cur_run_phys = 0;
        uint32_t cur_run_pages = 0;
        size_t   pagemap_failures = 0;
        uint8_t *base = (uint8_t *)weights_pool_va;

        for (size_t pi = 0; pi < total_pages; pi++) {
            uint64_t va = (uint64_t)(uintptr_t)base + pi * 4096ull;
            uint64_t phys = virt_to_phys((void *)(uintptr_t)va);
            if (phys == 0) {
                pagemap_failures++;
                /* Flush current run; skip this page. SLM-OS
                 * will leave the corresponding 4 KB hole in
                 * the GMMU and a CE memcpy that lands there
                 * will MMU-fault. */
                if (cur_run_pages > 0 && n_extents < MAX_EXTENTS) {
                    extents[n_extents].phys = cur_run_phys;
                    extents[n_extents].n_pages = cur_run_pages;
                    extents[n_extents].reserved = 0;
                    n_extents++;
                    cur_run_phys = 0;
                    cur_run_pages = 0;
                }
                continue;
            }
            if (cur_run_pages == 0) {
                cur_run_phys = phys;
                cur_run_pages = 1;
            } else if (phys ==
                       cur_run_phys + (uint64_t)cur_run_pages * 4096ull) {
                cur_run_pages++;
            } else {
                /* Discontinuity — flush. */
                if (n_extents < MAX_EXTENTS) {
                    extents[n_extents].phys = cur_run_phys;
                    extents[n_extents].n_pages = cur_run_pages;
                    extents[n_extents].reserved = 0;
                    n_extents++;
                } else {
                    /* Table full — break out, will warn below. */
                    break;
                }
                cur_run_phys = phys;
                cur_run_pages = 1;
            }
        }
        /* Flush final run. */
        if (cur_run_pages > 0 && n_extents < MAX_EXTENTS) {
            extents[n_extents].phys = cur_run_phys;
            extents[n_extents].n_pages = cur_run_pages;
            extents[n_extents].reserved = 0;
            n_extents++;
        }
        msync(extents_va, extents_bytes_total, MS_SYNC);

        printf("[gpu-helper] Weights extents: %u runs (%zu pages "
               "total) → phys=0x%llx",
               (unsigned)n_extents, total_pages,
               (unsigned long long)extents_phys);
        if (n_extents == MAX_EXTENTS) {
            printf(" [TRUNCATED — bump MAX_EXTENTS]");
        }
        if (pagemap_failures > 0) {
            printf(" [%zu pagemap failures]", pagemap_failures);
        }
        printf("\n");
    }

    /* Allocate a dedicated dmabuf for the handoff block. Writing via
     * /dev/mem is blocked by CONFIG_STRICT_DEVMEM for System RAM, but
     * we can write to dmabuf memory via its own mmap. SLM-OS scans
     * a range of physical memory for the magic value to find this. */
    int handoff_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);
    void *handoff = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_SHARED, handoff_dmabuf, 0);
    if (handoff == MAP_FAILED) {
        perror("mmap handoff dmabuf");
        return 1;
    }
    memset(handoff, 0, 4096);
    uint64_t handoff_phys = virt_to_phys(handoff);
    printf("[gpu-helper] Handoff dmabuf phys = 0x%llx\n",
           (unsigned long long)handoff_phys);

    /* Populate the handoff block using the shared struct definition.
     * Using named fields (not hand-indexed words) means SLM-OS and
     * this helper can never drift out of sync silently — any struct
     * reorder is a compile error on rebuild. */
    /* Handoff version selection:
     *   v8 — weights pool allocated (W1, this commit)
     *   v7 — QMD pool allocated (no weights pool)
     *   v2 — channel-only (no v7 pool, no weights pool)
     * Each higher version is a strict superset: v9 readers can
     * consume v8 handoffs by ignoring the trailing weights_extents_*
     * fields, v8 readers ignore v9, etc. */
    uint32_t handoff_version = (n_extents > 0)         ? 9u
                              : (weights_pool_size > 0) ? 8u
                              : (qmd_pool_slots > 0)   ? 7u
                              :                          2u;
    struct ga10b_channel_handoff hoff = {
        .magic              = GA10B_CHANNEL_HANDOFF_MAGIC,
        .version            = handoff_version,
        .channel_id         = 0,  /* nvgpu doesn't expose this cheaply;
                                   * work_submit_token below is the
                                   * authoritative field for the
                                   * doorbell write. */
        .tsg_id             = 0,
        .userd_phys         = userd_phys,
        /* ram_userd_gp_put_w = 35, ram_userd_gp_get_w = 34 from
         * hw_ram_ga10b.h: USERD entry layout on Ampere. */
        .userd_gp_put_offset = 35 * 4,
        .userd_gp_get_offset = 34 * 4,
        .gpfifo_phys        = gpfifo_phys,
        .gpfifo_gpu_va      = sb.gpfifo_gpu_va,
        .gpfifo_entries     = GPU_LAUNCH_GPFIFO_ENTRIES,
        .gpfifo_entry_size  = GPU_LAUNCH_GPFIFO_ENTRY_BYTES,
        .pushbuf_phys       = pb_phys,
        .pushbuf_gpu_va     = pb_map.offset,
        .pushbuf_size       = 65536,
        .semaphore_phys     = sem_phys,
        .semaphore_gpu_va   = sem_map.offset,
        .inst_block_phys    = 0,  /* populated post-ALLOC by the isolation
                                   * test's FECS_CURRENT_CTX read, AFTER
                                   * the GR engine confirms our channel
                                   * just ran. See `read_fecs_current_ctx_-
                                   * inst_phys` + the publish site below the
                                   * sema-fire branch. The initial-zero
                                   * sentinel here means "skip", picked up
                                   * by SLM-OS's `nvgpu oplib stage` to fall
                                   * back to walk-based discovery. */
        .initial_gp_put     = 0,
        .initial_gp_get     = 0,
        .work_submit_token  = sb.work_submit_token,
        /* v3 dispatch fields, repurposed in v7 mode as the pre-
         * staged SASS region + cbuf for `slm_oplib_dispatch`.
         * Zero in v2 mode (no v7 pre-staging requested). */
        .shader_phys        = sass_phys,
        .shader_gpu_va      = sass_gva,
        .shader_size        = sass_size_bytes,
        .cbuf_phys          = cbuf_phys,
        .cbuf_gpu_va        = cbuf_gva,
        .cbuf_size          = (cbuf_phys != 0) ? 4096u : 0u,
        /* v7-only: per-dispatch QMD pool. Zero in v2 mode. */
        .qmd_pool_phys      = qmd_pool_phys,
        .qmd_pool_gpu_va    = qmd_pool_gva,
        .qmd_pool_size_bytes = qmd_pool_size_bytes,
        .qmd_pool_n_slots   = qmd_pool_size_bytes / 256u,
        /* v8-only: weights pool. Zero in v2/v7 mode. SLM-OS's
         * `slm load` populates the pool's contents post-kexec. */
        .weights_pool_phys       = weights_pool_phys,
        .weights_pool_gpu_va     = weights_pool_gva,
        .weights_pool_size_bytes = weights_pool_size,
        /* v9-only: weights-pool per-page extents. Zero in v2..v8
         * mode (no pagemap walk done). #788 Stage 4. */
        .weights_extents_phys    = extents_phys,
        .weights_n_extents       = n_extents,
    };
    memcpy(handoff, &hoff, sizeof(hoff));
    msync(handoff, 4096, MS_SYNC);
    printf("[gpu-helper] Handoff block written to dmabuf phys 0x%llx\n",
           (unsigned long long)handoff_phys);
    printf("[gpu-helper] Magic at offset 0: 0x%08x\n",
           ((uint32_t *)handoff)[0]);

    /* Prime PBDMA: submit a NOP pushbuffer from userspace by writing
     * GP_PUT in USERD and ringing the doorbell on the channel fd's
     * mmap. This "activates" the channel so PBDMA is polling it when
     * SLM-OS later writes GP_PUT after kexec. */
    /* Isolation test (issue #273): submit a real SEMAPHORE_RELEASE from
     * Linux userspace via mmap+doorbell (same path SLM-OS uses post-
     * kexec). If the semaphore fires Linux-side, the channel is fully
     * capable and Phase 7's issue is post-kexec state loss. If it
     * doesn't, the channel setup itself is still incomplete.
     *
     * Previous helper versions wrote the doorbell at mmap offset 0 —
     * that hits the wrong register. The actual USERMODE doorbell is at
     * BAR0+0xBB0090, which is offset 0x90 *within* the CTRL mmap
     * (the mmap covers BAR0+0xBB0000..+0xBB0FFF). Fixed below. */
    printf("[gpu-helper] === ISOLATION TEST: userspace mmap+doorbell ===\n");

    /* SEMAPHORE_RELEASE via PBDMA-decoded host-family methods.
     *
     * Encoding matches nvgpu's gv11b_sema_add_incr_cmd verbatim
     * (~/slmos-ref/nvidia/nvgpu-hal-sync-sema_cmdbuf_gv11b.c:45-101).
     * Method headers carry method_id = byte_off / 4 at bits [12:0]
     * (NOT byte_off at [11:0]) — the hardware decodes bits [12:0]
     * as method_id, so misplacing the value causes PBDMA to silently
     * discard the method while still advancing GP_GET. Verified
     * sema-fires live on jetson-nano-2 2026-04-18.
     *
     * Host family at byte 0x5C-0x6C is PBDMA-decoded and bypasses
     * GR/MME entirely — no SET_OBJECT needed, no class bind. The
     * COMPUTE_B path via AMPERE_COMPUTE_B (class 0xC7C0) on subch 1
     * is mothballed pending MME program-load support (issue #291);
     * until then, the helper's isolation test uses the host family.
     *
     * **LAYOUT LOCKED BY TESTS:** host-tools/gsp-harness/test_ga10b_bringup.c
     * pins the dword stream of ga10b_build_sema_release_pushbuffer
     * (the kernel-side mirror) against literal values. If this
     * helper diverges from that encoding, the pre-kexec isolation
     * test and SLM-OS's post-kexec submit won't agree. Helper uses
     * hardcoded literals rather than importing the kernel macro
     * because the builder lives in a C file, not a header. */
    uint32_t *pb32 = (uint32_t *)pb_va;
    uint64_t sem_gva = sem_map.offset;
    pb32[0] = 0x20010017u;                 /* SEM_ADDR_LO  (method_id 0x17 = byte 0x5C) */
    pb32[1] = (uint32_t)(sem_gva & 0xFFFFFFFFu);
    pb32[2] = 0x20010018u;                 /* SEM_ADDR_HI  (method_id 0x18 = byte 0x60) */
    pb32[3] = (uint32_t)((sem_gva >> 32) & 0xFFu);
    pb32[4] = 0x20010019u;                 /* SEM_PAYLOAD_LO (method_id 0x19 = byte 0x64) */
    pb32[5] = HELPER_SMOKETEST_SEM_PAYLOAD;
    pb32[6] = 0x2001001au;                 /* SEM_PAYLOAD_HI (method_id 0x1a = byte 0x68) — ignored for 32-bit */
    pb32[7] = 0u;
    pb32[8] = 0x2001001bu;                 /* SEM_EXECUTE  (method_id 0x1b = byte 0x6C) */
    pb32[9] = 0x00000001u;                 /* OPERATION_RELEASE (bit 0) | no wfi */
    msync(pb_va, 4096, MS_SYNC);

    /* Pre-clear the semaphore so we can detect the GPU write. */
    *(volatile uint32_t *)sem_va = 0;
    msync(sem_va, 4096, MS_SYNC);

    /* Build GPFIFO entry — 10 dwords (5 method pairs, no SET_OBJECT). */
    uint64_t pb_gva = pb_map.offset;
    uint32_t gp_e0 = (uint32_t)(pb_gva & 0xFFFFFFFCu);
    uint32_t gp_e1 = (uint32_t)((pb_gva >> 32) & 0xFFu) | (10u << 10);
    ((uint32_t *)gpfifo_va)[0] = gp_e0;
    ((uint32_t *)gpfifo_va)[1] = gp_e1;
    msync(gpfifo_va, 8, MS_SYNC);
    printf("[gpu-helper] GPFIFO[0] = 0x%08x_%08x (pb_va=0x%llx, 10 dwords)\n",
           gp_e1, gp_e0, (unsigned long long)pb_gva);

    /* Advance GP_PUT in USERD (word 35). */
    ((uint32_t *)userd_va)[35] = 1;
    msync(userd_va, 4096, MS_SYNC);

    /* mmap the USERMODE doorbell page from the CTRL fd. The mmap covers
     * the 4KB page starting at BAR0+0xBB0000; the actual doorbell is at
     * offset 0x90 (BAR0+0xBB0090). Writing at offset 0 hits a control
     * register that isn't the doorbell. */
    void *doorbell_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_SHARED, ctrl_fd, 0);
    if (doorbell_page == MAP_FAILED) {
        perror("mmap doorbell page on ctrl fd");
    } else {
        volatile uint32_t *doorbell =
            (volatile uint32_t *)((char *)doorbell_page + 0x90);
        printf("[gpu-helper] Doorbell page %p + 0x90; writing token 0x%x\n",
               doorbell_page, sb.work_submit_token);
        *doorbell = sb.work_submit_token;
        __asm__ volatile("dsb sy" ::: "memory");

        /* Poll semaphore for up to 1s. */
        uint32_t sem_val = 0;
        for (int i = 0; i < 100; i++) {
            sem_val = *(volatile uint32_t *)sem_va;
            if (sem_val == HELPER_SMOKETEST_SEM_PAYLOAD) break;
            usleep(10000);
        }
        uint32_t gp_get_after = ((volatile uint32_t *)userd_va)[34];
        printf("[gpu-helper] After doorbell: GP_GET=%u (want 1), "
               "sem=0x%08x (want 0x%x)\n",
               gp_get_after, sem_val, HELPER_SMOKETEST_SEM_PAYLOAD);
        if (sem_val == HELPER_SMOKETEST_SEM_PAYLOAD) {
            printf("[gpu-helper] >>> ISOLATION: sema fires Linux-side — "
                   "channel capable, kexec breaks state\n");

            /* #788 Stage 9 fix: publish `inst_block_phys` in the
             * handoff so SLM-OS's `nvgpu oplib stage` doesn't have
             * to walk DRAM looking for our inst block.
             *
             * Critically, we do this read RIGHT AFTER the
             * isolation-test sema fires — at that moment the GR
             * engine just ran our pushbuffer, so FECS_CURRENT_CTX
             * is guaranteed to point at OUR channel. If we read
             * any later, GR might context-switch back to Xorg or
             * nvgpu's internal channel. If we read earlier, our
             * channel might not yet have been scheduled.
             *
             * SLM-OS's `oplib stage` still pushbuf-PA validates
             * before trusting the value, so a wrong inst_block_phys
             * here (e.g. if FECS drifts before we read in some
             * future timing-sensitive case) won't propagate as a
             * bad pointer downstream. */
            hoff.inst_block_phys = read_fecs_current_ctx_inst_phys();
            if (hoff.inst_block_phys != 0) {
                printf("[gpu-helper] >>> publishing inst_block_phys=0x%llx "
                       "in handoff (skips SLM-OS's DRAM walk)\n",
                       (unsigned long long)hoff.inst_block_phys);
            }
        } else if (gp_get_after == 1) {
            printf("[gpu-helper] >>> ISOLATION: PBDMA consumed entry but "
                   "method didn't fire — channel setup incomplete\n");
        } else {
            printf("[gpu-helper] >>> ISOLATION: PBDMA didn't even walk "
                   "the entry — channel not scheduled\n");
        }
    }

    /* Re-read + update handoff with the current GP_PUT/GP_GET values
     * AND the FECS-derived inst_block_phys captured above. */
    hoff.initial_gp_put = ((volatile uint32_t *)userd_va)[35];
    hoff.initial_gp_get = ((volatile uint32_t *)userd_va)[34];
    memcpy(handoff, &hoff, sizeof(hoff));
    msync(handoff, 4096, MS_SYNC);

    printf("[gpu-helper] Channel ready. Sleeping up to %d s — "
           "run kexec now.\n", timeout_secs);
    printf("[gpu-helper] To kexec: sudo slmos-kexec --no-gpu-suspend\n");

    /* Sleep bounded — keep fds open (channel stays alive) while
     * waiting for kexec. SIGTERM/SIGINT or timeout both cleanly exit,
     * releasing the channel. */
    for (int remaining = timeout_secs; remaining > 0 && !g_shutdown; ) {
        int slice = remaining > 60 ? 60 : remaining;
        sleep(slice);
        remaining -= slice;
    }
    printf("[gpu-helper] Exiting (%s) — channel will be released.\n",
           g_shutdown ? "signal" : "timeout");
    return 0;
}
