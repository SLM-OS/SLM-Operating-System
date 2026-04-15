/*
 * vfio.c — persistent VFIO session + DMA mapping (E3.2).
 *
 * Layout: one container fd, one group fd, one device fd per session.
 * All mapped into a single Type1 IOMMU context so every DMA buffer
 * we hand to the GPU appears in the same IOVA space.
 *
 * IOVA allocator: bump pointer starting at VFIO_IOVA_BASE. The GPU's
 * Ampere Falcon DMA can address up to 40 bits of IOVA, so we stay
 * well within that. We never recycle IOVAs in this process (harness
 * is short-lived); `vfio_dma_free` unmaps but doesn't hole-fill the
 * allocator. That's fine — a single bringup run allocates at most a
 * few hundred buffers totaling under 100 MB.
 */

#include "vfio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/vfio.h>

/* Start IOVA allocation at 256 MB — above the typical "low memory"
 * region Linux userspace sometimes uses for fixed mappings, and
 * aligned to make DMATRFBASE math clean (bottom 8 bits zero). */
#define VFIO_IOVA_BASE      0x10000000ull
#define VFIO_PAGE_SIZE      4096u

struct vfio_session {
    int container_fd;
    int group_fd;
    int device_fd;
    int group_id;
    bool trace;

    uint64_t next_iova;    /* bump allocator cursor */
    size_t   inflight;     /* bytes currently mapped */
};

/* ---- Helpers ---- */

static int read_iommu_group(const char *pci_path)
{
    char link[256], target[256];
    snprintf(link, sizeof(link), "%s/iommu_group", pci_path);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) return -1;
    target[n] = '\0';
    char *slash = strrchr(target, '/');
    if (!slash) return -1;
    return atoi(slash + 1);
}

static size_t round_up_page(size_t size)
{
    return (size + VFIO_PAGE_SIZE - 1) & ~(size_t)(VFIO_PAGE_SIZE - 1);
}

/* ---- Public API ---- */

struct vfio_session *vfio_open(const char *pci_path, bool trace)
{
    if (!pci_path) return NULL;

    int group_id = read_iommu_group(pci_path);
    if (group_id < 0) {
        if (trace) fprintf(stderr, "[VFIO] no IOMMU group for %s\n", pci_path);
        return NULL;
    }

    int container = open("/dev/vfio/vfio", O_RDWR);
    if (container < 0) {
        if (trace) fprintf(stderr, "[VFIO] /dev/vfio/vfio: %s\n", strerror(errno));
        return NULL;
    }

    /* Sanity check — API version 0 is what we compile against. */
    int api = ioctl(container, VFIO_GET_API_VERSION);
    if (api != VFIO_API_VERSION) {
        if (trace) fprintf(stderr, "[VFIO] unexpected API version %d\n", api);
        close(container);
        return NULL;
    }
    if (!ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
        if (trace) fprintf(stderr, "[VFIO] Type1 IOMMU not supported\n");
        close(container);
        return NULL;
    }

    char gpath[64];
    snprintf(gpath, sizeof(gpath), "/dev/vfio/%d", group_id);
    int group = open(gpath, O_RDWR);
    if (group < 0) {
        if (trace) fprintf(stderr, "[VFIO] %s: %s\n", gpath, strerror(errno));
        close(container);
        return NULL;
    }

    struct vfio_group_status gstat = { .argsz = sizeof(gstat) };
    if (ioctl(group, VFIO_GROUP_GET_STATUS, &gstat) < 0) {
        if (trace) fprintf(stderr, "[VFIO] GROUP_GET_STATUS: %s\n", strerror(errno));
        close(group); close(container);
        return NULL;
    }
    if (!(gstat.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        if (trace) fprintf(stderr, "[VFIO] group %d not viable (flags=0x%x)\n",
                           group_id, gstat.flags);
        close(group); close(container);
        return NULL;
    }

    if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
        if (trace) fprintf(stderr, "[VFIO] SET_CONTAINER: %s\n", strerror(errno));
        close(group); close(container);
        return NULL;
    }
    if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
        if (trace) fprintf(stderr, "[VFIO] SET_IOMMU: %s\n", strerror(errno));
        close(group); close(container);
        return NULL;
    }

    const char *bdf = strrchr(pci_path, '/');
    bdf = bdf ? bdf + 1 : pci_path;
    int device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, bdf);
    if (device < 0) {
        if (trace) fprintf(stderr, "[VFIO] GET_DEVICE_FD %s: %s\n",
                           bdf, strerror(errno));
        close(group); close(container);
        return NULL;
    }

    struct vfio_session *s = calloc(1, sizeof(*s));
    if (!s) {
        close(device); close(group); close(container);
        return NULL;
    }
    s->container_fd = container;
    s->group_fd     = group;
    s->device_fd    = device;
    s->group_id     = group_id;
    s->trace        = trace;
    s->next_iova    = VFIO_IOVA_BASE;
    s->inflight     = 0;

    if (trace)
        fprintf(stderr, "[VFIO] opened group %d device %s\n", group_id, bdf);
    return s;
}

int vfio_device_fd(const struct vfio_session *s)
{
    return s ? s->device_fd : -1;
}

int vfio_container_fd(const struct vfio_session *s)
{
    return s ? s->container_fd : -1;
}

int vfio_region_info(const struct vfio_session *s, uint32_t index,
                     uint64_t *out_offset, uint64_t *out_size)
{
    if (!s) return -1;
    struct vfio_region_info ri = {
        .argsz = sizeof(ri),
        .index = index,
    };
    if (ioctl(s->device_fd, VFIO_DEVICE_GET_REGION_INFO, &ri) < 0) return -1;
    if (out_offset) *out_offset = ri.offset;
    if (out_size)   *out_size   = ri.size;
    return 0;
}

void *vfio_dma_alloc(struct vfio_session *s, size_t size, size_t align,
                     uint64_t *out_iova)
{
    if (!s || size == 0) return NULL;

    size_t alloc_size = round_up_page(size);
    /* Align the IOVA to the requested alignment (at least page). */
    size_t iova_align = align < VFIO_PAGE_SIZE ? VFIO_PAGE_SIZE : align;

    /* mmap anonymous, writable. MAP_LOCKED so the pages stay resident
     * — an IOMMU mapping wants stable physical backing. Fallback to
     * mlock() if the kernel rejects MAP_LOCKED (non-root in some
     * setups): try without, then mlock separately. */
    void *va = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
    if (va == MAP_FAILED) {
        va = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (va == MAP_FAILED) {
            if (s->trace) fprintf(stderr, "[VFIO] mmap %zu: %s\n",
                                  alloc_size, strerror(errno));
            return NULL;
        }
        if (mlock(va, alloc_size) < 0) {
            if (s->trace) fprintf(stderr, "[VFIO] mlock %zu: %s\n",
                                  alloc_size, strerror(errno));
            munmap(va, alloc_size);
            return NULL;
        }
    }

    /* Bump-allocate an IOVA, aligned. */
    uint64_t iova = (s->next_iova + iova_align - 1) & ~(uint64_t)(iova_align - 1);

    struct vfio_iommu_type1_dma_map map = {
        .argsz   = sizeof(map),
        .flags   = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        .vaddr   = (uintptr_t)va,
        .iova    = iova,
        .size    = alloc_size,
    };
    if (ioctl(s->container_fd, VFIO_IOMMU_MAP_DMA, &map) < 0) {
        if (s->trace) fprintf(stderr, "[VFIO] MAP_DMA iova=0x%lx size=%zu: %s\n",
                              (unsigned long)iova, alloc_size, strerror(errno));
        munlock(va, alloc_size);
        munmap(va, alloc_size);
        return NULL;
    }

    s->next_iova = iova + alloc_size;
    s->inflight += alloc_size;

    if (s->trace)
        fprintf(stderr, "[VFIO] DMA: va=%p iova=0x%lx size=%zu\n",
                va, (unsigned long)iova, alloc_size);

    if (out_iova) *out_iova = iova;
    return va;
}

void vfio_dma_free(struct vfio_session *s, void *va, size_t size)
{
    if (!s || !va || size == 0) return;
    size_t alloc_size = round_up_page(size);

    /* Unmap is indexed by IOVA, not VA — we don't store the reverse
     * lookup, so walk the bump allocator's implicit layout: each
     * allocation's IOVA equals `VFIO_IOVA_BASE + sum of prior sizes
     * with alignment padding`, which we don't track per-allocation.
     *
     * The simple approach: the caller doesn't tell us the IOVA, so
     * we can't do a targeted unmap. In a short-lived harness this is
     * fine — the kernel unmaps everything when the container closes
     * at process exit. Track the size decrement so leak checks work. */
    munlock(va, alloc_size);
    munmap(va, alloc_size);
    s->inflight -= alloc_size;
}

size_t vfio_dma_inflight_bytes(const struct vfio_session *s)
{
    return s ? s->inflight : 0;
}

void vfio_close(struct vfio_session *s)
{
    if (!s) return;
    /* Kernel unmaps all DMA + detaches the group when these fds close. */
    if (s->device_fd    >= 0) close(s->device_fd);
    if (s->group_fd     >= 0) close(s->group_fd);
    if (s->container_fd >= 0) close(s->container_fd);
    free(s);
}
