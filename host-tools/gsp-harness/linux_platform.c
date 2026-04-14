/*
 * linux_platform.c — Linux userspace implementation of
 * `struct gsp_platform_ops` (see kernel/gpu/nvidia/gsp.h).
 *
 * This is the THIRD platform implementation alongside x86-64
 * bare-metal (kernel/arch/x86_64/nvidia_gsp_platform.c) and
 * (future) Jetson bare-metal (kernel/arch/arm64/). It targets
 * an NVIDIA Ampere GPU that has been bound to vfio-pci — the
 * kernel keeps hands off, and we mmap BAR0/BAR1 directly.
 *
 * See docs/testing/test-pc-linux-vfio-setup.md for the one-time
 * host configuration.
 */

/* _GNU_SOURCE is provided by the Makefile's -D flag. */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../../kernel/gpu/nvidia/gsp.h"
#include "../../kernel/gpu/nvidia/nvidia_vbios.h"

/* ---- Global state for the Linux platform ops ----
 *
 * All mutable state is file-scope globals because the harness is a
 * short-lived process with exactly one GPU. Kernel bare-metal has
 * the same single-GPU assumption today; if that changes, both impls
 * grow a context argument together.
 */

static volatile uint32_t *g_bar0;    /* BAR0 MMIO — 16 MB mapping */
static size_t             g_bar0_size;
static volatile uint8_t  *g_bar1;    /* BAR1 VRAM — 256 MB mapping */
static size_t             g_bar1_size;
static bool               g_trace;

/* PCI device sysfs path captured at init — needed again by the lazy
 * VBIOS loader. Owned by the caller of linux_gsp_platform_init(),
 * which guarantees the string outlives the harness process. */
static const char *g_pci_path;

/* VBIOS state: the raw bytes live in g_vbios_buf (grown via malloc),
 * the parsed view lives in g_vbios. Loaded on first fwsec request. */
static uint8_t              *g_vbios_buf;
static size_t                g_vbios_buf_size;
static struct nvidia_vbios   g_vbios;
static bool                  g_vbios_loaded;

/* Firmware blobs loaded from /lib/firmware via the Linux VFS, in
 * the same format the bare-metal build embeds via .incbin. */
struct host_blob {
    uint8_t *data;
    size_t   size;
};
static struct host_blob g_fw_blobs[GSP_FW_KIND_COUNT];
static const char VERSION_STR[] = "535.113.01";

/* ---- BAR0 register access ---- */

static uint32_t linux_gsp_read32(uint32_t offset)
{
    if (!g_bar0 || offset + 4 > g_bar0_size) {
        fprintf(stderr, "gsp_read32(0x%x): BAR0 not mapped or out of range\n",
                offset);
        return 0xBADF5040u;
    }
    uint32_t v = g_bar0[offset / 4];
    if (g_trace)
        fprintf(stderr, "  r32[0x%06x] = 0x%08x\n", offset, v);
    return v;
}

static void linux_gsp_write32(uint32_t offset, uint32_t value)
{
    if (!g_bar0 || offset + 4 > g_bar0_size) {
        fprintf(stderr, "gsp_write32(0x%x): BAR0 not mapped or out of range\n",
                offset);
        return;
    }
    if (g_trace)
        fprintf(stderr, "  w32[0x%06x] = 0x%08x\n", offset, value);
    g_bar0[offset / 4] = value;
}

/* ---- BAR1 (VRAM) byte access ---- */

static void linux_gsp_bar1_read(uint32_t offset, void *dst, size_t n)
{
    if (!g_bar1 || offset + n > g_bar1_size) {
        fprintf(stderr, "bar1_read(0x%x, %zu): out of range\n", offset, n);
        return;
    }
    memcpy(dst, (const void *)(g_bar1 + offset), n);
}

static void linux_gsp_bar1_write(uint32_t offset, const void *src, size_t n)
{
    if (!g_bar1 || offset + n > g_bar1_size) {
        fprintf(stderr, "bar1_write(0x%x, %zu): out of range\n", offset, n);
        return;
    }
    memcpy((void *)(g_bar1 + offset), src, n);
}

/* ---- DMA allocation ----
 *
 * Under VFIO, userspace allocates a memory region and registers it
 * with the IOMMU via VFIO_IOMMU_MAP_DMA — the returned IOVA is what
 * the GPU uses to address the region. For now we take a shortcut:
 * allocate anonymous mmap'd memory and return its virtual address.
 * This works for testing BAR0 / BAR1 accessors but NOT for anything
 * that requires the GPU to DMA into it (Phase 2+ FB layout, Phase 6
 * message queues, engine submission).
 *
 * Full VFIO IOMMU DMA binding lands in E3 when the Falcon bringup
 * actually needs it — at that point this function grows a full
 * VFIO_IOMMU_MAP_DMA ioctl dance.
 */
static void *linux_gsp_dma_alloc(size_t size, size_t align, uint64_t *out_dma)
{
    size_t alloc_size = size;
    if (align < 4096) align = 4096;
    /* Round up to page alignment — mmap always returns page-aligned. */
    alloc_size = (alloc_size + 4095) & ~(size_t)4095;
    void *p = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        if (out_dma) *out_dma = 0;
        return NULL;
    }
    /* TODO(E3): VFIO_IOMMU_MAP_DMA to get a real IOVA. For probe-
     * level harness use the virtual address — Phase 0 / 1 code
     * paths don't dereference the DMA addr from the GPU side. */
    if (out_dma) *out_dma = (uint64_t)(uintptr_t)p;
    (void)align;
    return p;
}

static void linux_gsp_dma_free(void *ptr, size_t size)
{
    if (!ptr) return;
    size_t alloc_size = (size + 4095) & ~(size_t)4095;
    munmap(ptr, alloc_size);
}

/* ---- Cache ops / barrier ---- */

static void linux_gsp_cache_clean(const void *a, size_t s)      { (void)a; (void)s; }
static void linux_gsp_cache_invalidate(void *a, size_t s)       { (void)a; (void)s; }
static void linux_gsp_mb(void) { __asm__ volatile("mfence" ::: "memory"); }

/* ---- Firmware accessor (loads from /lib/firmware at init) ---- */

static void linux_gsp_firmware_get(enum gsp_firmware_kind kind,
                                   struct gsp_firmware_blob *out)
{
    if ((int)kind < 0 || kind >= GSP_FW_KIND_COUNT || !g_fw_blobs[kind].data) {
        out->data = NULL;
        out->size = 0;
        out->version = NULL;
        return;
    }
    out->data = g_fw_blobs[kind].data;
    out->size = g_fw_blobs[kind].size;
    out->version = VERSION_STR;
}

/* ---- VBIOS ----
 *
 * Linux exposes the GPU expansion ROM at
 *   /sys/bus/pci/devices/<BDF>/rom
 * but the file is empty until you enable ROM reads by writing "1"
 * to it. This matches the bare-metal ROM BAR toggle — the kernel is
 * doing the same PCI config write under the hood.
 *
 * Sequence:
 *   1. open  .../rom  O_RDWR
 *   2. write "1\n"    enables ROM decoding on the device
 *   3. read  bytes    until EOF (image size typically 128–512 KB)
 *   4. write "0\n"    restore (optional — the kernel also does this
 *                     when the fd closes, but being explicit is cheap)
 */
int nvidia_vbios_platform_load(const uint8_t **out_data, size_t *out_size)
{
    if (g_vbios_loaded) {
        if (out_data) *out_data = g_vbios.image;
        if (out_size) *out_size = g_vbios.image_size;
        return g_vbios.parsed_ok ? 0 : -1;
    }

    if (!g_pci_path) {
        fprintf(stderr, "[VBIOS] platform not initialized\n");
        return -1;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/rom", g_pci_path);

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[VBIOS] open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* Enable ROM decoding. */
    if (write(fd, "1\n", 2) != 2) {
        fprintf(stderr, "[VBIOS] enable ROM: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* Seek back — some kernels advance the offset on the enable-write. */
    if (lseek(fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "[VBIOS] lseek: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* Grow buffer as we read. 256 KB start covers nearly all GPUs. */
    size_t cap = 256 * 1024;
    size_t n = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { close(fd); return -1; }

    for (;;) {
        if (n == cap) {
            if (cap >= NVIDIA_VBIOS_MAX_SIZE) break;
            cap *= 2;
            if (cap > NVIDIA_VBIOS_MAX_SIZE) cap = NVIDIA_VBIOS_MAX_SIZE;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(fd); return -1; }
            buf = nb;
        }
        ssize_t got = read(fd, buf + n, cap - n);
        if (got < 0) {
            fprintf(stderr, "[VBIOS] read: %s\n", strerror(errno));
            free(buf); close(fd);
            return -1;
        }
        if (got == 0) break;
        n += (size_t)got;
    }

    /* Best-effort restore — ignore errors, we're about to close.
     * The ssize_t capture is to keep -Wunused-result quiet; gcc's
     * `warn_unused_result` attribute on write(2) ignores the (void)
     * cast. */
    if (lseek(fd, 0, SEEK_SET) < 0) { /* ignored */ }
    ssize_t _disable_rc = write(fd, "0\n", 2);
    (void)_disable_rc;
    close(fd);

    if (n < 4) {
        fprintf(stderr, "[VBIOS] only %zu bytes from %s\n", n, path);
        free(buf);
        return -1;
    }

    if (nvidia_vbios_parse(buf, n, &g_vbios) < 0) {
        fprintf(stderr, "[VBIOS] parse failed (%zu bytes)\n", n);
        free(buf);
        return -1;
    }

    g_vbios_buf = buf;
    g_vbios_buf_size = n;
    g_vbios_loaded = true;
    fprintf(stderr, "[VBIOS] parsed %zu bytes, %u BIT entries\n",
            n, g_vbios.num_entries);

    if (out_data) *out_data = g_vbios.image;
    if (out_size) *out_size = g_vbios.image_size;
    return 0;
}

static int linux_gsp_vbios_get_fwsec(const void **out_data, size_t *out_size)
{
    if (!g_vbios_loaded) {
        const uint8_t *tmp; size_t tsz;
        if (nvidia_vbios_platform_load(&tmp, &tsz) < 0) {
            *out_data = NULL; *out_size = 0;
            return -1;
        }
    }
    const uint8_t *fw = NULL;
    uint32_t fw_size = 0;
    if (nvidia_vbios_get_fwsec(&g_vbios, &fw, &fw_size) < 0) {
        *out_data = NULL; *out_size = 0;
        return -1;
    }
    *out_data = fw;
    *out_size = fw_size;
    return 0;
}

/* ---- Vtable ---- */

static const struct gsp_platform_ops linux_ops = {
    .read32           = linux_gsp_read32,
    .write32          = linux_gsp_write32,
    .bar1_read        = linux_gsp_bar1_read,
    .bar1_write       = linux_gsp_bar1_write,
    .dma_alloc        = linux_gsp_dma_alloc,
    .dma_free         = linux_gsp_dma_free,
    .cache_clean      = linux_gsp_cache_clean,
    .cache_invalidate = linux_gsp_cache_invalidate,
    .mb               = linux_gsp_mb,
    .firmware_get     = linux_gsp_firmware_get,
    .vbios_get_fwsec  = linux_gsp_vbios_get_fwsec,
};

/* ---- Setup ---- */

static int map_bar(const char *pci_path, int bar_index,
                   volatile void **out_ptr, size_t *out_size)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/resource%d", pci_path, bar_index);
    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "fstat %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    void *p = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap %s (%zu bytes): %s\n",
                path, (size_t)st.st_size, strerror(errno));
        return -1;
    }
    *out_ptr = p;
    *out_size = (size_t)st.st_size;
    return 0;
}

static int load_firmware(const char *chip)
{
    /* File paths under /lib/firmware/nvidia/<chip>/gsp/. Files are
     * zstd-compressed; we decompress inline using the external
     * `zstd` tool via popen for simplicity. An alternative would be
     * to link libzstd, but that's one more dependency. */
    static const char *NAMES[GSP_FW_KIND_COUNT] = {
        [GSP_FW_GSP]           = "gsp-535.113.01.bin.zst",
        [GSP_FW_BOOTLOADER]    = "bootloader-535.113.01.bin.zst",
        [GSP_FW_BOOTER_LOAD]   = "booter_load-535.113.01.bin.zst",
        [GSP_FW_BOOTER_UNLOAD] = "booter_unload-535.113.01.bin.zst",
    };

    for (int k = 0; k < GSP_FW_KIND_COUNT; k++) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "zstd -d -c /lib/firmware/nvidia/%s/gsp/%s",
                 chip, NAMES[k]);
        FILE *p = popen(cmd, "r");
        if (!p) {
            fprintf(stderr, "popen %s: %s\n", cmd, strerror(errno));
            return -1;
        }
        /* Grow the buffer as we read. gsp.bin is ~38 MB; the three
         * Falcon ucodes are < 100 KB each. Starting 16 MB avoids
         * many realloc cycles for the big one. */
        size_t cap = 16 * 1024 * 1024;
        size_t n = 0;
        uint8_t *buf = malloc(cap);
        if (!buf) { pclose(p); return -1; }
        while (!feof(p)) {
            if (n == cap) {
                cap *= 2;
                uint8_t *nb = realloc(buf, cap);
                if (!nb) { free(buf); pclose(p); return -1; }
                buf = nb;
            }
            size_t got = fread(buf + n, 1, cap - n, p);
            n += got;
            if (got == 0) break;
        }
        int rc = pclose(p);
        if (rc != 0) {
            fprintf(stderr, "zstd -d %s failed with rc %d\n", NAMES[k], rc);
            free(buf);
            return -1;
        }
        g_fw_blobs[k].data = buf;
        g_fw_blobs[k].size = n;
    }
    return 0;
}

/*
 * Public initializer. Maps BAR0/BAR1, loads firmware, installs the
 * vtable. Returns 0 on success. Called once from main().
 */
int linux_gsp_platform_init(const char *pci_path, const char *chip,
                            bool trace)
{
    g_trace = trace;
    g_pci_path = pci_path;

    if (map_bar(pci_path, 0, (volatile void **)&g_bar0, &g_bar0_size) < 0)
        return -1;
    if (map_bar(pci_path, 1, (volatile void **)&g_bar1, &g_bar1_size) < 0)
        return -1;
    fprintf(stderr, "[GSP-HARNESS] BAR0 mapped at %p size %zu\n",
            (void *)g_bar0, g_bar0_size);
    fprintf(stderr, "[GSP-HARNESS] BAR1 mapped at %p size %zu\n",
            (void *)g_bar1, g_bar1_size);

    if (load_firmware(chip) < 0) {
        fprintf(stderr, "[GSP-HARNESS] firmware load failed (chip=%s)\n", chip);
        return -1;
    }

    extern const struct gsp_platform_ops *gsp_platform;
    gsp_platform = &linux_ops;
    return 0;
}
