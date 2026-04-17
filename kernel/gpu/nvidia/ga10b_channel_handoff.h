/*
 * ga10b_channel_handoff.h — Shared structure between the Linux-side
 * channel helper and SLM-OS for inheriting a GPU channel across kexec.
 *
 * The Linux helper (scripts/gpu-channel-helper.c) creates an nvgpu
 * channel, fills in this structure with the channel's physical
 * addresses, and writes it to a fixed DRAM location. SLM-OS reads
 * the structure after kexec and uses the pre-created channel to
 * submit pushbuffer methods.
 *
 * The handoff address is GA10B_CHANNEL_HANDOFF_PHYS — chosen to be
 * within Jetson's NC memory region so SLM-OS can read it without
 * cache coherency issues. The Linux helper writes it via /dev/mem.
 *
 * Wire format: all fields are little-endian uint32/uint64.
 */

#ifndef GPU_NVIDIA_GA10B_CHANNEL_HANDOFF_H
#define GPU_NVIDIA_GA10B_CHANNEL_HANDOFF_H

#include <stdint.h>

/* Magic value to detect a valid handoff block.
 *
 * Production handoff discovery: SLM-OS scans a physical-memory range
 * for this magic value (the Linux helper allocates the handoff block
 * via nvmap which places it somewhere unpredictable in the IOVMM
 * heap). See ga10b_find_handoff_in_range() for the scanner and
 * kernel/gpu/nvidia/ga10b_bringup.c phase 6 for the range used. */
#define GA10B_CHANNEL_HANDOFF_MAGIC 0x47505548U  /* "GPUH" */

/*
 * Channel handoff block — written by Linux, read by SLM-OS.
 *
 * All physical addresses are CPU-physical (identity-mapped on GA10B
 * with SMMU passthrough). GPU-virtual addresses are in the channel's
 * GMMU address space (set up by nvgpu).
 */
struct ga10b_channel_handoff {
    uint32_t magic;             /* GA10B_CHANNEL_HANDOFF_MAGIC */
    uint32_t version;           /* 1 for this layout */
    uint32_t channel_id;        /* nvgpu channel ID (0-511) */
    uint32_t tsg_id;            /* TSG ID the channel belongs to */

    /* USERD (User Submit Data) — where GP_PUT lives.
     * The PBDMA reads GP_PUT from this memory location (not a register).
     * SLM-OS writes GP_PUT here to submit GPFIFO entries. */
    uint64_t userd_phys;        /* physical address of USERD page */
    uint32_t userd_gp_put_offset; /* byte offset of GP_PUT within USERD */
    uint32_t userd_gp_get_offset; /* byte offset of GP_GET within USERD */

    /* GPFIFO ring — circular buffer of 8-byte entries.
     * Each entry points to a pushbuffer segment. */
    uint64_t gpfifo_phys;       /* physical address of GPFIFO ring */
    uint64_t gpfifo_gpu_va;     /* GPU VA of GPFIFO ring */
    uint32_t gpfifo_entries;    /* number of GPFIFO slots (power of 2) */
    uint32_t gpfifo_entry_size; /* bytes per entry (8 on Ampere) */

    /* Pushbuffer — pre-allocated DMA buffer for method data.
     * SLM-OS writes methods here, then adds a GPFIFO entry pointing
     * to this buffer. */
    uint64_t pushbuf_phys;      /* physical address */
    uint64_t pushbuf_gpu_va;    /* GPU VA */
    uint32_t pushbuf_size;      /* bytes (typically 4 KB - 64 KB) */
    uint32_t _pad0;

    /* Semaphore — GPU writes here on SEMAPHORE_RELEASE method.
     * SLM-OS polls this address to detect method completion. */
    uint64_t semaphore_phys;    /* physical address */
    uint64_t semaphore_gpu_va;  /* GPU VA */

    /* Instance block — the channel's GMMU root. */
    uint64_t inst_block_phys;   /* physical address (for diagnostics) */

    /* GP_PUT/GP_GET current values at handoff time. */
    uint32_t initial_gp_put;    /* GP_PUT value when helper wrote this */
    uint32_t initial_gp_get;    /* GP_GET value (should equal GP_PUT) */
};

/* Wire-format size is locked: both the Linux helper and SLM-OS
 * depend on this exact layout. Any struct reorder or field addition
 * breaks the handoff silently — the static_assert catches it at
 * compile time on both sides. */
_Static_assert(sizeof(struct ga10b_channel_handoff) == 112,
               "ga10b_channel_handoff layout changed — update Linux "
               "helper (scripts/gpu-channel-helper.c) and bump version");

/*
 * Validate a candidate handoff block. Returns 0 iff magic, version,
 * addresses (all non-null), and gpfifo_entries (non-zero power of two)
 * are all correct. Pure-logic — no MMIO, host-testable.
 */
int ga10b_validate_handoff(const struct ga10b_channel_handoff *h);

/*
 * Scan a physical-memory range for the handoff magic at the given
 * stride. Returns the address of the first match, or 0 if not found.
 * Production callers use the IOVMM heap range; host tests pass their
 * own range over a mocked buffer.
 */
uint64_t ga10b_find_handoff_in_range(uint64_t start, uint64_t end,
                                     uint64_t stride);

#endif /* GPU_NVIDIA_GA10B_CHANNEL_HANDOFF_H */
