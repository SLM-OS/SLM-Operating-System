/*
 * gpu-channel-helper.c — Create an nvgpu channel on Jetson and write
 * the handoff metadata for SLM-OS to inherit after kexec.
 *
 * ============================================================
 *   STATUS: WORKING end-to-end (Phases 6+7) on L4T r36.4.7, 2026-04-17
 * ============================================================
 *
 * All 10 ioctls succeed; handoff block (wire v2, carrying
 * work_submit_token) is written to an nvmap dmabuf; the magic value
 * survives kexec and is found by SLM-OS's Phase 6 DRAM scan. Ioctl
 * parameters were reverse-engineered from CUDA via an LD_PRELOAD
 * ioctl-snoop (see commit history).
 *
 * Phase 7 (pushbuffer submission) works end-to-end: SLM-OS reads
 * the work_submit_token out of the handoff block and writes it to
 * BAR0+0xBB0090 (physical 0x17BB0090) from EL2. PBDMA consumes
 * the GPFIFO entry and GP_GET advances. The helper's pre-kexec
 * doorbell below is kept as a defensive prime — it's a no-op when
 * PBDMA is already scheduled on this channel but ensures CHRAM
 * has observed at least one update before kexec.
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

/* Shared handoff-block layout + magic. Using the kernel header here
 * ensures the struct field offsets match what SLM-OS expects; if the
 * layout changes, both sides rebuild together. */
#include "../kernel/gpu/nvidia/ga10b_channel_handoff.h"
#include <signal.h>

/* Default time to keep the channel alive waiting for kexec. Override
 * with --timeout-secs. */
#define DEFAULT_TIMEOUT_SECS  300

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

/* Allocate an nvmap buffer and return a dmabuf fd for it. */
static int nvmap_alloc_dmabuf(int nvmap_fd, uint32_t size, uint32_t align)
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

/* SIGTERM handler — on clean shutdown, let the kernel reap us (which
 * releases all nvgpu fds and frees the channel). No explicit cleanup
 * needed because nvgpu's release paths run on fd close. */
static volatile sig_atomic_t g_shutdown;
static void on_term(int sig) { (void)sig; g_shutdown = 1; }

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);  /* unbuffered output for kexec debugging */

    /* Parse --timeout-secs. */
    int timeout_secs = DEFAULT_TIMEOUT_SECS;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--timeout-secs") == 0 && i + 1 < argc) {
            timeout_secs = atoi(argv[++i]);
            if (timeout_secs <= 0) timeout_secs = DEFAULT_TIMEOUT_SECS;
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

    /* Bind channel to TSG. */
    int ch_fd_for_tsg = ch_fd;
    xioctl(tsg_fd, NVGPU_TSG_IOCTL_BIND_CHANNEL, &ch_fd_for_tsg,
           "TSG_BIND");

    /* Set nvmap fd on channel. */
    struct nvgpu_set_nvmap_fd_args nvm = { .fd = nvmap_fd };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_SET_NVMAP_FD, &nvm, "SET_NVMAP");

    /* Disable the watchdog — required when using DETERMINISTIC flag
     * in SETUP_BIND (nvgpu rejects the combination otherwise). */
    struct nvgpu_channel_wdt_args wdt = {
        .wdt_status = NVGPU_IOCTL_CHANNEL_DISABLE_WDT,
        .timeout_ms = 0,
    };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_WDT, &wdt, "WDT_DISABLE");

    /* Allocate buffers via nvmap: USERD (4KB), GPFIFO (8KB = 1024 entries). */
    int userd_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);
    int gpfifo_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 8192, 4096);
    printf("[gpu-helper] USERD dmabuf=%d, GPFIFO dmabuf=%d\n",
           userd_dmabuf, gpfifo_dmabuf);

    /* SETUP_BIND — creates GPFIFO ring + binds channel.
     * flags=0xa (DETERMINISTIC | USERMODE_SUPPORT) captured from CUDA
     * via LD_PRELOAD on L4T r36.4.7. */
    struct nvgpu_channel_setup_bind_args sb;
    memset(&sb, 0, sizeof(sb));
    sb.num_gpfifo_entries = 1024;
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

    /* Bind a compute object context to the channel. Without this, PBDMA
     * walks our pushbuffer entries (GP_GET advances) but the host
     * methods SEM_EXECUTE don't complete — CUDA's strace shows it
     * calling ALLOC_OBJ_CTX with class_num=AMPERE_COMPUTE_A right
     * after SETUP_BIND, and the Phase 7 semaphore-release E2E test
     * starts working once this is added. AMPERE_COMPUTE_A=0xC5C0
     * matches the channel-GPFIFO class 0xC56F's compute-engine peer. */
    struct nvgpu_alloc_obj_ctx_args octx;
    memset(&octx, 0, sizeof(octx));
    octx.class_num = 0xC5C0;   /* AMPERE_COMPUTE_A */
    octx.flags = 0;
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_ALLOC_OBJ_CTX, &octx, "ALLOC_OBJ_CTX");
    printf("[gpu-helper] ALLOC_OBJ_CTX OK (class=0xC5C0, obj_id=0x%llx)\n",
           (unsigned long long)octx.obj_id);

    /* Allocate pushbuffer + semaphore via nvmap. */
    int pb_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 65536, 4096);  /* 64 KB */
    int sem_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);

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
    void *gpfifo_va = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
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
    memset(gpfifo_va, 0, 8192);
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
    struct ga10b_channel_handoff hoff = {
        .magic              = GA10B_CHANNEL_HANDOFF_MAGIC,
        .version            = 2,
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
        .gpfifo_entries     = 1024,
        .gpfifo_entry_size  = 8,
        .pushbuf_phys       = pb_phys,
        .pushbuf_gpu_va     = pb_map.offset,
        .pushbuf_size       = 65536,
        .semaphore_phys     = sem_phys,
        .semaphore_gpu_va   = sem_map.offset,
        .inst_block_phys    = 0,  /* filled in from FECS_CURRENT_CTX if needed */
        .initial_gp_put     = 0,
        .initial_gp_get     = 0,
        .work_submit_token  = sb.work_submit_token,
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

    /* Write SEMAPHORE_RELEASE pushbuffer (10 dwords). Same encoding as
     * SLM-OS Phase 7 smoke test: new host-semaphore methods 0x5C..0x6C,
     * OPERATION=RELEASE(1), 32-bit, WFI enabled. */
    uint32_t *pb32 = (uint32_t *)pb_va;
    uint64_t sem_gva = sem_map.offset;
    pb32[0] = 0x2001005Cu;                /* SEM_ADDR_LO header */
    pb32[1] = (uint32_t)(sem_gva & 0xFFFFFFFFu);
    pb32[2] = 0x20010060u;                /* SEM_ADDR_HI header */
    pb32[3] = (uint32_t)((sem_gva >> 32) & 0xFFu);
    pb32[4] = 0x20010064u;                /* SEM_PAYLOAD_LO header */
    pb32[5] = 0x0000CAFEu;
    pb32[6] = 0x20010068u;                /* SEM_PAYLOAD_HI header */
    pb32[7] = 0;
    pb32[8] = 0x2001006Cu;                /* SEM_EXECUTE header */
    pb32[9] = 0x00000001u;                /* RELEASE | 32-bit | WFI_EN */
    msync(pb_va, 4096, MS_SYNC);

    /* Pre-clear the semaphore so we can detect the GPU write. */
    *(volatile uint32_t *)sem_va = 0;
    msync(sem_va, 4096, MS_SYNC);

    /* Build GPFIFO entry in Ampere HW format, length=10 dwords. */
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
            if (sem_val == 0xCAFEu) break;
            usleep(10000);
        }
        uint32_t gp_get_after = ((volatile uint32_t *)userd_va)[34];
        printf("[gpu-helper] After doorbell: GP_GET=%u (want 1), "
               "sem=0x%08x (want 0xCAFE)\n",
               gp_get_after, sem_val);
        if (sem_val == 0xCAFEu) {
            printf("[gpu-helper] >>> ISOLATION: sema fires Linux-side — "
                   "channel capable, kexec breaks state\n");
        } else if (gp_get_after == 1) {
            printf("[gpu-helper] >>> ISOLATION: PBDMA consumed entry but "
                   "method didn't fire — channel setup incomplete\n");
        } else {
            printf("[gpu-helper] >>> ISOLATION: PBDMA didn't even walk "
                   "the entry — channel not scheduled\n");
        }
    }

    /* Re-read + update handoff with the current GP_PUT/GP_GET values. */
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
