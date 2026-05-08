/*
 * hailo.h — Hailo-8/8L NPU driver interface.
 *
 * Mirrors the platform-shim pattern used by the NVIDIA GSP driver
 * (kernel/gpu/nvidia/gsp.h): a platform-independent core drives the
 * bringup, firmware load, and inference dispatch through a small
 * `hailo_platform_ops` vtable that each platform fills in. The only
 * live platform is the Pi 5 (`hailo_pi5.c` — PCIe via `pcie.h`,
 * DMA via PMM, cache ops via `kernel/gpu/cache.c`). QEMU, Jetson,
 * and x86-64 get the link-only stub in `hailo_stub.c`.
 *
 * The driver's three responsibilities:
 *   1. Probe: find the PCIe endpoint, map BAR0/BAR2/BAR4, read IDs.
 *   2. Boot: upload firmware via the ATR[0] window, poll boot_status
 *      and the ATR[1] "loaded" flag.
 *   3. Inference: (Phase 4+) parse .hef via nanopb, DMA weights,
 *      submit descriptors on the VDMA channel, wait for completion
 *      via MSI, read output tensors back.
 *
 * References (from ~/slmos-ref/):
 *   - hailo-driver-notes.md — annotated Linux driver
 *   - hailo-pcie-common.h / .c — device-independent constants
 *   - hailo-vdma-common.h / .c — descriptor ring + doorbell
 *   - hailo-fw-validation.h / .c — firmware header / validation
 *
 * Register layouts documented in hailo-driver-notes.md §3 (BAR0)
 * and §5 (BAR2 VDMA); do not duplicate them here beyond the handful
 * of offsets the core bringup code directly touches.
 */

#ifndef AI_ACCEL_HAILO_H
#define AI_ACCEL_HAILO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Error codes (negative like GSP/PCIe)                                        */
/* -------------------------------------------------------------------------- */

#define HAILO_OK                    0
#define HAILO_ERR_INVAL           (-1)
#define HAILO_ERR_NODEV           (-2)   /* no Hailo device on this platform */
#define HAILO_ERR_IO              (-3)   /* hardware fault / garbage reads */
#define HAILO_ERR_TIMEOUT         (-4)   /* boot / control poll expired */
#define HAILO_ERR_BAD_FIRMWARE    (-5)   /* malformed firmware blob */
#define HAILO_ERR_BAD_MODEL       (-6)   /* malformed .hef */
#define HAILO_ERR_NOMEM           (-7)
#define HAILO_ERR_UNSUPPORTED     (-8)

/* -------------------------------------------------------------------------- */
/* PCIe identifiers (~/slmos-ref/hailo/hailo-pcie-common.h:38-44)                */
/* -------------------------------------------------------------------------- */

#define HAILO_PCI_VENDOR_ID       0x1E60
#define HAILO_PCI_DEVICE_HAILO8   0x2864   /* Hailo-8 and Hailo-8L */

/* -------------------------------------------------------------------------- */
/* BAR indices (~/slmos-ref/hailo/hailo-pcie-common.h:26-28)                     */
/* -------------------------------------------------------------------------- */

#define HAILO_BAR_CONFIG          0   /* BAR0: PLDA bridge regs + ATRs */
#define HAILO_BAR_VDMA            2   /* BAR2: VDMA channel regs */
#define HAILO_BAR_FW_ACCESS       4   /* BAR4: ATR0-mapped SRAM window */

/* -------------------------------------------------------------------------- */
/* Key BAR0 register offsets (~/slmos-ref/derivatives/notes/hailo-driver-notes.md §3)         */
/* -------------------------------------------------------------------------- */

#define HAILO_REG_VENDOR              0x0098u
#define HAILO_REG_IMASK_HOST          0x0188u
#define HAILO_REG_ISTATUS_HOST        0x018Cu
#define HAILO_REG_SRC_IRQ_PER_CHAN    0x0400u
#define HAILO_REG_DST_IRQ_PER_CHAN    0x0500u

#define HAILO_ATR_BASE                0x0700u   /* ATR[0] */
#define HAILO_ATR_STRIDE              0x0020u   /* 32 B per ATR entry */
#define HAILO_ATR_COUNT               4         /* ATR[0..3] */

/* Within one ATR entry: */
#define HAILO_ATR_OFF_PARAM           0x00u
#define HAILO_ATR_OFF_SRC             0x04u
#define HAILO_ATR_OFF_TRSL_ADDR_LO    0x08u
#define HAILO_ATR_OFF_TRSL_ADDR_HI    0x0Cu
#define HAILO_ATR_OFF_TRSL_PARAM      0x10u

#define HAILO_ATR_PARAM_VALUE         0x17u     /* constant; combine with idx */
#define HAILO_ATR_TRSL_AXI            0x06u     /* AXI memory translation */
#define HAILO_ATR_TABLE_SIZE          0x1000u   /* 4 KB per translation */

/* Compose the atr_param register value for ATR entry `idx` (0..3).
 * Matches the upstream driver's `ATR_PARAM | (index << 12)` formula
 * — see hailo-pcie-common.c:75. Avoids open-coding the shift at
 * call sites and guards against future targeting of ATR[1..3]. */
#define HAILO_ATR_PARAM(idx)          (HAILO_ATR_PARAM_VALUE | ((uint32_t)(idx) << 12))

/*
 * ATR[1]'s trsl_addr_lo is repurposed as a "firmware loaded" flag.
 * FW writes PCIE_BLOCK_ADDRESS_ATR1 into it once bootstrap finishes.
 * (hailo-pcie-common.c:845)
 */
#define HAILO_ATR1_FW_LOADED_MAGIC    0x00200000u

/* Boot-status register (Hailo-8, per-board table in hailo-pcie-common.c:297).
 * Accessed through the ATR[0] window into device SRAM. */
#define HAILO_BOOT_STATUS_DEV_ADDR    0x000E0000u
#define  HAILO_BOOT_STATUS_UNINIT       0x00000001u
#define  HAILO_BOOT_STATUS_IN_BOOTLOADER 0x00000002u  /* observed value */

/* Firmware trigger doorbell (hailo-pcie-common.c:307: `raise_ready_offset`).
 * This lives in BAR4 space and writes 1 to start the boot. */
#define HAILO_FW_TRIGGER_OFFSET       0x1684u
#define HAILO_FW_TRIGGER_VALUE        0x00000001u

/* Control-channel doorbell masks (hailo-pcie-common.c:476). */
#define HAILO_CTRL_APP_CPU_MASK       0x00000001u
#define HAILO_CTRL_CORE_CPU_MASK      0x00000002u

/* ISTATUS_HOST SW-interrupt demux (hailo-pcie-common.h:71-75). */
#define HAILO_IRQ_FW_NOTIFICATION     (1u << 25)  /* 0x02 in high byte */
#define HAILO_IRQ_FW_CONTROL          (1u << 26)  /* 0x04 in high byte */
#define HAILO_IRQ_DRIVER_DOWN_ACK     (1u << 27)  /* 0x08 in high byte */

/* -------------------------------------------------------------------------- */
/* Firmware header — ~/slmos-ref/hailo/hailo-fw-validation.h                     */
/* -------------------------------------------------------------------------- */

#define HAILO_FW_MAGIC_HAILO8         0x1DD89DE0u
#define HAILO_FW_CODE_ALIGN           4u
#define HAILO_FW_MAX_CODE_SIZE        0x40000u   /* 256 KB — app firmware */
/* Core firmware max. Same value today, but Linux distinguishes
 * MAXIMUM_APP_FIRMWARE_CODE_SIZE and MAXIMUM_CORE_FIRMWARE_CODE_SIZE
 * — keeping them separate here makes future bump-one-not-the-other
 * changes safe. */
#define HAILO_FW_MAX_CORE_CODE_SIZE   0x40000u   /* 256 KB — core firmware */
#define HAILO_FW_MAX_CERT_KEY         0x1000u
#define HAILO_FW_MAX_CERT_CONTENT     0x1000u

/* Firmware header layout version the driver knows how to parse. A
 * future Hailo FW with header_version != 0 would have additional
 * or reordered fields; refuse to boot one rather than misinterpret. */
#define HAILO_FW_HEADER_VERSION_V0    0u

struct hailo_firmware_header {
    uint32_t magic;            /* HAILO_FW_MAGIC_HAILO8 */
    uint32_t header_version;   /* 0 for HAILO_FIRMWARE_HEADER_VERSION_INITIAL */
    uint32_t firmware_major;
    uint32_t firmware_minor;
    uint32_t firmware_revision;
    uint32_t code_size;
};

struct hailo_fw_cert_header {
    uint32_t key_size;
    uint32_t content_size;
};

/* -------------------------------------------------------------------------- */
/* Device-side load addresses (hailo-pcie-common.c:297-307, Hailo-8)          */
/* -------------------------------------------------------------------------- */

struct hailo_fw_addrs {
    uint32_t boot_fw_header;       /* = 0xE0030 */
    uint32_t boot_key_cert;        /* = 0xE0048 */
    uint32_t boot_cont_cert;       /* = 0xE0390 */
    uint32_t app_fw_code_ram_base; /* = 0x60000 */
    uint32_t core_code_ram_base;   /* = 0xC0000 */
    uint32_t core_fw_header;       /* = 0xA0000 */
    uint32_t raise_ready_offset;   /* = 0x1684 */
    uint32_t boot_status;          /* = 0xE0000 */
    uint32_t trigger_address;      /* = 0xE0980 */
};

/* The Hailo-8 constants, exported for consumers who want the table. */
extern const struct hailo_fw_addrs hailo_fw_addrs_hailo8;

/* -------------------------------------------------------------------------- */
/* Platform ops vtable                                                         */
/* -------------------------------------------------------------------------- */

/*
 * Populated by each platform before calling hailo_init(). The core
 * never touches hardware directly. Contract mirrors
 * `gsp_platform_ops` — see docs/nvidia-gsp.md §"Platform Shim
 * Contract" for the design rationale.
 *
 * All MMIO accessors take a BAR index and a byte offset. The
 * platform is responsible for mapping BARs to kernel virtual
 * addresses (typically via pcie_map_bar on Pi 5) and for the
 * correct volatile + barrier behaviour of the underlying read/write.
 */
struct hailo_platform_ops {
    const char *name;

    /* Returns HAILO_OK on success. The platform is expected to
     * have already discovered the device (via pcie_find_device or
     * equivalent) before calling hailo_init(). Failing to find the
     * device returns HAILO_ERR_NODEV from hailo_init(), not from
     * here — init() itself just sanity-checks ops. */
    int (*init)(void);

    /* Symmetric teardown; may be NULL. */
    void (*shutdown)(void);

    /* 32-bit register I/O. Offset is within the named BAR. */
    uint32_t (*read32)(uint8_t bar, uint32_t offset);
    void     (*write32)(uint8_t bar, uint32_t offset, uint32_t value);

    /* Multi-word copy into BAR4 (for firmware and control channel
     * writes via the ATR[0] window). Source is host memory; the
     * platform takes care of any unaligned head/tail handling. */
    void (*bar4_write)(uint32_t offset, const void *src, size_t n);
    void (*bar4_read)(uint32_t offset, void *dst, size_t n);

    /* DMA-capable buffer allocation. Returns the CPU VA and, in
     * *iova_out, the PCIe-side address the Hailo endpoint should
     * target. `align` typically 64 KB for descriptor rings.
     * Platform is responsible for cache sync on sync_* calls below.
     *
     * dma_free must be passed the same `size` and `align` the
     * allocation used — a buddy-allocator backend (Pi 5) rounds
     * `max(size, align)` up to a power-of-2 block, and needs both
     * values to reconstruct the block order on free. */
    void *(*dma_alloc)(size_t size, size_t align, uint64_t *iova_out);
    void  (*dma_free)(void *ptr, size_t size, size_t align);

    /* Optional: allocator variant that biases toward LOW physical
     * addresses. NULL on platforms where the bias doesn't matter
     * (e.g. coherent IOMMU systems where any address is reachable).
     * Caller picks which allocator to invoke per-allocation — there
     * is no hidden mode flag, so concurrent allocations from different
     * threads can each request their own bias safely.
     *
     * Use case: PCIe inbound translation windows on some platforms
     * only reach the bottom of physical RAM, so DMA buffers visible
     * to the device must land there. `dma_free` is shared with
     * dma_alloc — the buffer can be returned via the same free
     * regardless of which allocator produced it. */
    void *(*dma_alloc_low)(size_t size, size_t align, uint64_t *iova_out);

    /* Cache maintenance on DMA buffers. Safe no-op on coherent
     * platforms; real work on Pi 5. */
    void (*cache_clean)(const void *addr, size_t size);
    void (*cache_invalidate)(void *addr, size_t size);

    /* Memory barrier — ensures prior stores to device memory are
     * observable before the next MMIO write. Required (not NULL).
     * On ARM64 `dsb sy`; on platforms with a weaker default, a
     * full-system barrier. */
    void (*mb)(void);

    /* Microsecond-granularity delay used by boot poll loops. */
    void (*udelay)(uint32_t usec);

    /* Register `handler` for the device's MSI. Returns HAILO_OK or
     * a negative error. May be NULL on platforms where only probe
     * and boot matter (no runtime inference). */
    int (*register_irq)(void (*handler)(void *ctx), void *ctx);

    /* Bounce the PCIe device's power state. `state` matches the PCI
     * D-state encoding: 0 = D0 (active), 3 = D3hot (deep idle but
     * still on the bus). Linux's hailo_pcie does D0→D3hot at end of
     * boot, then D3hot→D0 on user open; this op exposes the same
     * capability so platform-independent code can replicate the
     * round-trip. May be NULL on platforms without PCI PM cap. */
    int (*set_power_state)(uint8_t state);
};

/* Installed by the platform before hailo_init(). */
extern const struct hailo_platform_ops *hailo_platform;

/* -------------------------------------------------------------------------- */
/* Device state                                                                */
/* -------------------------------------------------------------------------- */

enum hailo_state {
    HAILO_STATE_UNINIT         = 0,
    HAILO_STATE_PROBED         = 1,  /* IDs read, BARs mapped */
    HAILO_STATE_FIRMWARE_ARMED = 2,  /* FW image written, trigger not yet raised */
    HAILO_STATE_BOOTING        = 3,
    HAILO_STATE_RUNNING        = 4,  /* FW booted, control channel live */
    HAILO_STATE_FAILED         = 0xFF,
};

/* -------------------------------------------------------------------------- */
/* Public API — implemented in hailo_core.c                                    */
/* -------------------------------------------------------------------------- */

/*
 * Probe the device and read identifiers. Requires
 * hailo_platform->read32 to already route to BAR0 (i.e. the
 * platform init has mapped BAR0).
 *
 * On success writes the read-back vendor/device id to *out_vendor
 * and *out_device (either may be NULL). State advances to
 * HAILO_STATE_PROBED.
 */
int hailo_probe(uint16_t *out_vendor, uint16_t *out_device);

/* Accessors — do not require the device to be booted. */
enum hailo_state hailo_get_state(void);
const char      *hailo_state_str(enum hailo_state s);

/*
 * Read the Hailo-8 firmware version from the loaded boot ROM /
 * app FW. Only valid once the firmware is booted (state ==
 * HAILO_STATE_RUNNING). Writes major/minor/revision to the out
 * pointers; any may be NULL.
 *
 * Returns HAILO_ERR_NODEV if called before hailo_boot() succeeds.
 *
 * Lands in Phase 5 once the control channel is live; currently
 * returns HAILO_ERR_UNSUPPORTED as a placeholder.
 */
int hailo_get_firmware_version(uint32_t *out_major, uint32_t *out_minor,
                               uint32_t *out_revision);

/*
 * Write the boot firmware image and trigger boot.
 *
 * @fw_bytes, @fw_size: the raw `hailo8_fw.bin` file as shipped by
 *                      Hailo. Layout: [app_header, app_code,
 *                      cert_header, cert_key, cert_content,
 *                      core_header, core_code].
 * Returns HAILO_OK once the FW-loaded ATR[1] flag is observed, or
 * a negative error on timeout / validation failure.
 */
int hailo_boot(const void *fw_bytes, size_t fw_size);

/*
 * Validate a raw Hailo-8 firmware blob's outer headers without
 * writing anything to the device. Safe to call offline, which is
 * how the unit tests exercise the validator.
 *
 * Returns HAILO_OK if the blob has a well-formed app-FW header
 * (magic, version, code_size within limits). Deeper checks (CRC,
 * certificate format) are deferred to hailo_boot() itself.
 */
int hailo_validate_firmware(const void *fw_bytes, size_t fw_size);

/*
 * Core init — call from platform code after installing
 * hailo_platform. Validates the ops table and advances the state
 * machine from UNINIT to UNINIT-with-ops (no hardware access).
 */
int hailo_init(void);

#endif /* AI_ACCEL_HAILO_H */
