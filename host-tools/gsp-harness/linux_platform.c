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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/vfio.h>

#include "../../kernel/gpu/nvidia/gsp.h"
#include "../../kernel/gpu/nvidia/nvidia_vbios.h"
#include "vfio.h"

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

/* Persistent VFIO session (E3.2). NULL if VFIO isn't available for
 * this device — the harness still works via sysfs in that case, but
 * DMA-dependent operations (Falcon ucode upload, RPC rings) will
 * fail with a clear diagnostic. */
static struct vfio_session *g_vfio;

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
 * Routes through the shared VFIO session (vfio.c) so every DMA buffer
 * is mapped into the IOMMU and reachable from the GPU. This is what
 * E3.2+ needs — Falcon ucode upload, Booter Load, GSP-RM RPC rings
 * all DMA from system memory using the IOVA we hand back.
 *
 * Fallback: if VFIO isn't available (e.g. device not bound to
 * vfio-pci, `/dev/vfio/vfio` not accessible), fall back to anonymous
 * mmap with the VA reported as "DMA address". That's a lie — the GPU
 * can't reach it — but it lets `--probe` and `--vbios` work on
 * systems without full VFIO setup for read-only validation.
 */
static void *linux_gsp_dma_alloc(size_t size, size_t align, uint64_t *out_dma)
{
    if (g_vfio) {
        return vfio_dma_alloc(g_vfio, size, align, out_dma);
    }

    /* Fallback — not GPU-reachable, only useful for pure-CPU validation. */
    size_t alloc_size = (size + 4095) & ~(size_t)4095;
    if (align < 4096) align = 4096;
    void *p = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        if (out_dma) *out_dma = 0;
        return NULL;
    }
    if (out_dma) *out_dma = (uint64_t)(uintptr_t)p;
    (void)align;
    return p;
}

static void linux_gsp_dma_free(void *ptr, size_t size)
{
    if (!ptr) return;
    if (g_vfio) {
        vfio_dma_free(g_vfio, ptr, size);
        return;
    }
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
 * Four access paths, tried in order:
 *
 * 1. **BAR0 PROM window (NV_PROM_DATA)**. Authoritative. This is
 *    what openrm's `kgspExtractVbiosFromRom_TU102` uses and what
 *    the proprietary driver uses. BAR0 + 0x300000 is a 1 MB MMIO
 *    window that directly mirrors the GPU's SPI flash — bypassing
 *    the PCI Expansion ROM BAR entirely. On GA10x the Expansion
 *    ROM BAR caps at 512 KB but the VBIOS can be up to 1 MB; this
 *    path returns all of it. Requires BAR0 to be mapped (it
 *    already is for register access).
 *
 * 2. /dev/mem at the ROM BAR physical address. Legacy fallback.
 *    Returns up to 512 KB. Rarely enough on Ampere, but kept for
 *    systems where BAR0 PROM window isn't accessible.
 *
 * 3. VFIO ROM region. Uses `pci_map_rom()` under the hood —
 *    subject to the PCIR-LAST truncation.
 *
 * 4. sysfs `/sys/bus/pci/devices/<BDF>/rom`. Last resort; stops
 *    at PCIR LAST, usually ~149 KB on GA10x.
 */

#define PCI_CFG_COMMAND_OFF   0x04
#define PCI_CFG_ROM_BAR_OFF   0x30

/* NV_PROM_DATA — BAR0 offset that maps the GPU's SPI flash as a
 * 1 MB MMIO window. From NVIDIA open-gpu-kernel-modules'
 * `src/common/inc/swref/published/turing/tu102/dev_ext_devices.h`.
 * Used on Turing+ including all Ampere (the GA10x HAL doesn't
 * override VBIOS extraction — reuses the TU102 implementation). */
#define NV_PROM_DATA_OFFSET   0x00300000u
#define NV_PROM_DATA_SIZE     0x00100000u    /* 1 MB */

/*
 * Read the VBIOS from BAR0's PROM window. This is the authoritative
 * method the open-RM and proprietary drivers use — reads the actual
 * SPI flash contents regardless of the PCI Expansion ROM BAR size
 * advertised in config space. Returns byte count on success (up to
 * 1 MB), 0 if BAR0 isn't mapped, -1 on unexpected error.
 *
 * We use 32-bit reads because the PROM window is register-width —
 * a `memcpy` or byte-by-byte read would go through the same MMIO
 * path but wastes cycles. 32-bit reads match what openrm does.
 */
static ssize_t read_vbios_via_prom_window(uint8_t **out_buf)
{
    if (!g_bar0) return 0;
    /* BAR0 mapping must be large enough to cover the PROM window. */
    if (g_bar0_size < NV_PROM_DATA_OFFSET + NV_PROM_DATA_SIZE) {
        if (g_trace)
            fprintf(stderr, "[VBIOS] BAR0 too small (%zu < %u) for PROM window\n",
                    g_bar0_size, NV_PROM_DATA_OFFSET + NV_PROM_DATA_SIZE);
        return 0;
    }

    uint8_t *buf = malloc(NV_PROM_DATA_SIZE);
    if (!buf) return -1;

    const volatile uint32_t *prom =
        &g_bar0[NV_PROM_DATA_OFFSET / 4];
    uint32_t *dst = (uint32_t *)buf;
    for (size_t i = 0; i < NV_PROM_DATA_SIZE / 4; i++)
        dst[i] = prom[i];

    if (g_trace)
        fprintf(stderr, "[VBIOS] read 1 MB via BAR0 + 0x%x PROM window\n",
                NV_PROM_DATA_OFFSET);

    *out_buf = buf;
    return (ssize_t)NV_PROM_DATA_SIZE;
}

/*
 * Try /dev/mem at the ROM BAR's physical address. Reads PCI config
 * via sysfs (works even when vfio-pci is bound — sysfs config writes
 * are gated, but we read first to find the address, then write to
 * enable the BAR via VFIO config space if available).
 *
 * Returns positive byte count on success, 0 if path not viable
 * (no /dev/mem access, can't enable the BAR), -1 on failure.
 */
static ssize_t read_vbios_via_devmem(const char *pci_path,
                                     uint8_t **out_buf)
{
    char cfg_path[256];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config", pci_path);

    int cfg_fd = open(cfg_path, O_RDWR);
    if (cfg_fd < 0) {
        if (g_trace) fprintf(stderr, "[VBIOS] open %s: %s (try sudo)\n",
                             cfg_path, strerror(errno));
        return 0;
    }

    /* Read ROM_BAR (offset 0x30) and COMMAND (offset 0x04). */
    uint32_t saved_rom = 0, saved_cmd = 0;
    if (pread(cfg_fd, &saved_rom, 4, 0x30) != 4 ||
        pread(cfg_fd, &saved_cmd, 4, 0x04) != 4) {
        close(cfg_fd);
        return 0;
    }

    uint32_t rom_phys = saved_rom & 0xFFFFF800u;
    if (rom_phys == 0 || rom_phys == 0xFFFFF800u) {
        if (g_trace) fprintf(stderr, "[VBIOS] no ROM BAR address assigned\n");
        close(cfg_fd);
        return 0;
    }

    /* Probe BAR size: write all-1s, read back. The 0 in bit 0 keeps
     * ROM disabled during the probe. */
    uint32_t probe = 0xFFFFF800u;
    if (pwrite(cfg_fd, &probe, 4, 0x30) != 4) {
        close(cfg_fd);
        return 0;
    }
    uint32_t sized = 0;
    if (pread(cfg_fd, &sized, 4, 0x30) != 4) {
        close(cfg_fd);
        return 0;
    }
    uint32_t bar_size = ~(sized & 0xFFFFF800u) + 1;
    if (bar_size == 0 || bar_size > 16 * 1024 * 1024) {
        if (g_trace) fprintf(stderr, "[VBIOS] implausible BAR size %u\n", bar_size);
        /* Restore and bail. */
        ssize_t _r = pwrite(cfg_fd, &saved_rom, 4, 0x30);
        (void)_r;
        close(cfg_fd);
        return 0;
    }
    if (g_trace) fprintf(stderr, "[VBIOS] ROM BAR @ 0x%x size %u\n",
                         rom_phys, bar_size);

    /* Enable ROM (bit 0) + memory-space decoding. */
    uint32_t enabled_rom = rom_phys | 1u;
    uint32_t enabled_cmd = saved_cmd | 0x2u;
    if (pwrite(cfg_fd, &enabled_rom, 4, 0x30) != 4 ||
        pwrite(cfg_fd, &enabled_cmd, 4, 0x04) != 4) {
        if (g_trace) fprintf(stderr, "[VBIOS] cannot enable ROM BAR via /sys/.../config\n");
        ssize_t _r1 = pwrite(cfg_fd, &saved_rom, 4, 0x30);
        ssize_t _r2 = pwrite(cfg_fd, &saved_cmd, 4, 0x04);
        (void)_r1; (void)_r2;
        close(cfg_fd);
        return 0;
    }

    /* mmap /dev/mem at the ROM BAR physical address. */
    int mem_fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (mem_fd < 0) {
        if (g_trace) fprintf(stderr, "[VBIOS] /dev/mem: %s\n", strerror(errno));
        ssize_t _r1 = pwrite(cfg_fd, &saved_rom, 4, 0x30);
        ssize_t _r2 = pwrite(cfg_fd, &saved_cmd, 4, 0x04);
        (void)_r1; (void)_r2;
        close(cfg_fd);
        return 0;
    }

    void *mp = mmap(NULL, bar_size, PROT_READ, MAP_SHARED, mem_fd, (off_t)rom_phys);
    if (mp == MAP_FAILED) {
        if (g_trace) fprintf(stderr, "[VBIOS] mmap /dev/mem @0x%x: %s\n",
                             rom_phys, strerror(errno));
        close(mem_fd);
        ssize_t _r1 = pwrite(cfg_fd, &saved_rom, 4, 0x30);
        ssize_t _r2 = pwrite(cfg_fd, &saved_cmd, 4, 0x04);
        (void)_r1; (void)_r2;
        close(cfg_fd);
        return 0;
    }

    /* Copy via 4-byte MMIO reads to keep the bus access aligned. */
    uint8_t *buf = malloc(bar_size);
    if (buf) {
        const volatile uint32_t *src = (const volatile uint32_t *)mp;
        uint32_t *dst = (uint32_t *)buf;
        for (size_t i = 0; i < bar_size / 4; i++) dst[i] = src[i];
    }

    munmap(mp, bar_size);
    close(mem_fd);

    /* Restore ROM BAR + COMMAND. */
    ssize_t _r1 = pwrite(cfg_fd, &saved_rom, 4, 0x30);
    ssize_t _r2 = pwrite(cfg_fd, &saved_cmd, 4, 0x04);
    (void)_r1; (void)_r2;
    close(cfg_fd);

    if (!buf) return -1;
    *out_buf = buf;
    return (ssize_t)bar_size;
}

static int read_vfio_iommu_group(const char *pci_path)
{
    /* /sys/bus/pci/devices/<BDF>/iommu_group is a symlink to
     * /sys/kernel/iommu_groups/<id>. We just want the trailing id. */
    char link[256], target[256];
    snprintf(link, sizeof(link), "%s/iommu_group", pci_path);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) return -1;
    target[n] = '\0';
    char *slash = strrchr(target, '/');
    if (!slash) return -1;
    return atoi(slash + 1);
}

/*
 * Returns positive byte count of bytes read into *out_buf (caller
 * frees), 0 if VFIO isn't usable for this device, -1 on failure.
 */
static ssize_t read_vbios_via_vfio(const char *pci_path,
                                   uint8_t **out_buf)
{
    int group_id = read_vfio_iommu_group(pci_path);
    if (group_id < 0) {
        if (g_trace) fprintf(stderr, "[VBIOS] no IOMMU group — VFIO unusable\n");
        return 0;
    }

    int container = open("/dev/vfio/vfio", O_RDWR);
    if (container < 0) {
        if (g_trace) fprintf(stderr, "[VBIOS] /dev/vfio/vfio: %s\n", strerror(errno));
        return 0;
    }

    char gpath[64];
    snprintf(gpath, sizeof(gpath), "/dev/vfio/%d", group_id);
    int group = open(gpath, O_RDWR);
    if (group < 0) { close(container); return 0; }

    struct vfio_group_status gstat = { .argsz = sizeof(gstat) };
    if (ioctl(group, VFIO_GROUP_GET_STATUS, &gstat) < 0 ||
        !(gstat.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        close(group); close(container); return 0;
    }
    if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0 ||
        ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
        close(group); close(container); return 0;
    }

    /* The BDF VFIO needs is just the trailing component of pci_path. */
    const char *bdf = strrchr(pci_path, '/');
    bdf = bdf ? bdf + 1 : pci_path;
    int device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, bdf);
    if (device < 0) {
        fprintf(stderr, "[VBIOS] VFIO_GROUP_GET_DEVICE_FD %s: %s\n",
                bdf, strerror(errno));
        close(group); close(container); return 0;
    }

    /* Find config space region — its index is VFIO_PCI_CONFIG_REGION_INDEX (7). */
    struct vfio_region_info cfg = {
        .argsz = sizeof(cfg),
        .index = VFIO_PCI_CONFIG_REGION_INDEX,
    };
    if (ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &cfg) < 0) {
        close(device); close(group); close(container); return -1;
    }

    /* Save current ROM BAR + COMMAND, then enable. */
    uint32_t saved_rom = 0, saved_cmd = 0;
    if (pread(device, &saved_rom, 4, cfg.offset + PCI_CFG_ROM_BAR_OFF) != 4 ||
        pread(device, &saved_cmd, 4, cfg.offset + PCI_CFG_COMMAND_OFF) != 4) {
        close(device); close(group); close(container); return -1;
    }
    uint32_t enabled_rom = (saved_rom & 0xFFFFF800u) | 1u;
    uint32_t enabled_cmd = saved_cmd | 0x2u;        /* memory-space enable */
    if (pwrite(device, &enabled_rom, 4, cfg.offset + PCI_CFG_ROM_BAR_OFF) != 4 ||
        pwrite(device, &enabled_cmd, 4, cfg.offset + PCI_CFG_COMMAND_OFF) != 4) {
        close(device); close(group); close(container); return -1;
    }

    struct vfio_region_info rom = {
        .argsz = sizeof(rom),
        .index = VFIO_PCI_ROM_REGION_INDEX,
    };
    ssize_t n = -1;
    if (ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &rom) == 0 && rom.size > 0) {
        uint8_t *buf = malloc(rom.size);
        if (buf) {
            n = pread(device, buf, rom.size, rom.offset);
            if (n > 0) {
                *out_buf = buf;
            } else {
                free(buf);
                n = -1;
            }
        }
    }

    /* Restore — best-effort. The ssize_t captures suppress
     * -Wunused-result; gcc's `warn_unused_result` attribute on
     * pwrite ignores the (void) cast. */
    ssize_t _r1 = pwrite(device, &saved_rom, 4, cfg.offset + PCI_CFG_ROM_BAR_OFF);
    ssize_t _r2 = pwrite(device, &saved_cmd, 4, cfg.offset + PCI_CFG_COMMAND_OFF);
    (void)_r1; (void)_r2;
    close(device); close(group); close(container);
    return n;
}

static ssize_t read_vbios_via_sysfs(const char *pci_path,
                                    uint8_t **out_buf)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/rom", pci_path);

    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;

    if (write(fd, "1\n", 2) != 2) { close(fd); return -1; }
    if (lseek(fd, 0, SEEK_SET) < 0) { close(fd); return -1; }

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
        if (got < 0) { free(buf); close(fd); return -1; }
        if (got == 0) break;
        n += (size_t)got;
    }

    if (lseek(fd, 0, SEEK_SET) < 0) { /* ignored */ }
    ssize_t _disable_rc = write(fd, "0\n", 2);
    (void)_disable_rc;
    close(fd);
    *out_buf = buf;
    return (ssize_t)n;
}

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

    /* Try the BAR0 PROM window first — this is what the open-RM and
     * proprietary drivers use, and it's the only path that returns
     * the full SPI-flash contents on GA10x regardless of the
     * Expansion ROM BAR's advertised size. Fall back through the
     * legacy paths (/dev/mem, VFIO ROM region, sysfs ROM) for systems
     * where BAR0 PROM access isn't viable. */
    uint8_t *buf = NULL;
    ssize_t n = read_vbios_via_prom_window(&buf);
    const char *via = "BAR0+0x300000 PROM";
    if (n <= 0) {
        n = read_vbios_via_devmem(g_pci_path, &buf);
        via = "devmem";
    }
    if (n <= 0) {
        n = read_vbios_via_vfio(g_pci_path, &buf);
        via = "vfio";
    }
    if (n <= 0) {
        n = read_vbios_via_sysfs(g_pci_path, &buf);
        via = "sysfs";
    }
    if (n <= 0) {
        fprintf(stderr, "[VBIOS] all access paths failed\n");
        return -1;
    }
    if (n < 4) {
        fprintf(stderr, "[VBIOS] only %zd bytes via %s\n", n, via);
        free(buf);
        return -1;
    }

    if (nvidia_vbios_parse(buf, (size_t)n, &g_vbios) < 0) {
        fprintf(stderr, "[VBIOS] parse failed (%zd bytes via %s)\n", n, via);
        free(buf);
        return -1;
    }

    g_vbios_buf = buf;
    g_vbios_buf_size = (size_t)n;
    g_vbios_loaded = true;
    fprintf(stderr, "[VBIOS] parsed %zd bytes via %s, %u BIT entries\n",
            n, via, g_vbios.num_entries);

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

    /* Open a persistent VFIO session for DMA. Non-fatal on failure —
     * the harness is still useful for read-only work (--probe, --vbios,
     * --falcons) without DMA. Actions that require DMA will fail
     * explicitly when they try to allocate. */
    g_vfio = vfio_open(pci_path, trace);
    if (g_vfio) {
        fprintf(stderr, "[GSP-HARNESS] VFIO session open — DMA enabled\n");
    } else {
        fprintf(stderr, "[GSP-HARNESS] VFIO unavailable — DMA disabled "
                        "(read-only operations still work)\n");
    }

    if (load_firmware(chip) < 0) {
        fprintf(stderr, "[GSP-HARNESS] firmware load failed (chip=%s)\n", chip);
        return -1;
    }

    extern const struct gsp_platform_ops *gsp_platform;
    gsp_platform = &linux_ops;
    return 0;
}

/*
 * Expose the VFIO session for harness code paths that need its
 * state (the --dma-test action in main.c, future --bringup steps).
 */
struct vfio_session *linux_gsp_vfio_session(void)
{
    return g_vfio;
}
