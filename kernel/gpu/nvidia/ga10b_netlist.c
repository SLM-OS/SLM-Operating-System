/*
 * ga10b_netlist.c - GA10B NET?_img netlist container parser (#1088).
 *
 * See ga10b_netlist.h for the format and for which image nvgpu picks.
 * nvgpu trusts the image completely (netlist.c:612-622 dereferences
 * data_offset/data_size unchecked); this parser validates every
 * region against the blob size up front so a truncated or corrupt
 * image is rejected before any view is handed out.
 */

#include "ga10b_netlist.h"

#include "string.h"

#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "ga10b_netlist: typed views assume a little-endian CPU"
#endif

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int ga10b_netlist_open(struct ga10b_netlist *out, const void *blob, size_t size)
{
    if (out == nullptr) {
        return GA10B_NETLIST_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (blob == nullptr) {
        return GA10B_NETLIST_ERR_ARG;
    }

    const uint8_t *b = blob;
    if (size < sizeof(struct ga10b_netlist_image_header)) {
        return GA10B_NETLIST_ERR_TRUNCATED;
    }

    uint32_t version = rd32(b);
    uint32_t regions = rd32(b + 4);

    uint64_t table_end = (uint64_t)sizeof(struct ga10b_netlist_image_header) +
                         (uint64_t)regions * sizeof(struct ga10b_netlist_region);
    if (table_end > (uint64_t)size) {
        return GA10B_NETLIST_ERR_TRUNCATED;
    }

    for (uint32_t i = 0; i < regions; i++) {
        const uint8_t *r = b + sizeof(struct ga10b_netlist_image_header) +
                           (size_t)i * sizeof(struct ga10b_netlist_region);
        uint64_t dsize = rd32(r + 4);
        uint64_t doff = rd32(r + 8);
        if (doff + dsize > (uint64_t)size) {
            return GA10B_NETLIST_ERR_BOUNDS;
        }
    }

    out->data = b;
    out->size = size;
    out->version = version;
    out->region_count = regions;
    return GA10B_NETLIST_OK;
}

int ga10b_netlist_region_at(const struct ga10b_netlist *nl, uint32_t idx,
                            struct ga10b_netlist_region *out)
{
    if (nl == nullptr || out == nullptr || nl->data == nullptr ||
        idx >= nl->region_count) {
        return GA10B_NETLIST_ERR_ARG;
    }
    const uint8_t *r = nl->data + sizeof(struct ga10b_netlist_image_header) +
                       (size_t)idx * sizeof(struct ga10b_netlist_region);
    out->region_id = rd32(r);
    out->data_size = rd32(r + 4);
    out->data_offset = rd32(r + 8);
    return GA10B_NETLIST_OK;
}

int ga10b_netlist_find(const struct ga10b_netlist *nl, uint32_t region_id,
                       struct ga10b_netlist_blob_view *out)
{
    if (nl == nullptr || out == nullptr || nl->data == nullptr) {
        return GA10B_NETLIST_ERR_ARG;
    }
    out->data = nullptr;
    out->size = 0;

    bool found = false;
    for (uint32_t i = 0; i < nl->region_count; i++) {
        struct ga10b_netlist_region r;
        (void)ga10b_netlist_region_at(nl, i, &r);
        if (r.region_id == region_id) {
            /* Last occurrence wins (nvgpu overwrites per occurrence). */
            out->data = nl->data + r.data_offset;
            out->size = r.data_size;
            found = true;
        }
    }
    return found ? GA10B_NETLIST_OK : GA10B_NETLIST_ERR_NOT_FOUND;
}

/* Find + 4-byte alignment check shared by the typed views. */
static int find_aligned(const struct ga10b_netlist *nl, uint32_t region_id,
                        struct ga10b_netlist_blob_view *v)
{
    int rc = ga10b_netlist_find(nl, region_id, v);
    if (rc != GA10B_NETLIST_OK) {
        return rc;
    }
    if (((uintptr_t)v->data & 3u) != 0u) {
        return GA10B_NETLIST_ERR_ALIGN;
    }
    return GA10B_NETLIST_OK;
}

int ga10b_netlist_get_u32_list(const struct ga10b_netlist *nl, uint32_t region_id,
                               struct ga10b_netlist_u32_view *out)
{
    if (out == nullptr) {
        return GA10B_NETLIST_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    struct ga10b_netlist_blob_view v;
    int rc = find_aligned(nl, region_id, &v);
    if (rc != GA10B_NETLIST_OK) {
        return rc;
    }
    out->l = (const uint32_t *)(const void *)v.data;
    /* netlist.c:85-87: count = (len + sizeof(u32) - 1) / sizeof(u32). */
    out->count = (uint32_t)(((uint64_t)v.size + 3u) / 4u);
    out->size = v.size;
    return GA10B_NETLIST_OK;
}

int ga10b_netlist_get_av_list(const struct ga10b_netlist *nl, uint32_t region_id,
                              struct ga10b_netlist_av_view *out)
{
    if (out == nullptr) {
        return GA10B_NETLIST_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    struct ga10b_netlist_blob_view v;
    int rc = find_aligned(nl, region_id, &v);
    if (rc != GA10B_NETLIST_OK) {
        return rc;
    }
    out->l = (const struct ga10b_netlist_av *)(const void *)v.data;
    out->count = v.size / (uint32_t)sizeof(struct ga10b_netlist_av); /* netlist.c:100 */
    return GA10B_NETLIST_OK;
}

int ga10b_netlist_get_av64_list(const struct ga10b_netlist *nl, uint32_t region_id,
                                struct ga10b_netlist_av64_view *out)
{
    if (out == nullptr) {
        return GA10B_NETLIST_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    struct ga10b_netlist_blob_view v;
    int rc = find_aligned(nl, region_id, &v);
    if (rc != GA10B_NETLIST_OK) {
        return rc;
    }
    out->l = (const struct ga10b_netlist_av64 *)(const void *)v.data;
    out->count = v.size / (uint32_t)sizeof(struct ga10b_netlist_av64); /* netlist.c:113 */
    return GA10B_NETLIST_OK;
}

int ga10b_netlist_get_aiv_list(const struct ga10b_netlist *nl, uint32_t region_id,
                               struct ga10b_netlist_aiv_view *out)
{
    if (out == nullptr) {
        return GA10B_NETLIST_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    struct ga10b_netlist_blob_view v;
    int rc = find_aligned(nl, region_id, &v);
    if (rc != GA10B_NETLIST_OK) {
        return rc;
    }
    out->l = (const struct ga10b_netlist_aiv *)(const void *)v.data;
    out->count = v.size / (uint32_t)sizeof(struct ga10b_netlist_aiv); /* netlist.c:126 */
    return GA10B_NETLIST_OK;
}

int ga10b_netlist_get_u32(const struct ga10b_netlist *nl, uint32_t region_id,
                          uint32_t *out)
{
    if (out == nullptr) {
        return GA10B_NETLIST_ERR_ARG;
    }
    struct ga10b_netlist_blob_view v;
    int rc = ga10b_netlist_find(nl, region_id, &v);
    if (rc != GA10B_NETLIST_OK) {
        return rc;
    }
    if (v.size < 4u) {
        return GA10B_NETLIST_ERR_SIZE;
    }
    *out = rd32(v.data);
    return GA10B_NETLIST_OK;
}

const char *ga10b_netlist_region_name(uint32_t id)
{
    switch (id) {
    case 0:  return "FECS_UCODE_DATA";
    case 1:  return "FECS_UCODE_INST";
    case 2:  return "GPCCS_UCODE_DATA";
    case 3:  return "GPCCS_UCODE_INST";
    case 4:  return "SW_BUNDLE_INIT";
    case 5:  return "SW_CTX_LOAD";
    case 6:  return "SW_NON_CTX_LOAD";
    case 7:  return "SW_METHOD_INIT";
    case 8:  return "CTXREG_SYS";
    case 9:  return "CTXREG_GPC";
    case 10: return "CTXREG_TPC";
    case 11: return "CTXREG_ZCULL_GPC";
    case 12: return "CTXREG_PM_SYS";
    case 13: return "CTXREG_PM_GPC";
    case 14: return "CTXREG_PM_TPC";
    case 15: return "MAJORV";
    case 16: return "BUFFER_SIZE";
    case 17: return "CTXSW_REG_BASE_INDEX";
    case 18: return "NETLIST_NUM";
    case 19: return "CTXREG_PPC";
    case 20: return "CTXREG_PMPPC";
    case 21: return "NVPERF_CTXREG_SYS";
    case 22: return "NVPERF_FBP_CTXREGS";
    case 23: return "NVPERF_CTXREG_GPC";
    case 24: return "NVPERF_FBP_ROUTER";
    case 25: return "NVPERF_GPC_ROUTER";
    case 26: return "CTXREG_PMLTC";
    case 27: return "CTXREG_PMFBPA";
    case 28: return "SWVEIDBUNDLEINIT";
    case 29: return "NVPERF_SYS_ROUTER";
    case 30: return "NVPERF_PMA";
    case 31: return "CTXREG_PMROP";
    case 32: return "CTXREG_PMUCGPC";
    case 33: return "CTXREG_ETPC";
    case 34: return "SW_BUNDLE64_INIT";
    case 35: return "NVPERF_PMCAU";
    case 36: return "CTXREG_SYS_COMPUTE";
    case 37: return "CTXREG_SYS_GFX";
    case 38: return "CTXREG_GPC_COMPUTE";
    case 39: return "CTXREG_GPC_GFX";
    case 40: return "CTXREG_TPC_COMPUTE";
    case 41: return "CTXREG_TPC_GFX";
    case 42: return "CTXREG_PPC_COMPUTE";
    case 43: return "CTXREG_PPC_GFX";
    case 44: return "CTXREG_ETPC_COMPUTE";
    case 45: return "CTXREG_ETPC_GFX";
    case 48: return "SW_NON_CTX_LOCAL_COMPUTE_LOAD";
    case 49: return "SW_NON_CTX_LOCAL_GFX_LOAD";
    case 50: return "SW_NON_CTX_GLOBAL_COMPUTE_LOAD";
    case 51: return "SW_NON_CTX_GLOBAL_GFX_LOAD";
    case 52: return "NVPERF_SYS_CONTROL";
    case 53: return "NVPERF_FBP_CONTROL";
    case 54: return "NVPERF_GPC_CONTROL";
    case 55: return "NVPERF_PMA_CONTROL";
    case 57: return "CTXREG_LTS_BC";
    case 58: return "CTXREG_LTS_UC";
    default: return "?";
    }
}
