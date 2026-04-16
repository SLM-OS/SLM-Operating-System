/*
 * gsp.h — NVIDIA GSP-RM bringup API (shared across Ampere platforms).
 *
 * Implements Phase E of docs/archive/plans/x86-64-capstone-gap-closure-plan.md
 * (and issue #28 for Jetson) — booting the RISC-V "GPU System
 * Processor" microcontroller so the GPU's compute engines become
 * usable for model inference.
 *
 * Architecture: a thin `struct gsp_platform_ops` vtable abstracts the
 * two things that differ between discrete PCIe (x86-64 + RTX 3050)
 * and integrated SoC (Jetson Orin Nano, GA10B):
 *
 *   - Register access — x86 reads via ioremap'd BAR0 through the PCI
 *     config space helpers; Jetson reads MMIO at a fixed physaddr.
 *   - DMA memory — x86 carves DMA-capable pages from PMM and uses
 *     the GPU's IOMMU-less direct DMA; Jetson has unified memory
 *     with cache maintenance (DC CVAC) via the existing `cache.c`
 *     helpers.
 *   - Firmware sourcing — x86 embeds the 4 firmware blobs at build
 *     time (extracted from /lib/firmware/); Jetson can load the
 *     same blobs from the rootfs at runtime.
 *   - VBIOS parsing — x86 reads the expansion ROM via PCI BAR;
 *     Jetson has no VBIOS (firmware runtime services come from QSPI
 *     via the pre-boot firmware).
 *
 * Everything else — the 7-phase boot sequence, FWSEC execution, WPR
 * setup, RISC-V bringup, RPC protocol, compute engine init — is
 * Ampere architecture and identical across platforms. That code
 * lives in the sibling .c files and calls out through this vtable.
 */

#ifndef GPU_NVIDIA_GSP_H
#define GPU_NVIDIA_GSP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- GSP-shared error codes ----
 *
 * Bare-metal C lacks <errno.h> (it's not a freestanding header — see
 * kernel/CLAUDE.md), so we publish a small set of named negative
 * constants for the GSP/Falcon/RPC code to return. Values match the
 * common Linux errno mapping for familiarity, all of which are
 * negative so any caller doing `if (rc < 0)` keeps working unchanged.
 *
 * Choose a code per failure mode rather than blanket -1; the harness
 * surfaces these via `b.last_error_phase` for hardware-debug triage
 * (a NOMEM at phase 102 vs a TIMEOUT at phase 106 are very different
 * stories for the same caller). */
#define GSP_OK              0
#define GSP_ERR_INVAL     (-22)   /* invalid argument or wrong state */
#define GSP_ERR_IO        (-5)    /* hardware fault — register reads garbage */
#define GSP_ERR_NOMEM     (-12)   /* DMA / shm allocation failed */
#define GSP_ERR_FAULT     (-14)   /* corrupt firmware / unparseable header */
#define GSP_ERR_NOSPC     (-28)   /* ring full / no headroom */
#define GSP_ERR_NOSYS     (-38)   /* feature not yet wired (e.g. RPC pre-init) */
#define GSP_ERR_TIMEOUT   (-110)  /* hardware poll exceeded budget */

/* ---- Firmware manifest ---- */

enum gsp_firmware_kind {
    GSP_FW_GSP          = 0,    /* gsp-535.113.01.bin (RISC-V ELF, ~38 MB) */
    GSP_FW_BOOTLOADER   = 1,    /* bootloader-535.113.01.bin (~20 KB)      */
    GSP_FW_BOOTER_LOAD  = 2,    /* booter_load-535.113.01.bin (~60 KB)     */
    GSP_FW_BOOTER_UNLOAD = 3,   /* booter_unload-535.113.01.bin (~40 KB)   */
    GSP_FW_KIND_COUNT,
};

struct gsp_firmware_blob {
    const uint8_t *data;
    size_t          size;
    const char     *version;    /* "535.113.01" — kept with the blob */
};

/* ---- Platform ops vtable ----
 *
 * Populated per-platform at GSP init. The shared boot/RPC/engine code
 * never reaches around this table.
 */
struct gsp_platform_ops {
    /*
     * BAR0 register access. Offsets are relative to the GPU's BAR0
     * base — identical between discrete (via PCI ECAM) and
     * integrated (via MMIO 0x17000000 on Jetson).
     *
     * **Implementations MUST use `volatile` accesses** when reading
     * and writing the MMIO region. The GPU's side has invisible
     * side-effects on every read (pops interrupt-source registers,
     * advances Falcon DMA state, etc.), and the compiler is
     * otherwise free to coalesce two reads of CPUCTL into one or
     * to hoist writes out of a polling loop. Every in-tree backend
     * today (`x86_gsp_bar0_read32` in `nvidia_gsp_platform.c`,
     * `linux_gsp_bar0_read32` in the gsp-harness linux_platform.c)
     * casts the mapped BAR0 pointer through `volatile uint32_t *`
     * for exactly this reason. A future backend that drops the
     * qualifier would introduce a hard-to-diagnose hang. (#163)
     *
     * read32/write32 MUST NOT cache. Writes must be serialized
     * against subsequent reads (x86: no-op; Jetson: DMB SY between
     * successive MMIO writes to distinct registers).
     */
    uint32_t (*read32)(uint32_t bar0_offset);
    void     (*write32)(uint32_t bar0_offset, uint32_t value);

    /*
     * BAR1 (VRAM) byte-level access. On x86-64 this is an MMIO
     * aperture (uncached, strongly-ordered). On Jetson this is a
     * window into unified memory; the platform is responsible for
     * any needed cache maintenance before returning.
     */
    void (*bar1_read)(uint32_t offset, void *dst, size_t n);
    void (*bar1_write)(uint32_t offset, const void *src, size_t n);

    /*
     * Allocate `size` bytes of DMA-accessible memory aligned to
     * `align`. Returns a CPU-addressable pointer; writes
     * `*out_dma_addr` with the physical address the GPU sees.
     *
     * On x86-64 the DMA address == physical address (IOMMU
     * passthrough). On Jetson the GPU uses SMMU-mapped stream IDs
     * — the platform may need to install a mapping and return the
     * IOVA. Shared code treats this as opaque.
     */
    void *(*dma_alloc)(size_t size, size_t align, uint64_t *out_dma_addr);
    void  (*dma_free)(void *ptr, size_t size);

    /*
     * Cache maintenance. `dma_alloc` returns nominally-coherent
     * memory on both platforms, but nouveau / GSP-RM occasionally
     * requires explicit writeback. x86 no-ops (coherent MMIO);
     * Jetson does DC CVAC on ARM64.
     */
    void (*cache_clean)(const void *addr, size_t size);
    void (*cache_invalidate)(void *addr, size_t size);

    /* Memory barrier (full). x86: `mfence`. Jetson: `dsb sy`. */
    void (*mb)(void);

    /*
     * Firmware accessor. Never NULL — on platforms that can't
     * supply a blob, returns {NULL, 0, NULL} and the bringup
     * aborts cleanly in Phase 0.
     */
    void (*firmware_get)(enum gsp_firmware_kind kind,
                         struct gsp_firmware_blob *out);

    /*
     * VBIOS FWSEC access (x86-64 only). Jetson's implementation
     * should set `*out_data = NULL` and return 0; the boot
     * sequence has a Jetson path that sources FWSEC-equivalent
     * setup from the bootloader instead.
     */
    int (*vbios_get_fwsec)(const void **out_data, size_t *out_size);
};

/* Current active platform ops. Set by the platform init before
 * calling gsp_init(); treated as read-only after. */
extern const struct gsp_platform_ops *gsp_platform;

/*
 * gsp_dma_alloc_checked — wrapper around `gsp_platform->dma_alloc`
 * that verifies the returned IOVA satisfies the requested alignment.
 *
 * Ampere's Falcon DMA silently truncates DMATRFBASE when bits 7:0 are
 * non-zero; the current Linux and PMM backends both return aligned
 * IOVAs, but the contract is not asserted. A future backend (mmap-
 * based test harness, hypervisor passthrough, etc.) that returns a
 * misaligned IOVA would produce a "Falcon ucode never halts" hang
 * with no obvious diagnostic. This helper turns that failure mode
 * into an early return with GSP_ERR_NOMEM, which the caller already
 * knows how to handle. (#170)
 *
 * Returns the virtual address on success, NULL on failure (including
 * alignment violation). Writes *out_iova on success.
 */
static inline void *gsp_dma_alloc_checked(size_t size, size_t align,
                                          uint64_t *out_iova)
{
    if (!gsp_platform || !gsp_platform->dma_alloc)
        return (void *)0;
    void *va = gsp_platform->dma_alloc(size, align, out_iova);
    if (!va) return (void *)0;
    if (out_iova && (*out_iova & (align - 1u))) {
        gsp_platform->dma_free(va, size);
        return (void *)0;
    }
    return va;
}

/* ---- Public boot API ---- */

enum gsp_state {
    GSP_STATE_UNINIT         = 0,
    GSP_STATE_FW_LOADED      = 1, /* Phase 0 done */
    GSP_STATE_FB_LAID_OUT    = 2, /* Phase 2 done */
    GSP_STATE_FWSEC_RUN      = 3, /* Phase 3 done */
    GSP_STATE_RISCV_RESET    = 4, /* Phase 4 done */
    GSP_STATE_MAILBOXES      = 5, /* Phase 5 done */
    GSP_STATE_BOOTING        = 6, /* Phase 6 in progress */
    GSP_STATE_RUNNING        = 7, /* Phase 7 done — GSP_INIT_DONE RPC received */
    GSP_STATE_FAILED         = 0xFF,
};

/*
 * Kick off the full 7-phase GSP-RM boot sequence. Returns 0 on
 * success (GSP-RM running, RPC channel open), negative errno on
 * any failure — with `gsp_last_error_phase()` identifying where.
 *
 * Callable from platform init after both gsp_platform and the
 * hardware (BAR mapped, GPU identified via BOOT_42) are ready.
 * Takes ~1 s on real hardware (nouveau's typical bringup time).
 */
int gsp_init(void);

/* Diagnostics. */
enum gsp_state gsp_get_state(void);
int            gsp_last_error_phase(void);

/* ---- Minimal compute submission (filled in by E5) ---- */

/*
 * Submit a FP32 matmul kernel. Returns 0 on successful submission
 * (the RPC was sent; use gsp_compute_sync() to wait). Blocks only
 * as long as the command ring is full.
 *
 * Inputs / output are VRAM offsets (relative to BAR1) because
 * that's where GSP-RM wants its operands. The runtime side is
 * responsible for DMA'ing tensor data in/out via `bar1_write` /
 * `bar1_read` before/after submission.
 */
int  gsp_compute_matmul_submit(uint64_t a_off, uint64_t b_off,
                               uint64_t c_off,
                               uint32_t m, uint32_t k, uint32_t n);
int  gsp_compute_sync(void);

#endif /* GPU_NVIDIA_GSP_H */
