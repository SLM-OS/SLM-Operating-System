/*
 * ga10b_ctxsw_ucode.c - FECS + GPCCS ctxsw ucode surface (#1088).
 *
 * Mirrors nvgpu_gr_falcon_init_ctxsw_ucode() (common/gr/gr_falcon.c:
 * 300-391, L4T R36.4.x). See ga10b_ctxsw_ucode.h.
 */

#include "ga10b_ctxsw_ucode.h"

#include "string.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1u) & ~(a - 1u);
}

/* nvgpu_gr_falcon_init_ctxsw_ucode_segment (gr_falcon.c:248-257). */
static void init_segment(struct ga10b_ctxsw_ucode_segment *seg,
                         uint64_t *offset, uint32_t size)
{
    seg->offset = (uint32_t)*offset;
    seg->size = size;
    *offset = align_up(*offset + size, GA10B_CTXSW_SEGMENT_ALIGN);
}

/* nvgpu_gr_falcon_init_ctxsw_ucode_segments (gr_falcon.c:259-274)
 * plus the boot-signature checksum from copy_ctxsw_ucode_segments
 * (gr_falcon.c:292-296; nvgpu_gr_checksum_u32 is a wrapping add,
 * gr_utils.c:32-35). */
static int init_segments(struct ga10b_ctxsw_ucode_segments *s, uint64_t *offset,
                         const uint8_t *fw, size_t fw_size,
                         uint32_t inst_size, uint32_t data_size)
{
    if (fw_size < sizeof(struct ga10b_ctxsw_bootloader_desc)) {
        return GA10B_CTXSW_ERR_FW_SIZE;
    }

    uint32_t desc_size = rd32(fw + 4);
    uint32_t imem_offset = rd32(fw + 8);
    uint32_t entry_point = rd32(fw + 12);

    /* boot_size = NVGPU_ALIGN(bootdesc->size, sizeof(u32)). nvgpu
     * copies that many bytes from fw + 16 without checking the file
     * length; reject a file that does not actually hold them. */
    uint64_t boot_size = align_up(desc_size, 4u);
    if (sizeof(struct ga10b_ctxsw_bootloader_desc) + boot_size > (uint64_t)fw_size) {
        return GA10B_CTXSW_ERR_FW_SIZE;
    }

    /* Netlist u32 count rounds up (netlist.c:85-87); size = count * 4. */
    uint64_t code_size = align_up(inst_size, 4u);
    uint64_t dsize = align_up(data_size, 4u);

    s->boot_entry = entry_point;
    s->boot_imem_offset = imem_offset;
    init_segment(&s->boot, offset, (uint32_t)boot_size);
    init_segment(&s->code, offset, (uint32_t)code_size);
    init_segment(&s->data, offset, (uint32_t)dsize);
    if (*offset > 0xFFFFFFFFull) {
        return GA10B_CTXSW_ERR_OVERFLOW;
    }

    uint32_t sig = 0;
    const uint8_t *boot = fw + sizeof(struct ga10b_ctxsw_bootloader_desc);
    for (uint32_t i = 0; i < s->boot.size / 4u; i++) {
        sig += rd32(boot + (size_t)i * 4u);
    }
    s->boot_signature = sig;
    return GA10B_CTXSW_OK;
}

int ga10b_ctxsw_ucode_inputs_from_netlist(const struct ga10b_netlist *nl,
                                          struct ga10b_ctxsw_ucode_inputs *in)
{
    if (nl == nullptr || in == nullptr) {
        return GA10B_CTXSW_ERR_ARG;
    }
    struct ga10b_netlist_blob_view fi, fd, gi, gd;
    if (ga10b_netlist_find(nl, GA10B_NETLIST_REGIONID_FECS_UCODE_INST, &fi) != 0 ||
        ga10b_netlist_find(nl, GA10B_NETLIST_REGIONID_FECS_UCODE_DATA, &fd) != 0 ||
        ga10b_netlist_find(nl, GA10B_NETLIST_REGIONID_GPCCS_UCODE_INST, &gi) != 0 ||
        ga10b_netlist_find(nl, GA10B_NETLIST_REGIONID_GPCCS_UCODE_DATA, &gd) != 0) {
        return GA10B_CTXSW_ERR_NETLIST;
    }
    in->fecs_inst = fi.data;  in->fecs_inst_size = fi.size;
    in->fecs_data = fd.data;  in->fecs_data_size = fd.size;
    in->gpccs_inst = gi.data; in->gpccs_inst_size = gi.size;
    in->gpccs_data = gd.data; in->gpccs_data_size = gd.size;
    return GA10B_CTXSW_OK;
}

int ga10b_ctxsw_ucode_layout(const struct ga10b_ctxsw_ucode_inputs *in,
                             struct ga10b_ctxsw_ucode_info *out)
{
    if (in == nullptr || out == nullptr) {
        return GA10B_CTXSW_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (in->fecs_fw == nullptr || in->gpccs_fw == nullptr) {
        return GA10B_CTXSW_ERR_ARG;
    }
    if ((in->fecs_inst == nullptr && in->fecs_inst_size != 0u) ||
        (in->fecs_data == nullptr && in->fecs_data_size != 0u) ||
        (in->gpccs_inst == nullptr && in->gpccs_inst_size != 0u) ||
        (in->gpccs_data == nullptr && in->gpccs_data_size != 0u)) {
        return GA10B_CTXSW_ERR_ARG;
    }

    /* FECS first, then GPCCS, sharing one running offset
     * (gr_falcon.c:339-351). */
    uint64_t offset = 0;
    int rc = init_segments(&out->fecs, &offset, in->fecs_fw, in->fecs_fw_size,
                           in->fecs_inst_size, in->fecs_data_size);
    if (rc == GA10B_CTXSW_OK) {
        rc = init_segments(&out->gpccs, &offset, in->gpccs_fw, in->gpccs_fw_size,
                           in->gpccs_inst_size, in->gpccs_data_size);
    }
    if (rc != GA10B_CTXSW_OK) {
        memset(out, 0, sizeof(*out));
        return rc;
    }
    out->surface_size = (uint32_t)offset;
    return GA10B_CTXSW_OK;
}

/* nvgpu_gr_falcon_copy_ctxsw_ucode_segments (gr_falcon.c:276-298):
 * write boot.size bytes of boot image, then the code and data lists.
 * The list buffers nvgpu writes are count*4 long but only hold `len`
 * real bytes (the rest is kzalloc zero), so copying `len` bytes into
 * the already-zeroed surface is byte-identical. */
static void copy_segments(uint8_t *dst, const struct ga10b_ctxsw_ucode_segments *s,
                          const uint8_t *fw,
                          const uint8_t *inst, uint32_t inst_size,
                          const uint8_t *data, uint32_t data_size)
{
    memcpy(dst + s->boot.offset, fw + sizeof(struct ga10b_ctxsw_bootloader_desc),
           s->boot.size);
    if (inst_size != 0u) {
        memcpy(dst + s->code.offset, inst, inst_size);
    }
    if (data_size != 0u) {
        memcpy(dst + s->data.offset, data, data_size);
    }
}

int ga10b_ctxsw_ucode_build(const struct ga10b_ctxsw_ucode_inputs *in,
                            void *buf, size_t buf_size,
                            struct ga10b_ctxsw_ucode_info *out)
{
    int rc = ga10b_ctxsw_ucode_layout(in, out);
    if (rc != GA10B_CTXSW_OK) {
        return rc;
    }
    if (buf == nullptr || buf_size < out->surface_size) {
        return GA10B_CTXSW_ERR_BUF_SMALL;
    }

    uint8_t *dst = buf;
    memset(dst, 0, out->surface_size);
    copy_segments(dst, &out->fecs, in->fecs_fw,
                  in->fecs_inst, in->fecs_inst_size,
                  in->fecs_data, in->fecs_data_size);
    copy_segments(dst, &out->gpccs, in->gpccs_fw,
                  in->gpccs_inst, in->gpccs_inst_size,
                  in->gpccs_data, in->gpccs_data_size);
    return GA10B_CTXSW_OK;
}
