/*
 * cma-probe.c — Probe Jetson's contiguous-DMA ceiling
 *
 * The W1 architectural decision in `docs/design/gpu-weights-pool.md`
 * hinges on whether nvmap can grant a single ~1.5 GB contiguous DMA
 * buffer to back the SLM weights pool. If it can, the v8 handoff
 * carries one (phys, gpu_va, size) triple. If it can't, the helper
 * has to fall back to multi-chunk allocation and the wire format
 * grows an array.
 *
 * This tool answers that question empirically. It bisects nvmap
 * allocation sizes from 1.5 GB downward, attempts each via the same
 * NVMAP_IOC_CREATE_64 / NVMAP_IOC_ALLOC sequence the channel helper
 * uses, mmap's the result to verify the allocation is real (not a
 * deferred reservation), and reports the largest successful size.
 *
 * Run as root on a Jetson with the L4T nvmap driver loaded:
 *
 *   sudo /home/slmos-1/cma-probe
 *
 * Standalone — does NOT open the GPU channel, set up GPFIFO, or
 * touch the existing helper machinery. The only side effect is the
 * temporary nvmap allocations themselves, all freed before exit.
 *
 * Compile on Jetson:
 *
 *   gcc -O2 -o cma-probe cma-probe.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/types.h>

#include "/usr/src/nvidia/nvidia-oot/include/uapi/linux/nvmap.h"

/* Probe descending sizes. Powers-of-2 + halfway points to bracket
 * the actual ceiling tightly without burning hundreds of attempts.
 *
 * 1.5 GB is the W1 design's target size for Qwen2.5-1.5B; smaller
 * values give a sense of how much headroom the platform actually
 * has. 64 MB is the floor — anything below that is effectively
 * "the SASS pool is fine" and weights staging is infeasible
 * regardless of the wire format. */
static const uint64_t PROBE_SIZES[] = {
    1610612736ull,   /* 1.5 GB — the design target */
    1342177280ull,   /* 1.25 GB */
    1073741824ull,   /* 1 GB */
     805306368ull,   /* 768 MB */
     536870912ull,   /* 512 MB */
     402653184ull,   /* 384 MB */
     268435456ull,   /* 256 MB */
     134217728ull,   /* 128 MB */
      67108864ull,   /*  64 MB */
};
#define N_PROBE_SIZES (sizeof(PROBE_SIZES) / sizeof(PROBE_SIZES[0]))

/* Best-effort dump of meminfo + CMA stats so the reader can
 * correlate the probe ceiling with the platform's actual reserves. */
static void dump_platform_context(void)
{
    printf("=== Platform context ===\n");
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "MemTotal:", 9) == 0 ||
                strncmp(line, "MemFree:",  8) == 0 ||
                strncmp(line, "MemAvailable:", 13) == 0 ||
                strncmp(line, "CmaTotal:", 9) == 0 ||
                strncmp(line, "CmaFree:",  8) == 0) {
                fputs(line, stdout);
            }
        }
        fclose(f);
    }

    /* CMA debugfs (kernel must be built with CONFIG_CMA_DEBUGFS).
     * Often available on L4T but not guaranteed; print whatever's
     * accessible without erroring out. */
    f = fopen("/sys/kernel/debug/cma/cma-default/count", "r");
    if (f) {
        unsigned long pages = 0;
        if (fscanf(f, "%lu", &pages) == 1) {
            printf("cma-default/count: %lu pages (%.1f MB at 4 KB/page)\n",
                   pages, (pages * 4.0) / 1024.0);
        }
        fclose(f);
    }
    printf("\n");
}

/* Try one allocation at the given size. Returns the dmabuf fd on
 * success or -1 on any failure (allocation, mmap, page-touch). All
 * resources owned by this call are released before return. */
static int try_alloc(int nvmap_fd, uint64_t size)
{
    /* CREATE_64: declare the handle's intended size. */
    struct nvmap_create_handle cr;
    memset(&cr, 0, sizeof(cr));
    cr.size64 = size;
    if (ioctl(nvmap_fd, NVMAP_IOC_CREATE_64, &cr) < 0) {
        printf("  CREATE_64(%llu) -> errno=%d (%s)\n",
               (unsigned long long)size, errno, strerror(errno));
        return -1;
    }
    uint32_t handle = cr.handle64;

    /* ALLOC: actually back the handle with physical pages. This is
     * where the contiguous-DMA constraint bites; if CMA can't grant
     * the size, this ioctl fails. */
    struct nvmap_alloc_handle al;
    memset(&al, 0, sizeof(al));
    al.handle = handle;
    al.heap_mask = 0x40000000;  /* IOVMM heap — same as gpu-channel-helper.c */
    al.flags = 0x8000003;       /* cacheable + nvmap-specific bits */
    al.align = 0x1000;
    al.numa_nid = -1;
    if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &al) < 0) {
        int saved = errno;
        printf("  ALLOC(%llu) -> errno=%d (%s)\n",
               (unsigned long long)size, saved, strerror(saved));
        /* Drop the handle even though ALLOC failed — CREATE_64
         * leaked the descriptor on the kernel side. */
        struct nvmap_handle_param fp = { .handle = handle };
        (void)fp;  /* nvmap free-by-handle-only path is awkward;
                    * letting the process exit reaps it. */
        return -1;
    }

    /* GET_FD: convert the nvmap handle into a dmabuf fd we can mmap. */
    struct nvmap_create_handle gf;
    memset(&gf, 0, sizeof(gf));
    gf.handle = handle;
    if (ioctl(nvmap_fd, NVMAP_IOC_GET_FD, &gf) < 0) {
        printf("  GET_FD(%llu) -> errno=%d (%s)\n",
               (unsigned long long)size, errno, strerror(errno));
        return -1;
    }

    /* mmap the buffer to confirm the allocation is real. nvmap
     * sometimes succeeds CREATE+ALLOC then fails on first touch
     * (deferred reservation against an oversubscribed CMA pool);
     * mmap forces the pages to materialize. */
    void *va = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, gf.fd, 0);
    if (va == MAP_FAILED) {
        printf("  mmap(%llu) -> errno=%d (%s)\n",
               (unsigned long long)size, errno, strerror(errno));
        close(gf.fd);
        return -1;
    }

    /* Touch the first byte of every 64 KB chunk to force every CMA
     * page into the buffer's backing. If CMA was lying about the
     * allocation, this is where the SIGBUS would fire. */
    volatile uint8_t *p = (volatile uint8_t *)va;
    uint64_t step = 64ull * 1024;
    for (uint64_t off = 0; off < size; off += step) {
        p[off] = 0xA5;
    }
    /* Verify the writes stuck (rules out a copy-on-write sham). */
    for (uint64_t off = 0; off < size; off += step) {
        if (p[off] != 0xA5) {
            printf("  page-verify(%llu) failed at off=%llu\n",
                   (unsigned long long)size, (unsigned long long)off);
            munmap(va, size);
            close(gf.fd);
            return -1;
        }
    }

    munmap(va, size);
    return gf.fd;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("cma-probe — Jetson contiguous-DMA ceiling probe\n");
    printf("(W1 prerequisite per docs/design/gpu-weights-pool.md)\n\n");

    dump_platform_context();

    int nvmap_fd = open("/dev/nvmap", O_RDWR);
    if (nvmap_fd < 0) {
        perror("open /dev/nvmap");
        return 1;
    }

    printf("=== Probe descending sizes ===\n");
    uint64_t largest_ok = 0;
    int largest_fd = -1;
    for (size_t i = 0; i < N_PROBE_SIZES; i++) {
        uint64_t sz = PROBE_SIZES[i];
        printf("[%zu/%zu] try %llu B (%.1f MB)\n",
               i + 1, N_PROBE_SIZES,
               (unsigned long long)sz, sz / (1024.0 * 1024.0));
        int fd = try_alloc(nvmap_fd, sz);
        if (fd >= 0) {
            printf("  OK\n");
            if (sz > largest_ok) {
                if (largest_fd >= 0) close(largest_fd);
                largest_ok = sz;
                largest_fd = fd;
            } else {
                close(fd);
            }
            /* First success — no need to probe smaller sizes;
             * larger sizes were already tried earlier. */
            break;
        }
    }

    printf("\n=== Result ===\n");
    if (largest_ok == 0) {
        printf("FAIL: no size in the probe table allocated successfully.\n");
        printf("Even 64 MB contiguous failed — something is wrong with\n");
        printf("the platform (CMA exhausted or nvmap unhealthy).\n");
        if (nvmap_fd >= 0) close(nvmap_fd);
        return 2;
    }
    printf("Largest contiguous DMA: %llu B (%.2f GB)\n",
           (unsigned long long)largest_ok,
           largest_ok / (1024.0 * 1024.0 * 1024.0));
    printf("\nW1 implication:\n");
    if (largest_ok >= 1610612736ull) {
        printf("  ✓ 1.5 GB target is feasible. v8 handoff can carry a\n");
        printf("    single (phys, gpu_va, size) triple. Multi-chunk\n");
        printf("    fallback NOT required for the design-target size.\n");
    } else if (largest_ok >= 1073741824ull) {
        printf("  ⚠ 1.5 GB target is NOT feasible but 1 GB is.\n");
        printf("    Qwen2.5-1.5B Q4_K_M weights (~1 GB total) fit in a\n");
        printf("    single chunk, but headroom for KV cache + scratch is\n");
        printf("    tight. v8 schema can stay single-triple if we cap\n");
        printf("    pool size at 1 GB, or grow to multi-chunk.\n");
    } else {
        printf("  ✗ 1.5 GB target NOT feasible; %.2f GB max contiguous\n",
               largest_ok / (1024.0 * 1024.0 * 1024.0));
        printf("    means the v8 handoff MUST carry an array of\n");
        printf("    (phys, gpu_va, size) chunks rather than a single\n");
        printf("    triple. The helper will need to allocate N buffers\n");
        printf("    each up to %llu B and let the GMMU stitch them.\n",
               (unsigned long long)largest_ok);
    }

    /* Free the buffer + nvmap fd; the kernel reaps the rest. */
    if (largest_fd >= 0) close(largest_fd);
    close(nvmap_fd);
    return 0;
}
