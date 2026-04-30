/*
 * gsp_wpr_meta.h — GspFwWprMeta struct definition.
 *
 * This is the 256-byte handoff structure between SLM-OS (acting as
 * the bootloader) and SEC2's HS booter. Booter reads the address of
 * this struct from the Falcon's MAILBOX0/1 registers, validates the
 * `magic` and `revision`, and uses the field values to:
 *
 *   - DMA the GSP-RM ELF (radix3-mapped) from sysmem into WPR2 FB
 *   - DMA the GSP-RM bootloader from sysmem into WPR2 FB
 *   - Find the FRTS region, the heap, and the WPR2 boundaries
 *
 * If MAGIC or REVISION mismatch, booter halts with an error code
 * in MAILBOX0 (no DMA happens). If the struct is all-zero (the
 * E3.4 placeholder), booter NULL-derefs `sysmemAddrOfRadix3Elf`
 * during the radix3 page walk and hangs forever — that's the
 * symptom Stage A unblocks by populating the bare-minimum fields
 * (magic, revision, fbSize, valid radix3 chain pointer, WPR2
 * boundaries).
 *
 * The struct layout MUST stay binary-compatible with NVIDIA's R535
 * `GspFwWprMeta` definition. Field order, sizes, padding, and the
 * union arrangement are all checked at compile time against
 * `_Static_assert` constants below — any drift fails the build.
 *
 * Reference: ../slmos-reference-cache/nouveau/nouveau-r535-nvrm-gsp.h:417-555 (verbatim
 * copy of NVIDIA's R535 OGKM source). MAGIC/REVISION at lines 557-559.
 */

#ifndef SLMOS_GSP_WPR_META_H
#define SLMOS_GSP_WPR_META_H

#include <stddef.h>
#include <stdint.h>

/*
 * Verbatim layout from nouveau-r535-nvrm-gsp.h. Field names and
 * order kept identical so a future maintainer cross-referencing
 * against the upstream definition can match line-for-line.
 *
 * `_Pragma`-pack is NOT used — every field is naturally aligned
 * (all u64s on 8-byte boundaries, the inner u16/u32 union packs
 * to 8). Compile-time offset asserts below catch any layout drift.
 *
 * Tagged so `struct GspFwWprMeta_s` is forward-declarable from
 * other headers without pulling in the full definition.
 */
typedef struct GspFwWprMeta_s {
    /* Magic. BL writes this; booter validates against
     * GSP_FW_WPR_META_MAGIC before doing anything else. */
    uint64_t magic;

    /* Revision number of Booter-BL-Sequencer handoff interface.
     * Bumped when this interface changes incompatibly OR when
     * GSP-RM ucode is revoked. */
    uint64_t revision;

    /* ---- Members regarding data in SYSMEM ---- */
    /* Consumed by Booter for DMA. */

    uint64_t sysmemAddrOfRadix3Elf;
    uint64_t sizeOfRadix3Elf;

    uint64_t sysmemAddrOfBootloader;
    uint64_t sizeOfBootloader;

    /* Offsets inside bootloader image needed by Booter. */
    uint64_t bootloaderCodeOffset;
    uint64_t bootloaderDataOffset;
    uint64_t bootloaderManifestOffset;

    union {
        /* Used only at initial boot. */
        struct {
            uint64_t sysmemAddrOfSignature;
            uint64_t sizeOfSignature;
        };
        /* Used at suspend/resume to read GspFwHeapFreeList.
         * Offset relative to GspFwWprMeta FBMEM PA (gspFwWprStart). */
        struct {
            uint32_t gspFwHeapFreeListWprOffset;
            uint32_t unused0;
            uint64_t unused1;
        };
    };

    /* ---- Members describing FB layout ---- */
    uint64_t gspFwRsvdStart;

    uint64_t nonWprHeapOffset;
    uint64_t nonWprHeapSize;

    uint64_t gspFwWprStart;

    /* GSP-RM uses to set up heap. */
    uint64_t gspFwHeapOffset;
    uint64_t gspFwHeapSize;

    /* BL uses to find ELF for jump. */
    uint64_t gspFwOffset;
    /* Size is sizeOfRadix3Elf above. */

    uint64_t bootBinOffset;
    /* Size is sizeOfBootloader above. */

    uint64_t frtsOffset;
    uint64_t frtsSize;

    uint64_t gspFwWprEnd;

    /* GSP-RM uses for fbRegionInfo. */
    uint64_t fbSize;

    /* ---- Other members ---- */

    uint64_t vgaWorkspaceOffset;
    uint64_t vgaWorkspaceSize;

    /* Boot count. Booter uses to decide whether to load firmware. */
    uint64_t bootCount;

    /* This union starts at an 8-byte boundary and packs the inner
     * struct's u32 fields naturally. Both branches occupy 32 bytes. */
    union {
        struct {
            /* Shared partition RPC memory (physical address). */
            uint64_t partitionRpcAddr;
            /* Offsets relative to partitionRpcAddr. */
            uint16_t partitionRpcRequestOffset;
            uint16_t partitionRpcReplyOffset;
            /* Code/data section offset+size. */
            uint32_t elfCodeOffset;
            uint32_t elfDataOffset;
            uint32_t elfCodeSize;
            uint32_t elfDataSize;
            /* Used during GSP-RM resume to check for revocation. */
            uint32_t lsUcodeVersion;
        };
        struct {
            /* Pad for partitionRpc* fields plus 4 bytes. */
            uint32_t partitionRpcPadding[4];
            /* CrashCat (contiguous) buffer size/location. */
            uint64_t sysmemAddrOfCrashReportQueue;
            uint32_t sizeOfCrashReportQueue;
            /* Pad for lsUcodeVersion. */
            uint32_t lsUcodeVersionPadding[1];
        };
    };

    /* Number of VF partitions allocating sub-heaps from the WPR heap.
     * Used during boot to ensure the heap is adequately sized. */
    uint8_t gspFwHeapVfPartitionCount;

    /* Pad to exactly 256 bytes. Can be repurposed as new fields
     * without bumping revision. Initialized to 0. */
    uint8_t padding[7];

    /* BL writes this for verification (i.e. Booter says OK to boot). */
    uint64_t verified;     /* 0x0 = unverified, 0xa0...a0 = verified */
} GspFwWprMeta;

/* Per nouveau-r535-nvrm-gsp.h:557. Booter rejects any other value
 * with an error code in MAILBOX0 — the BL/booter handshake's first
 * sanity check. */
#define GSP_FW_WPR_META_MAGIC      0xdc3aae21371a60b3ULL

/* Per nouveau-r535-nvrm-gsp.h:559. Bumped when the layout above
 * changes incompatibly OR when GSP-RM ucode is revoked. R535 is
 * the only revision SLM-OS targets. */
#define GSP_FW_WPR_META_REVISION   1ULL

/* `verified` field sentinel — booter writes this back to the BL
 * after a successful WPR2 lock. SLM-OS reads it as part of the
 * post-booter diagnostic. */
#define GSP_FW_WPR_META_VERIFIED   0xa0a0a0a0a0a0a0a0ULL

/* ---- Layout pinning ----
 *
 * Anything below that fails-build means the struct above no longer
 * matches the NVIDIA layout and the next hardware iteration will
 * either NULL-deref inside SEC2 or, worse, succeed-but-trash random
 * GSP-RM state. Pin every field offset that booter or SEC2 reads
 * directly. (Other fields that only GSP-RM-side code consumes are
 * pinned by sizeof; their byte offset is implicit.) */

_Static_assert(sizeof(GspFwWprMeta) == 256,
               "GspFwWprMeta must be exactly 256 bytes — booter ABI");

_Static_assert(offsetof(GspFwWprMeta, magic) == 0x00,
               "magic must be at offset 0 (first thing booter reads)");
_Static_assert(offsetof(GspFwWprMeta, revision) == 0x08,
               "revision pinned for booter validation");
_Static_assert(offsetof(GspFwWprMeta, sysmemAddrOfRadix3Elf) == 0x10,
               "sysmemAddrOfRadix3Elf — booter dereferences this for "
               "the radix3 page walk; misaligned offset = NULL deref");
_Static_assert(offsetof(GspFwWprMeta, sizeOfRadix3Elf) == 0x18,
               "sizeOfRadix3Elf paired with sysmemAddrOfRadix3Elf");
_Static_assert(offsetof(GspFwWprMeta, sysmemAddrOfBootloader) == 0x20,
               "sysmemAddrOfBootloader — booter DMA source");
_Static_assert(offsetof(GspFwWprMeta, gspFwRsvdStart) == 0x58,
               "gspFwRsvdStart — first FB-layout field after the union");
_Static_assert(offsetof(GspFwWprMeta, gspFwWprStart) == 0x70,
               "gspFwWprStart — booter sanity-checks WPR2 lo register");
_Static_assert(offsetof(GspFwWprMeta, gspFwWprEnd) == 0xa8,
               "gspFwWprEnd — booter sanity-checks WPR2 hi register");
_Static_assert(offsetof(GspFwWprMeta, fbSize) == 0xb0,
               "fbSize — booter validates FB-relative offsets against this");
_Static_assert(offsetof(GspFwWprMeta, bootCount) == 0xc8,
               "bootCount — pinned because the union starts immediately after");
_Static_assert(offsetof(GspFwWprMeta, gspFwHeapVfPartitionCount) == 0xf0,
               "gspFwHeapVfPartitionCount — first field after the union; "
               "wrong offset = union packing drifted");
_Static_assert(offsetof(GspFwWprMeta, verified) == 0xf8,
               "verified — last field, booter writes the sentinel back here");

/* ---- Stage A helpers (defined in bringup.c) ----
 *
 * Pure functions — no platform vtable, no GPU. Pulled out so
 * `host-tools/gsp-harness/test_bringup.c` can pin the field-write
 * logic without standing up a fake DMA stack. */

/*
 * Populate the bare-minimum WprMeta fields required for SEC2's HS
 * booter to perform validation without NULL-deref'ing the radix3
 * walk. Stage A goal: turn an infinite hang into a discrete
 * MAILBOX0 error code we can interpret.
 *
 *   - magic + revision: pass booter's first sanity check
 *   - sysmemAddrOfRadix3Elf + sizeOfRadix3Elf: non-NULL chain so
 *     the page walk doesn't fault
 *   - gspFwWprStart + gspFwWprEnd: from FWSEC-FRTS Phase 1's
 *     `wpr2_addr` / `wpr2_size`; booter cross-checks against the
 *     PFB MMU WPR2_LO/HI BAR0 registers
 *   - fbSize: lets booter validate FB-relative offsets
 *
 * Other fields (bootloader/signature/heap/partition-RPC) stay
 * zero. Booter is expected to reject the missing bootloader with
 * a specific status code, which IS the diagnostic.
 *
 * @meta is zeroed first then re-populated. NULL @meta is a no-op.
 */
void gsp_wpr_meta_populate_minimum(GspFwWprMeta *meta,
                                   uint64_t radix3_l0_iova,
                                   uint64_t radix3_elf_size,
                                   uint64_t wpr2_addr,
                                   uint64_t wpr2_size,
                                   uint64_t fb_size);

/*
 * Fill a 3-level radix3 page chain with single-entry mappings:
 *
 *     L0[0] = l1_iova
 *     L1[0] = l2_iova
 *     L2[0] = elf_iova
 *
 * Caller has already allocated three 4 KB DMA-mapped pages (one
 * per level) and a dummy ELF page, and obtained their IOVAs. The
 * helper writes only the first entry of each level — the rest
 * stays whatever the caller left there (zero, ideally). NULL
 * pointer in any page argument is a no-op for the entire call.
 *
 * Reference: nouveau `nvkm_gsp_radix3_sg`
 * (`../slmos-reference-cache/nouveau/nouveau-gsp-r535.c:1656-1713`). Single-entry
 * chain is the Stage A simplification — production GSP-RM ELF
 * spans many L2 pages.
 */
void gsp_radix3_fill_dummy_chain(uint64_t *l0_page, uint64_t l1_iova,
                                 uint64_t *l1_page, uint64_t l2_iova,
                                 uint64_t *l2_page, uint64_t elf_iova);

#endif /* SLMOS_GSP_WPR_META_H */
