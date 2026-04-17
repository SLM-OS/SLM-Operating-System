/*
 * gpu-channel-helper.c — Create an nvgpu channel on Jetson and write
 * the handoff metadata for SLM-OS to inherit after kexec.
 *
 * ============================================================
 *   STATUS: BLOCKED (2026-04-17)
 * ============================================================
 *
 * This helper compiles but cannot complete on L4T r36.4.7. The
 * first ioctl (NVGPU_GPU_IOCTL_ALLOC_AS) fails with EINVAL on
 * both /dev/nvhost-ctrl-gpu and /dev/nvgpu/igpu0/ctrl, with both
 * big_page_size=0 and big_page_size=0x10000. /dev/nvhost-as-gpu
 * also returns EINVAL on open().
 *
 * CUDA successfully creates channels (see ftrace output with
 * `echo 1 > /sys/kernel/debug/tracing/events/gk20a/enable`), so
 * the ioctl path IS functional — our invocation is missing
 * something nvgpu requires. Diagnosis requires either:
 *
 *   1. The L4T nvgpu source code (only headers are on the Jetson;
 *      the actual ioctl handlers are in the out-of-tree kernel
 *      module which Jetson ships only as binary).
 *   2. strace on a CUDA program to capture the exact ioctl
 *      sequence and flags CUDA uses. strace isn't on the Jetson
 *      by default and installing it may require apt access.
 *   3. L4T documentation or NVIDIA developer forums for the
 *      exact sequence (nvgpu UAPI isn't publicly documented).
 *
 * The SLM-OS side (handoff reader in ga10b_bringup.c Phase 6+7,
 * `peek` shell command, --no-gpu-suspend kexec) is complete and
 * ready. This helper is committed as documented future work.
 *
 * ============================================================
 *
 * Usage: sudo ./gpu-channel-helper
 *
 * This program:
 *   1. Opens a TSG + channel + address space via nvgpu ioctls
 *   2. Allocates GPFIFO, USERD, pushbuffer, and semaphore buffers
 *   3. Sets up the channel (SETUP_BIND with USERMODE_SUPPORT)
 *   4. Maps pushbuffer + semaphore into the GPU address space
 *   5. Writes the channel handoff block to GA10B_CHANNEL_HANDOFF_PHYS
 *   6. Sleeps indefinitely — the kexec helper runs while this sleeps
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

/* Handoff address — must match ga10b_channel_handoff.h in SLM-OS. */
#define HANDOFF_PHYS    0xBDFFF000ULL
#define HANDOFF_MAGIC   0x47505548U  /* "GPUH" */

/* nvmap heap flags */
#define NVMAP_HEAP_SYSMEM  (1 << 31)
#define NVMAP_HANDLE_UNCACHEABLE  0x0

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

int main(void)
{
    setbuf(stdout, NULL);  /* unbuffered output for kexec debugging */
    printf("[gpu-helper] Starting channel creation...\n");

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

    /* ram_userd_gp_put_w = 35, ram_userd_gp_get_w = 34 (from hw_ram_ga10b.h) */
    uint32_t *h = (uint32_t *)handoff;
    h[0] = HANDOFF_MAGIC;           /* magic */
    h[1] = 1;                       /* version */
    h[2] = 0;                       /* channel_id (TODO: get from nvgpu) */
    h[3] = 0;                       /* tsg_id */

    /* USERD */
    *(uint64_t *)&h[4] = userd_phys;
    h[6] = 35 * 4;                  /* gp_put offset = word 35 * 4 bytes */
    h[7] = 34 * 4;                  /* gp_get offset = word 34 * 4 bytes */

    /* GPFIFO */
    *(uint64_t *)&h[8] = gpfifo_phys;
    *(uint64_t *)&h[10] = sb.gpfifo_gpu_va;
    h[12] = 1024;                   /* entries */
    h[13] = 8;                      /* entry size */

    /* Pushbuffer */
    *(uint64_t *)&h[14] = pb_phys;
    *(uint64_t *)&h[16] = pb_map.offset;  /* GPU VA */
    h[18] = 65536;                  /* size */
    h[19] = 0;                      /* pad */

    /* Semaphore */
    *(uint64_t *)&h[20] = sem_phys;
    *(uint64_t *)&h[22] = sem_map.offset; /* GPU VA */

    /* Instance block (for diagnostics — read from FECS_CURRENT_CTX later) */
    *(uint64_t *)&h[24] = 0;       /* will be filled if needed */

    /* GP_PUT / GP_GET initial values */
    h[26] = 0;                      /* initial gp_put */
    h[27] = 0;                      /* initial gp_get */

    msync(handoff, 4096, MS_SYNC);
    printf("[gpu-helper] Handoff block written to dmabuf phys 0x%llx\n",
           (unsigned long long)handoff_phys);
    printf("[gpu-helper] Magic at offset 0: 0x%08x\n", h[0]);

    /* Prime PBDMA: submit a NOP pushbuffer from userspace by writing
     * GP_PUT in USERD and ringing the doorbell on the channel fd's
     * mmap. This "activates" the channel so PBDMA is polling it when
     * SLM-OS later writes GP_PUT after kexec. */
    printf("[gpu-helper] Priming PBDMA with a NOP pushbuffer...\n");

    /* Write a NOP method to the pushbuffer at offset 0 */
    *(uint32_t *)pb_va = 0x00000000u;  /* NOP: subch 0, method 0, count 0 */
    msync(pb_va, 4096, MS_SYNC);

    /* Build the GPFIFO entry (Ampere format):
     *   entry0[31:2] = gpu_va & 0xFFFFFFFC
     *   entry1[7:0]  = gpu_va[39:32]
     *   entry1[30:10] = length in 4-byte dwords */
    uint64_t pb_gva = pb_map.offset;  /* pb GPU VA */
    uint32_t gp_e0 = (uint32_t)(pb_gva & 0xFFFFFFFCu);
    uint32_t gp_e1 = (uint32_t)((pb_gva >> 32) & 0xFFu) | (1u << 10);
    ((uint32_t *)gpfifo_va)[0] = gp_e0;
    ((uint32_t *)gpfifo_va)[1] = gp_e1;
    msync(gpfifo_va, 8, MS_SYNC);
    printf("[gpu-helper] GPFIFO[0] = 0x%08x_%08x (pb_va=0x%llx)\n",
           gp_e1, gp_e0, (unsigned long long)pb_gva);

    /* Advance GP_PUT in USERD (word 35) */
    ((uint32_t *)userd_va)[35] = 1;
    msync(userd_va, 4096, MS_SYNC);

    /* Ring the doorbell: mmap the CTRL fd which provides the shared
     * usermode doorbell region (captured from CUDA's mmap pattern —
     * CUDA mmaps /dev/nvgpu/igpu0/ctrl, not per-channel fds). Write
     * the work_submit_token at offset 0. */
    void *doorbell = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                          MAP_SHARED, ctrl_fd, 0);
    if (doorbell == MAP_FAILED) {
        perror("mmap doorbell page on ctrl fd");
    } else {
        printf("[gpu-helper] Doorbell mapped at %p; writing token 0x%x\n",
               doorbell, sb.work_submit_token);
        *(volatile uint32_t *)doorbell = sb.work_submit_token;
        /* Do not msync — it's MMIO, no dirty flush needed */

        /* Wait briefly for PBDMA to consume. */
        usleep(50000);  /* 50 ms */
        uint32_t gp_get_after = ((volatile uint32_t *)userd_va)[34];
        printf("[gpu-helper] After doorbell: GP_GET = %u (should be 1 if consumed)\n",
               gp_get_after);
    }

    /* Re-read + update handoff with the new GP_PUT/GP_GET values. */
    h[26] = 1;  /* initial_gp_put */
    h[27] = ((volatile uint32_t *)userd_va)[34];  /* initial_gp_get */
    msync(handoff, 4096, MS_SYNC);

    printf("[gpu-helper] Channel ready. Sleeping — run kexec now.\n");
    printf("[gpu-helper] To kexec: sudo slmos-kexec --no-gpu-suspend\n");

    /* Sleep forever — keep fds open so channel stays alive. */
    while (1) sleep(3600);

    return 0;
}
