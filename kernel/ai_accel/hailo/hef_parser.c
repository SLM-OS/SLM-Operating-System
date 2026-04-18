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
 *   - first network group's op count and I/O pad shapes
 *
 * Deeper fields (per-context actions, weight ranges) will be added
 * by attaching more callbacks here. The pattern is stable: one
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
/* ProtoHEFTensorShape → ProtoHEFPad → ProtoHEFOp callback chain.              */
/*                                                                             */
/* Runs only inside the FIRST network group (gated by a flag in pad_ctx /      */
/* op_ctx — the caller sets it once). Each pad decoded into hef_info.pads[]    */
/* consumes one slot; once the array fills, pads_truncated is set and later    */
/* pads are skipped but still advanced past (nanopb still needs to read        */
/* their bytes to step past a length-delimited sub-message).                   */
/* -------------------------------------------------------------------------- */

/*
 * ProtoHEFTensorShape decode. Called once per pad that carries the
 * tensor_shape branch of the shape_info oneof. The callback fills
 * the six uint32 dims into the parent pad slot.
 */
struct tshape_ctx {
    struct hef_pad_info *pad;  /* destination slot (must be non-NULL) */
};

static bool decode_tensor_shape_cb(pb_istream_t *stream,
                                   const pb_field_t *field,
                                   void **arg)
{
    struct tshape_ctx *tctx = (struct tshape_ctx *)*arg;

    /* ProtoHEFPad.shape_info is a proto oneof (tensor_shape|nms_shape).
     * With FT_CALLBACK, both branches share the same union slot, so
     * THIS callback gets invoked for BOTH tags. We only populate the
     * tensor-shape dims when the tag actually is tensor_shape (6);
     * for nms_shape (7) or any future branch we just advance past
     * the sub-message and return OK. field->tag reflects the
     * specific oneof branch being decoded. */
    if (field->tag != ProtoHEFPad_tensor_shape_tag) {
        return pb_read(stream, NULL, stream->bytes_left);
    }

    /* pb_istream_t parsing runs per-field. We handle each expected
     * uint32 tag ourselves rather than declaring a nested struct,
     * because the parent ProtoHEFPad already consumed the length-
     * prefix — nanopb hands us `stream` already bounded to this
     * sub-message's body. Walking tags here keeps the call depth
     * one level shallower than a nested pb_decode. */
    while (stream->bytes_left > 0) {
        uint64_t tag = 0;
        if (!pb_decode_varint(stream, &tag)) return false;
        uint32_t field_no  = (uint32_t)(tag >> 3);
        uint32_t wire_type = (uint32_t)(tag & 0x7u);
        if (wire_type != 0) {
            /* Unknown / future field type — skip the remainder. */
            if (!pb_read(stream, NULL, stream->bytes_left)) return false;
            break;
        }
        uint64_t v = 0;
        if (!pb_decode_varint(stream, &v)) return false;
        if (v > UINT32_MAX) return false;
        uint32_t u = (uint32_t)v;
        switch (field_no) {
        case 1: tctx->pad->height          = u; break;
        case 2: tctx->pad->padded_height   = u; break;
        case 3: tctx->pad->width           = u; break;
        case 4: tctx->pad->padded_width    = u; break;
        case 5: tctx->pad->features        = u; break;
        case 6: tctx->pad->padded_features = u; break;
        default: break;   /* ignore unknown fields */
        }
    }
    tctx->pad->has_tensor_shape = true;
    return true;
}

/*
 * ProtoHEFPad decode. Invoked once per repeated input_pads[] /
 * output_pads[] entry inside a ProtoHEFOp. The caller's pad_ctx
 * carries the is_input flag so we don't need two callback bodies.
 *
 * If the pads[] slot array is already full, this still has to run
 * (so nanopb's stream pointer advances past the sub-message), but
 * writes nothing — pads_truncated gets set the first time a pad is
 * dropped.
 */
struct pad_ctx {
    struct hef_info *info;
    bool             is_input;
};

static bool decode_pad_cb(pb_istream_t *stream,
                          const pb_field_t *field,
                          void **arg)
{
    (void)field;
    struct pad_ctx *pctx = (struct pad_ctx *)*arg;
    struct hef_info *info = pctx->info;

    struct hef_pad_info  discard = { 0 };  /* throwaway slot for overflow */
    struct hef_pad_info *slot;
    bool capture;

    if (info->pad_count < HEF_PARSER_MAX_PADS) {
        slot = &info->pads[info->pad_count];
        memset(slot, 0, sizeof(*slot));
        slot->is_input = pctx->is_input;
        capture = true;
    } else {
        slot = &discard;
        info->pads_truncated = true;
        capture = false;
    }

    struct u32_ctx index_ctx = {
        .dst = &slot->index, .present = &(bool){ false },
    };
    struct string_ctx name_ctx = {
        .dst = slot->name, .cap = HEF_PARSER_MAX_PAD_NAME,
        .truncated = &info->string_truncated,
    };
    struct tshape_ctx shape_ctx = { .pad = slot };

    ProtoHEFPad pad = ProtoHEFPad_init_default;
    pad.index.funcs.decode = read_u32_cb;
    pad.index.arg          = &index_ctx;
    pad.name.funcs.decode  = read_string_cb;
    pad.name.arg           = &name_ctx;
    /* Oneof access: tensor_shape lives inside the shape_info union
     * alongside nms_shape. Nanopb shares the callback slot between
     * oneof members — decode_tensor_shape_cb filters by field->tag. */
    pad.shape_info.tensor_shape.funcs.decode = decode_tensor_shape_cb;
    pad.shape_info.tensor_shape.arg          = &shape_ctx;

    if (!pb_decode(stream, ProtoHEFPad_fields, &pad)) return false;

    if (capture) info->pad_count++;
    return true;
}

/*
 * ProtoHEFOp decode. Bumps op_count and wires pad callbacks for
 * both input_pads and output_pads. The two pad_ctx structs are
 * stack-local to this frame — nanopb has already returned from
 * pb_decode before the frame unwinds, so the pointers stay live
 * for the full sub-decode.
 */
struct op_ctx {
    struct hef_info *info;
};

static bool decode_op_cb(pb_istream_t *stream,
                         const pb_field_t *field,
                         void **arg)
{
    (void)field;
    struct op_ctx *octx = (struct op_ctx *)*arg;

    struct pad_ctx input_ctx  = { .info = octx->info, .is_input = true  };
    struct pad_ctx output_ctx = { .info = octx->info, .is_input = false };

    ProtoHEFOp op = ProtoHEFOp_init_default;
    op.input_pads.funcs.decode  = decode_pad_cb;
    op.input_pads.arg           = &input_ctx;
    op.output_pads.funcs.decode = decode_pad_cb;
    op.output_pads.arg          = &output_ctx;

    if (!pb_decode(stream, ProtoHEFOp_fields, &op)) return false;

    octx->info->op_count++;
    return true;
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
    bool first_ng = (ng->info->network_group_count == 0);

    ProtoHEFNetworkGroup grp = ProtoHEFNetworkGroup_init_default;
    struct string_ctx name_ctx = {
        .dst = ng->info->first_network_group,
        .cap = HEF_PARSER_MAX_STR,
        .truncated = &ng->info->string_truncated,
    };
    struct op_ctx op_ctx = { .info = ng->info };
    if (first_ng) {
        grp.network_group_name.funcs.decode = read_string_cb;
        grp.network_group_name.arg          = &name_ctx;
        grp.ops.funcs.decode                = decode_op_cb;
        grp.ops.arg                         = &op_ctx;
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
 * the 16 KB kernel stack. This parser's own callback chain adds 5
 * levels (Hef → NetworkGroup → Op → Pad → TensorShape); well-formed
 * Hailo-compiled HEFs have fixed structural depth in that range,
 * well within safe limits. Any future path that loads HEF blobs
 * from a network source must either parse into a bounded-depth
 * staging buffer first or grow the stack for the decode call.
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
