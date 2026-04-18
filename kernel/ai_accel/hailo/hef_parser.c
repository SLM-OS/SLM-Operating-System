/*
 * hef_parser.c — nanopb-driven decode of the `.hef` protobuf body.
 *
 * Strategy
 * --------
 * hef.pb.c/h (generated from hef.proto by scripts/tools/
 * regen-hef-proto.sh) emits struct definitions where every field
 * is `pb_callback_t`. nanopb invokes our callbacks as it walks the
 * wire format; fields with a NULL callback are silently skipped
 * (the decoder still advances over their bytes — necessary because
 * length-delimited protobuf requires reading the length to step
 * past it). That lets us extract just the handful of fields the
 * kernel needs today without ever storing the ~45 variable-sized
 * repeated/bytes/string fields that a full decode would demand.
 *
 * Today the parser extracts:
 *   - header.hw_arch (uint32 / ProtoHEFHwArch enum)
 *   - header.sdk_version_str (string, truncated to HEF_PARSER_MAX_STR)
 *   - count of network_groups + name of the first one
 *
 * Deeper fields (ops, layers, weight ranges) will be added by
 * attaching more callbacks here. The pattern is stable: one
 * callback per field of interest, state passed through
 * pb_callback_t::arg.
 */

#include "hef_parser.h"
#include "hef.pb.h"
#include "pb_decode.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Callback helpers                                                            */
/* -------------------------------------------------------------------------- */

/*
 * Read a protobuf `string` field into a fixed-size char buffer,
 * appending a NUL. Truncates silently and flags the caller via
 * `*truncated`. nanopb passes the remaining bytes of the length-
 * delimited field in stream->bytes_left — the string is read
 * verbatim; it's not NUL-terminated on the wire.
 */
struct string_ctx {
    char    *dst;
    size_t   cap;         /* including the NUL */
    bool    *truncated;   /* set if stream content overflowed cap-1 */
};

static bool read_string_cb(pb_istream_t *stream,
                           const pb_field_t *field,
                           void **arg)
{
    (void)field;
    struct string_ctx *ctx = (struct string_ctx *)*arg;
    size_t remain = stream->bytes_left;
    size_t copy   = remain < (ctx->cap - 1) ? remain : (ctx->cap - 1);

    if (remain > ctx->cap - 1) {
        *ctx->truncated = true;
    }
    if (!pb_read(stream, (pb_byte_t *)ctx->dst, copy)) return false;
    /* Advance past any bytes we didn't copy. Chunked into a small
     * stack sink rather than one byte per pb_read call — matters
     * only for pathologically long strings (a well-formed HEF's
     * sdk_version is < 64 bytes), but a 1 MB truncated string
     * would be 16 384 iterations of the single-byte loop. */
    if (remain > copy) {
        pb_byte_t sink[64];
        size_t skip = remain - copy;
        while (skip > 0) {
            size_t chunk = skip < sizeof(sink) ? skip : sizeof(sink);
            if (!pb_read(stream, sink, chunk)) return false;
            skip -= chunk;
        }
    }
    ctx->dst[copy] = '\0';
    return true;
}

/*
 * Read a protobuf `uint32` field — scalar, not length-delimited
 * when the wire type is varint. nanopb calls `pb_decode_varint`
 * internally for us; the callback just stashes the value.
 */
struct u32_ctx {
    uint32_t *dst;
    bool     *present;
};

static bool read_u32_cb(pb_istream_t *stream,
                        const pb_field_t *field,
                        void **arg)
{
    (void)field;
    struct u32_ctx *ctx = (struct u32_ctx *)*arg;
    uint64_t v = 0;
    if (!pb_decode_varint(stream, &v)) return false;
    /* Reject out-of-range values rather than silently truncating.
     * All uint32-bound HEF fields (hw_arch enum, network_group_index,
     * etc.) fit well under UINT32_MAX; a larger value signals either
     * a corrupt blob or a schema-drift bug, both worth failing loud. */
    if (v > UINT32_MAX) return false;
    *ctx->dst = (uint32_t)v;
    *ctx->present = true;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Nested callbacks: ProtoHEFHeader decode                                     */
/* -------------------------------------------------------------------------- */

/*
 * Decode the ProtoHEFHeader sub-message attached to
 * ProtoHEFHef.header. Wires per-field callbacks for hw_arch and
 * sdk_version_str; other header fields fall through to nanopb's
 * default skip.
 */
struct header_ctx {
    struct hef_info *info;
};

static bool decode_header_cb(pb_istream_t *stream,
                             const pb_field_t *field,
                             void **arg)
{
    (void)field;
    struct header_ctx *hctx = (struct header_ctx *)*arg;

    struct u32_ctx hw_arch_ctx = {
        .dst = &hctx->info->hw_arch,
        .present = &hctx->info->hw_arch_known,
    };
    struct string_ctx sdk_ver_ctx = {
        .dst = hctx->info->sdk_version,
        .cap = HEF_PARSER_MAX_STR,
        .truncated = &hctx->info->string_truncated,
    };

    ProtoHEFHeader hdr = ProtoHEFHeader_init_default;
    hdr.hw_arch.funcs.decode = read_u32_cb;
    hdr.hw_arch.arg          = &hw_arch_ctx;
    hdr.sdk_version_str.funcs.decode = read_string_cb;
    hdr.sdk_version_str.arg          = &sdk_ver_ctx;

    return pb_decode(stream, ProtoHEFHeader_fields, &hdr);
}

/* -------------------------------------------------------------------------- */
/* Repeated network_groups counter + first-name capture                        */
/* -------------------------------------------------------------------------- */

/*
 * nanopb invokes this callback once per repeated ProtoHEFNetworkGroup
 * in the HEF. Each invocation's stream covers one instance; we bump
 * the counter and — only on the first one — capture network_group_name.
 */
struct ng_ctx {
    struct hef_info *info;
};

static bool decode_network_group_cb(pb_istream_t *stream,
                                    const pb_field_t *field,
                                    void **arg)
{
    (void)field;
    struct ng_ctx *ng = (struct ng_ctx *)*arg;
    bool capture_name = (ng->info->network_group_count == 0);

    ProtoHEFNetworkGroup grp = ProtoHEFNetworkGroup_init_default;
    struct string_ctx name_ctx = {
        .dst = ng->info->first_network_group,
        .cap = HEF_PARSER_MAX_STR,
        .truncated = &ng->info->string_truncated,
    };
    if (capture_name) {
        grp.network_group_name.funcs.decode = read_string_cb;
        grp.network_group_name.arg          = &name_ctx;
    }
    if (!pb_decode(stream, ProtoHEFNetworkGroup_fields, &grp)) return false;

    ng->info->network_group_count++;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

/*
 * Trust invariant: the `.hef` blob is assumed to come from a local
 * filesystem path that the caller controls (via `hailo load`). It is
 * NOT treated as untrusted input. Nanopb's default-skip recursion
 * into nested length-delimited sub-messages consumes one kernel-stack
 * frame per level, so an adversarially-nested proto could overflow
 * the 16 KB kernel stack. Well-formed Hailo-compiled HEFs have a
 * fixed structural depth (~5-6 levels), well within safe limits.
 * Any future path that loads HEF blobs from a network source must
 * either parse into a bounded-depth staging buffer first or grow the
 * stack for the decode call.
 */
int hef_parse_body(const void *blob, size_t size, struct hef_info *out)
{
    /* NULL inputs are caller bugs; a zero-length blob is a valid
     * (empty) protobuf that decodes to an all-default message. */
    if (!blob || !out) return HEF_PARSER_ERR_INVAL;

    memset(out, 0, sizeof(*out));

    struct header_ctx hctx = { .info = out };
    struct ng_ctx     ng   = { .info = out };

    ProtoHEFHef root = ProtoHEFHef_init_default;
    root.header.funcs.decode = decode_header_cb;
    root.header.arg          = &hctx;
    root.network_groups.funcs.decode = decode_network_group_cb;
    root.network_groups.arg          = &ng;

    pb_istream_t stream = pb_istream_from_buffer((const pb_byte_t *)blob, size);
    if (!pb_decode(&stream, ProtoHEFHef_fields, &root)) {
        return HEF_PARSER_ERR_DECODE;
    }
    return HEF_PARSER_OK;
}
