/*
 * nvidia_vbios.c — Shared NVIDIA VBIOS / BIT table parser (E2).
 *
 * Runs on every platform that has GSP-RM bringup wired in: x86-64
 * bare-metal (via PCI expansion ROM), and the Linux userspace
 * harness (via /sys/bus/pci/.../rom). Jetson has no VBIOS in this
 * format — its FWSEC-equivalent setup comes from QSPI via the
 * pre-boot firmware and does not route through this parser.
 *
 * Tested against a real GP104 (GTX 1070) VBIOS dump via the
 * standalone test vector check in kernel/tests/test_nvidia_vbios.c.
 * FWSEC extraction (type 0x85) is Ampere-specific and will be
 * validated on test-pc via the gsp-harness once Linux is up.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "nvidia_vbios.h"

/* VBIOS layout constants — verified against a real VBIOS dump. */
#define PCI_ROM_SIG_0       0x55
#define PCI_ROM_SIG_1       0xAA
#define BIT_SIG_0           0xFF
#define BIT_SIG_1           0xB8
#define BIT_ID_STR          "BIT"   /* followed by NUL in the dump */

/* BIT header layout (12 bytes total for the versions we support):
 *   +0  u16  0xFFB8         signature
 *   +2  4B   "BIT\0"        identifier
 *   +6  u8   reserved / version
 *   +7  u8   reserved / version
 *   +8  u8   hdr_size
 *   +9  u8   entry_size
 *   +10 u8   num_entries
 *   +11 u8   checksum
 *
 * Each entry:
 *   +0  u8   id
 *   +1  u8   version
 *   +2  u16  data_length
 *   +4  u16  data_offset (within the full VBIOS image)
 */
#define BIT_HDR_SIZE_MIN    12
#define BIT_ENTRY_SIZE      6
#define BIT_MAX_ENTRIES     64      /* defensive cap */

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/*
 * Scan the VBIOS image for the BIT signature. BIT always appears
 * in the first 8 KB of a well-formed VBIOS, but different board
 * vendors place it at slightly different offsets, so we scan.
 */
static int find_bit_signature(const uint8_t *image, size_t image_size,
                              uint32_t *out_off)
{
    if (image_size < 64)
        return -1;
    /* Cap the search at 64 KB — BIT has never been observed past
     * that, and scanning the whole ROM wastes cycles on large
     * dumps. */
    size_t limit = image_size > 64 * 1024 ? 64 * 1024 : image_size;
    for (size_t i = 0; i + 8 < limit; i++) {
        if (image[i]   == BIT_SIG_0 &&
            image[i+1] == BIT_SIG_1 &&
            image[i+2] == 'B' &&
            image[i+3] == 'I' &&
            image[i+4] == 'T' &&
            image[i+5] == 0) {
            *out_off = (uint32_t)i;
            return 0;
        }
    }
    return -1;
}

int nvidia_vbios_parse(const uint8_t *image, size_t image_size,
                       struct nvidia_vbios *out)
{
    if (!image || !out) return -1;
    out->parsed_ok = false;

    if (image_size < 64 || image_size > NVIDIA_VBIOS_MAX_SIZE)
        return -1;

    /* PCI expansion ROM signature. Required before we trust any
     * downstream offsets. */
    if (image[0] != PCI_ROM_SIG_0 || image[1] != PCI_ROM_SIG_1)
        return -1;

    uint32_t bit_off = 0;
    if (find_bit_signature(image, image_size, &bit_off) < 0)
        return -1;

    /* BIT header must fit in the image with room for the claimed
     * number of entries. */
    if (bit_off + BIT_HDR_SIZE_MIN > image_size)
        return -1;

    uint8_t hdr_size    = image[bit_off + 8];
    uint8_t entry_size  = image[bit_off + 9];
    uint8_t num_entries = image[bit_off + 10];

    if (hdr_size    < BIT_HDR_SIZE_MIN) return -1;
    if (entry_size  != BIT_ENTRY_SIZE)  return -1;
    if (num_entries > BIT_MAX_ENTRIES)  return -1;

    uint32_t entries_end = bit_off + hdr_size + (uint32_t)num_entries * entry_size;
    if (entries_end > image_size)
        return -1;

    out->image       = image;
    out->image_size  = image_size;
    out->bit_offset  = bit_off;
    out->hdr_size    = hdr_size;
    out->entry_size  = entry_size;
    out->num_entries = num_entries;
    out->parsed_ok   = true;
    return 0;
}

int nvidia_vbios_find_entry(const struct nvidia_vbios *vb,
                            uint8_t id, int version,
                            uint32_t *out_data_off, uint32_t *out_data_len)
{
    if (!vb || !vb->parsed_ok) return -1;

    const uint8_t *image = vb->image;
    uint32_t entry_base = vb->bit_offset + vb->hdr_size;

    for (uint8_t i = 0; i < vb->num_entries; i++) {
        uint32_t e = entry_base + (uint32_t)i * vb->entry_size;
        uint8_t  eid = image[e];
        uint8_t  ever = image[e + 1];
        uint16_t elen = rd16(&image[e + 2]);
        uint16_t eoff = rd16(&image[e + 4]);

        if (eid != id) continue;
        if (version >= 0 && (int)ever != version) continue;

        /* Validate the referenced data is actually inside the image. */
        if ((uint32_t)eoff + elen > vb->image_size) return -1;

        if (out_data_off) *out_data_off = eoff;
        if (out_data_len) *out_data_len = elen;
        return 0;
    }
    return -1;
}

/*
 * FWSEC ucode discovery. ALWAYS RETURNS -1 ON CURRENT CARDS — the
 * "BIT id 0x85" path was a pre-release Turing artifact; production
 * Turing/Ampere VBIOSes carry FWSEC inside PMU ucode descriptors
 * reachable via the 'I' (init scripts) BIT entry, not as a top-level
 * BIT entry. Walking those descriptors is an E3 prereq — see the
 * tracking issue. Until that lands this function exists so the
 * vtable shape is complete and so callers see a clean -1 instead
 * of a link error.
 */
int nvidia_vbios_get_fwsec(const struct nvidia_vbios *vb,
                           const uint8_t **out_data, uint32_t *out_size)
{
    if (!vb || !vb->parsed_ok) return -1;

    /* Try the historic id 0x85 first — harmless lookup, returns -1
     * on every card we've validated. Kept so that if NVIDIA ever
     * ships a card that does use this id, it'll work without a
     * rebuild. */
    uint32_t data_off = 0, data_len = 0;
    if (nvidia_vbios_find_entry(vb, VBIOS_BIT_ID_FWSEC, -1,
                                &data_off, &data_len) == 0
        && data_len > 0) {
        if (out_data) *out_data = vb->image + data_off;
        if (out_size) *out_size = data_len;
        return 0;
    }

    /* Production Turing+ path — not yet implemented (E3 prereq).
     * Return -1 cleanly so callers can branch instead of crashing. */
    return -1;
}
