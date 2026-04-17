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

/*
 * Handoff location: 4 KB at the END of Jetson's NC memory region.
 * NC memory base = 0xBDE00000, size = 2 MB.
 * Handoff at 0xBDFFF000 (last 4 KB page).
 *
 * This address is:
 *   - Within SLM-OS's identity-mapped DRAM range
 *   - In the NC memory region (no cache coherency issues)
 *   - Above SLM-OS's NC allocator (bump allocator starts at base,
 *     grows up — 4 KB at the top is safe from collision)
 *   - Not in OP-TEE's carveout (0xBE000000+)
 */
#define GA10B_CHANNEL_HANDOFF_PHYS  0xBDFFF000ULL

/* Magic value to detect a valid handoff block. */
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

#endif /* GPU_NVIDIA_GA10B_CHANNEL_HANDOFF_H */
