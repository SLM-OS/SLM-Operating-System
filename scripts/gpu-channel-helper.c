/*
 * gpu-channel-helper.c — Create an nvgpu channel on Jetson and write
 * the handoff metadata for SLM-OS to inherit after kexec.
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
        fprintf(stderr, "%s: %s\n", name, strerror(errno));
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

    /* Allocate backing memory */
    struct nvmap_alloc_handle al = {
        .handle = handle,
        .heap_mask = NVMAP_HEAP_SYSMEM,
        .flags = NVMAP_HANDLE_UNCACHEABLE,
        .align = align,
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
    printf("[gpu-helper] Starting channel creation...\n");

    /* Open nvmap for buffer allocation. */
    int nvmap_fd = xopen("/dev/nvmap", O_RDWR);

    /* Open GPU ctrl device. */
    int ctrl_fd = xopen("/dev/nvhost-ctrl-gpu", O_RDWR);

    /* Allocate address space. */
    struct nvgpu_alloc_as_args as_args = {
        .big_page_size = 0x10000,  /* 64 KB big pages */
    };
    xioctl(ctrl_fd, NVGPU_GPU_IOCTL_ALLOC_AS, &as_args, "ALLOC_AS");
    int as_fd = as_args.as_fd;
    printf("[gpu-helper] AS fd=%d\n", as_fd);

    /* Open TSG. */
    struct nvgpu_gpu_open_tsg_args tsg_args = { 0 };
    xioctl(ctrl_fd, NVGPU_GPU_IOCTL_OPEN_TSG, &tsg_args, "OPEN_TSG");
    int tsg_fd = tsg_args.tsg_fd;
    printf("[gpu-helper] TSG fd=%d\n", tsg_fd);

    /* Open channel. */
    struct nvgpu_gpu_open_channel_args ch_args = {
        .runlist_id = -1,  /* default: GR runlist */
    };
    xioctl(ctrl_fd, NVGPU_GPU_IOCTL_OPEN_CHANNEL, &ch_args,
           "OPEN_CHANNEL");
    int ch_fd = ch_args.channel_fd;
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

    /* Allocate buffers via nvmap: USERD (4KB), GPFIFO (8KB = 1024 entries). */
    int userd_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);
    int gpfifo_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 8192, 4096);
    printf("[gpu-helper] USERD dmabuf=%d, GPFIFO dmabuf=%d\n",
           userd_dmabuf, gpfifo_dmabuf);

    /* SETUP_BIND — creates GPFIFO ring + binds channel. */
    struct nvgpu_channel_setup_bind_args sb = {
        .num_gpfifo_entries = 1024,
        .num_inflight_jobs = 0,
        .flags = NVGPU_CHANNEL_SETUP_BIND_FLAGS_USERMODE_SUPPORT,
        .userd_dmabuf_fd = userd_dmabuf,
        .gpfifo_dmabuf_fd = gpfifo_dmabuf,
    };
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

    /* Map pushbuffer into GPU AS. */
    struct nvgpu_as_map_buffer_ex_args pb_map = {
        .dmabuf_fd = pb_dmabuf,
        .mapping_size = 65536,
        .page_size = 4096,
    };
    xioctl(as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &pb_map, "MAP_PB");
    printf("[gpu-helper] Pushbuffer GPU VA = 0x%llx\n",
           (unsigned long long)pb_map.offset);

    /* Map semaphore into GPU AS. */
    struct nvgpu_as_map_buffer_ex_args sem_map = {
        .dmabuf_fd = sem_dmabuf,
        .mapping_size = 4096,
        .page_size = 4096,
    };
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

    /* Write the handoff block to the fixed DRAM address. */
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) { perror("/dev/mem"); return 1; }

    void *handoff = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_SHARED, mem_fd, HANDOFF_PHYS);
    if (handoff == MAP_FAILED) {
        perror("mmap handoff");
        return 1;
    }

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
    printf("[gpu-helper] Handoff block written to 0x%llx\n",
           (unsigned long long)HANDOFF_PHYS);

    printf("[gpu-helper] Channel ready. Sleeping — run kexec now.\n");
    printf("[gpu-helper] To kexec: sudo slmos-kexec --no-gpu-suspend\n");

    /* Sleep forever — keep fds open so channel stays alive. */
    while (1) sleep(3600);

    return 0;
}
