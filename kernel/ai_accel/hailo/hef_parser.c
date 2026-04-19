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

/* u64 counterpart for proto uint64 scalars (e.g. sequencer bitmap
 * fields active_sc_bitmap / active_l2_bitmap / l2_write_*). Same
 * stash-into-dst-and-flag-present shape as read_u32_cb. */
struct u64_ctx {
    uint64_t *dst;
    bool     *present;
};

static bool read_u64_cb(pb_istream_t *stream,
                        const pb_field_t *field,
                        void **arg)
{
    (void)field;
    struct u64_ctx *ctx = (struct u64_ctx *)*arg;
    uint64_t v = 0;
    if (!pb_decode_varint(stream, &v)) return false;
    *ctx->dst = v;
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
     * one level shallower than a nested pb_decode.
     *
     * Forward-compat: if Hailo adds new fields with any wire type,
     * we skip just that field's bytes and keep parsing. Known
     * varint fields land in the switch; unknown or future
     * non-varint fields fall through to the per-wire-type skip so
     * subsequent known fields are still recovered. */
    while (stream->bytes_left > 0) {
        uint64_t tag = 0;
        if (!pb_decode_varint(stream, &tag)) return false;
        uint32_t field_no  = (uint32_t)(tag >> 3);
        uint32_t wire_type = (uint32_t)(tag & 0x7u);

        if (wire_type == 0) {   /* varint: known TensorShape fields */
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
            default: break;   /* future varint field — ignore */
            }
            continue;
        }

        /* Skip one unknown field of any wire type, so known fields
         * appearing after it still get parsed. Wire type 3 / 4 are
         * deprecated proto2 groups; treat as malformed. */
        switch (wire_type) {
        case 1: {   /* 64-bit fixed */
            if (!pb_read(stream, NULL, 8)) return false;
            break;
        }
        case 2: {   /* length-delimited */
            uint64_t len = 0;
            if (!pb_decode_varint(stream, &len)) return false;
            if (len > stream->bytes_left) return false;
            if (!pb_read(stream, NULL, (size_t)len)) return false;
            break;
        }
        case 5: {   /* 32-bit fixed */
            if (!pb_read(stream, NULL, 4)) return false;
            break;
        }
        default:
            return false;   /* wire type 3/4 (groups) or garbage */
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
 * If the pads[] slot array is already full, the decode still has to
 * advance the stream past the sub-message but none of the pad
 * fields need to be recovered — so the overflow branch leaves every
 * callback NULL and relies on nanopb's default-skip. Avoids a
 * stack-local scratch pad_info and the wasted callback work.
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

    ProtoHEFPad pad = ProtoHEFPad_init_default;

    if (info->pad_count < HEF_PARSER_MAX_PADS) {
        struct hef_pad_info *slot = &info->pads[info->pad_count];
        memset(slot, 0, sizeof(*slot));
        slot->is_input = pctx->is_input;

        /* read_u32_cb expects a non-NULL `present` pointer even when
         * the caller doesn't care; provide a named dummy so the
         * lifetime is obvious (vs. a compound-literal address). */
        bool index_present = false;
        struct u32_ctx index_ctx = {
            .dst = &slot->index, .present = &index_present,
        };
        struct string_ctx name_ctx = {
            .dst = slot->name, .cap = HEF_PARSER_MAX_PAD_NAME,
            .truncated = &info->string_truncated,
        };
        struct tshape_ctx shape_ctx = { .pad = slot };

        pad.index.funcs.decode = read_u32_cb;
        pad.index.arg          = &index_ctx;
        pad.name.funcs.decode  = read_string_cb;
        pad.name.arg           = &name_ctx;
        /* Oneof access: tensor_shape lives inside the shape_info
         * union alongside nms_shape. Nanopb shares the callback slot
         * between oneof members — decode_tensor_shape_cb filters by
         * field->tag. */
        pad.shape_info.tensor_shape.funcs.decode = decode_tensor_shape_cb;
        pad.shape_info.tensor_shape.arg          = &shape_ctx;

        if (!pb_decode(stream, ProtoHEFPad_fields, &pad)) return false;
        info->pad_count++;
        return true;
    }

    /* Overflow: no callbacks wired, nanopb default-skips every
     * field and advances the stream. Still report OK so the parent
     * op decode completes cleanly. */
    info->pads_truncated = true;
    if (!pb_decode(stream, ProtoHEFPad_fields, &pad)) return false;
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
/* ProtoHEFActionWriteDataCcw → Action → Operation → PreliminaryConfig chain.  */
/*                                                                             */
/* Captures each WriteDataCcw action's (offset-in-blob, size,                  */
/* cfg_channel_index) triple into info->ccw_actions[]. Offset is measured      */
/* from the base of the HEF blob the caller passed to hef_parse_body —         */
/* derived from nanopb's pb_istream_t::state, which for pb_istream_from_buffer */
/* is an advancing pointer into the source buffer (see                         */
/* kernel/lib/nanopb/pb_decode.c:buf_read). The ccw_ctx chain threads          */
/* `blob_base` down from the top-level pb_decode call.                         */
/* -------------------------------------------------------------------------- */

struct ccw_ctx {
    struct hef_info     *info;
    const uint8_t       *blob_base;
    /* One running slot; the bytes callback fills data_offset_in_blob
     * and data_size, the varint callback fills cfg_channel_index, and
     * after both fire the action callback commits the slot. */
    struct hef_ccw_action pending;
    bool                  pending_has_data;   /* data field seen */
};

static bool ccw_data_cb(pb_istream_t *stream,
                        const pb_field_t *field,
                        void **arg)
{
    (void)field;
    struct ccw_ctx *cctx = (struct ccw_ctx *)*arg;

    /* nanopb has already read the length-delimited header; `stream`
     * now points at the first byte of the data blob and `bytes_left`
     * is the length of the blob. The underlying source is the HEF
     * blob passed in through hef_parse_body — `stream->state` is the
     * advancing pointer into it (see buf_read). */
    const uint8_t *data_ptr = (const uint8_t *)stream->state;
    size_t         data_len = stream->bytes_left;

    cctx->pending.data_offset_in_blob = (uint32_t)(data_ptr - cctx->blob_base);
    cctx->pending.data_size           = (uint32_t)data_len;
    cctx->pending_has_data            = true;

    /* Advance past the bytes without copying. pb_read with NULL buf
     * on a buf-backed stream just bumps state + decrements
     * bytes_left (pb_decode.c:90+). */
    return pb_read(stream, NULL, data_len);
}

static bool decode_write_data_ccw_ptr_cb(pb_istream_t *stream,
                                         const pb_field_t *field,
                                         void **arg);

static bool decode_write_data_ccw_cb(pb_istream_t *stream,
                                     const pb_field_t *field,
                                     void **arg)
{
    /* Nanopb shares the callback slot across every branch of
     * ProtoHEFAction's `action` oneof, so this one function runs
     * for every action-oneof tag on the wire. We handle two:
     *   - write_data_ccw      (v0/v1, inline payload bytes)
     *   - write_data_ccw_ptr  (v2+, payload in the separate CCWS block)
     * Every other branch (write_data, enable_lcu, debug, …) just
     * gets skipped past. */
    if (field->tag == ProtoHEFAction_write_data_ccw_ptr_tag) {
        return decode_write_data_ccw_ptr_cb(stream, field, arg);
    }
    if (field->tag != ProtoHEFAction_write_data_ccw_tag) {
        return pb_read(stream, NULL, stream->bytes_left);
    }

    struct ccw_ctx *cctx = (struct ccw_ctx *)*arg;

    /* Reset the pending slot before the sub-decode populates it. */
    memset(&cctx->pending, 0, sizeof(cctx->pending));
    cctx->pending_has_data = false;

    ProtoHEFActionWriteDataCcw act = ProtoHEFActionWriteDataCcw_init_default;
    bool cfg_present = false;
    struct u32_ctx cfg_ctx = {
        .dst     = &cctx->pending.cfg_channel_index,
        .present = &cfg_present,
    };
    act.data.funcs.decode              = ccw_data_cb;
    act.data.arg                       = cctx;
    act.cfg_channel_index.funcs.decode = read_u32_cb;
    act.cfg_channel_index.arg          = &cfg_ctx;

    if (!pb_decode(stream, ProtoHEFActionWriteDataCcw_fields, &act)) {
        return false;
    }
    cctx->pending.cfg_channel_index_known = cfg_present;

    /* Commit the pending slot. Only actions with data bytes count —
     * an action carrying just cfg_channel_index without data is
     * malformed (but nanopb already parsed it; we ignore quietly). */
    if (!cctx->pending_has_data) return true;

    if (cctx->info->ccw_action_count < HEF_PARSER_MAX_CCW_ACTIONS) {
        cctx->info->ccw_actions[cctx->info->ccw_action_count] = cctx->pending;
    } else {
        cctx->info->ccw_actions_truncated = true;
    }
    cctx->info->ccw_action_count++;
    cctx->info->ccw_total_bytes += cctx->pending.data_size;
    return true;
}

/*
 * v2+ variant: write_data_ccw_ptr. Instead of carrying payload bytes
 * inline (write_data_ccw field 3), the action carries a pointer into
 * the separate CCWS block that follows the proto body. Three scalars:
 *   uint64 offset — byte offset into the CCWS block
 *   uint32 size   — payload length
 *   uint32 cfg_channel_index
 *
 * We store offset into `data_offset_in_blob` (semantic is caller-side;
 * is_ccw_ptr=true tells the uploader to resolve against ccws_base
 * rather than blob_base). Offsets are capped at UINT32_MAX; larger
 * values would overflow our 32-bit storage but CCWS blocks are
 * bounded well under that in practice (the scheduler_mlp_pi5.hef
 * CCWS block is ~200 KB).
 */
static bool decode_write_data_ccw_ptr_cb(pb_istream_t *stream,
                                         const pb_field_t *field,
                                         void **arg)
{
    if (field->tag != ProtoHEFAction_write_data_ccw_ptr_tag) {
        return pb_read(stream, NULL, stream->bytes_left);
    }

    struct ccw_ctx *cctx = (struct ccw_ctx *)*arg;

    /* Parse the inline scalars ourselves — three varint/uint32 fields
     * which are simpler to walk directly than going through the nanopb
     * sub-decode + callback wiring dance. */
    memset(&cctx->pending, 0, sizeof(cctx->pending));
    cctx->pending.is_ccw_ptr = true;
    cctx->pending_has_data   = false;

    /* Protobuf wire types (spec §3.1). Nanopb exposes these via
     * PB_WT_* but only when the caller pulls in pb.h's internals;
     * spell them locally for readability. */
    enum {
        WT_VARINT = 0,    /* int32/64, uint32/64, bool, enum */
        WT_64BIT  = 1,    /* fixed64, sfixed64, double */
        WT_LEN    = 2,    /* length-delimited (string, bytes, sub-msg) */
        WT_32BIT  = 5,    /* fixed32, sfixed32, float */
    };

    while (stream->bytes_left > 0) {
        uint64_t tag = 0;
        if (!pb_decode_varint(stream, &tag)) return false;
        uint32_t field_no  = (uint32_t)(tag >> 3);
        uint32_t wire_type = (uint32_t)(tag & 0x7u);

        if (wire_type == WT_VARINT) {
            uint64_t v = 0;
            if (!pb_decode_varint(stream, &v)) return false;
            switch (field_no) {
            case 1:     /* offset (uint64) — clamp to u32 (see comment) */
                if (v > UINT32_MAX) return false;
                cctx->pending.data_offset_in_blob = (uint32_t)v;
                break;
            case 2:     /* size (uint32) */
                if (v > UINT32_MAX) return false;
                cctx->pending.data_size = (uint32_t)v;
                cctx->pending_has_data  = true;
                break;
            case 3:     /* cfg_channel_index (uint32) */
                if (v > UINT32_MAX) return false;
                cctx->pending.cfg_channel_index       = (uint32_t)v;
                cctx->pending.cfg_channel_index_known = true;
                break;
            default:
                break;
            }
            continue;
        }

        /* Skip unknown wire types by size. */
        switch (wire_type) {
        case WT_64BIT: if (!pb_read(stream, NULL, 8)) return false; break;
        case WT_LEN: {
            uint64_t len = 0;
            if (!pb_decode_varint(stream, &len)) return false;
            if (len > stream->bytes_left) return false;
            if (!pb_read(stream, NULL, (size_t)len)) return false;
            break;
        }
        case WT_32BIT: if (!pb_read(stream, NULL, 4)) return false; break;
        default: return false;
        }
    }

    /* Commit if size was set. Zero-size actions are skipped silently. */
    if (!cctx->pending_has_data || cctx->pending.data_size == 0) return true;

    if (cctx->info->ccw_action_count < HEF_PARSER_MAX_CCW_ACTIONS) {
        cctx->info->ccw_actions[cctx->info->ccw_action_count] = cctx->pending;
    } else {
        cctx->info->ccw_actions_truncated = true;
    }
    cctx->info->ccw_action_count++;
    cctx->info->ccw_total_bytes += cctx->pending.data_size;
    return true;
}

static bool decode_action_cb(pb_istream_t *stream,
                             const pb_field_t *field,
                             void **arg)
{
    (void)field;
    struct ccw_ctx *cctx = (struct ccw_ctx *)*arg;

    ProtoHEFAction act = ProtoHEFAction_init_default;
    /* The oneof's branches share a single callback slot (they're
     * aliased in the generated union). Wire one dispatcher; it
     * dispatches internally by field->tag. Writing to
     * write_data_ccw is sufficient to reach every branch because
     * all oneof entries alias the same pb_callback_t storage. */
    act.action.write_data_ccw.funcs.decode = decode_write_data_ccw_cb;
    act.action.write_data_ccw.arg          = cctx;
    return pb_decode(stream, ProtoHEFAction_fields, &act);
}

static bool decode_operation_cb(pb_istream_t *stream,
                                const pb_field_t *field,
                                void **arg)
{
    (void)field;
    struct ccw_ctx *cctx = (struct ccw_ctx *)*arg;

    ProtoHEFOperation op = ProtoHEFOperation_init_default;
    op.actions.funcs.decode = decode_action_cb;
    op.actions.arg          = cctx;
    return pb_decode(stream, ProtoHEFOperation_fields, &op);
}

static bool decode_preliminary_config_cb(pb_istream_t *stream,
                                         const pb_field_t *field,
                                         void **arg)
{
    (void)field;
    struct ccw_ctx *cctx = (struct ccw_ctx *)*arg;

    ProtoHEFPreliminaryConfig cfg = ProtoHEFPreliminaryConfig_init_default;
    cfg.operation.funcs.decode = decode_operation_cb;
    cfg.operation.arg          = cctx;
    return pb_decode(stream, ProtoHEFPreliminaryConfig_fields, &cfg);
}

/* -------------------------------------------------------------------------- */
/* Edge-layer decode chain (Phase 6.2b)                                        */
/*                                                                              */
/* Walks the first network group's contexts[].metadata.edge_layers[] to        */
/* capture per-pad quantization (qp_scale / qp_zp) and stream info              */
/* (sys_index, core_bytes_per_buffer, core_buffers_per_frame). Matches each    */
/* edge layer back to our already-decoded pads[] by pad_index, so the ops[]    */
/* walk must complete first — the outer network_group decode wires           */
/* contexts AFTER ops so this natural ordering is honored.                      */
/*                                                                              */
/* Protobuf doesn't guarantee field order on the wire, so each                 */
/* ProtoHEFEdgeLayer decode stages pad_index + quant + stream fields in a     */
/* local struct and commits to the matching pad_info only at end-of-message.  */
/* -------------------------------------------------------------------------- */

struct edge_layer_stage {
    bool     seen_pad_index;
    uint32_t pad_index;

    bool     seen_direction;
    uint32_t direction;                /* 0 = HOST_TO_DEVICE (input), 1 = D2H */

    bool     seen_quant;
    uint32_t qp_scale_raw;
    uint32_t qp_zp_raw;

    bool     seen_stream;
    bool     seen_sys_index;          /* f8 specifically — used as pad_key fallback */
    uint32_t sys_index;
    uint32_t core_bytes_per_buffer;
    uint32_t core_buffers_per_frame;

    /* Tensor shape (also in ProtoHEFEdgeLayerBase). DFC 3.33.1 leaves
     * ProtoHEFNetworkGroup.ops[] empty for simple MLPs, so pad entries
     * come out of edge_layers directly rather than being back-filled
     * from an ops-populated pad slot. */
    bool     seen_shape;
    uint32_t height;
    uint32_t padded_height;
    uint32_t width;
    uint32_t padded_width;
    uint32_t features;
    uint32_t padded_features;
};

/*
 * ProtoHEFEdgeLayerNumericInfo decode. Both qp_zp (tag 1) and
 * qp_scale (tag 2) are floats on the wire (wire type 5, 32-bit fixed,
 * little-endian IEEE-754). Store the raw 4 bytes as uint32_t without
 * any float arithmetic so this file can stay compiled with
 * -mgeneral-regs-only.
 */
static bool decode_numeric_info_cb(pb_istream_t *stream,
                                   const pb_field_t *field,
                                   void **arg)
{
    (void)field;
    struct edge_layer_stage *st = (struct edge_layer_stage *)*arg;

    while (stream->bytes_left > 0) {
        uint64_t tag = 0;
        if (!pb_decode_varint(stream, &tag)) return false;
        uint32_t field_no  = (uint32_t)(tag >> 3);
        uint32_t wire_type = (uint32_t)(tag & 0x7u);

        if (wire_type == 5) {                       /* 32-bit fixed (float) */
            uint8_t buf[4];
            if (!pb_read(stream, buf, 4)) return false;
            uint32_t v = (uint32_t)buf[0]
                       | ((uint32_t)buf[1] << 8)
                       | ((uint32_t)buf[2] << 16)
                       | ((uint32_t)buf[3] << 24);
            switch (field_no) {
            case 1: st->qp_zp_raw    = v; st->seen_quant = true; break;
            case 2: st->qp_scale_raw = v; st->seen_quant = true; break;
            default: break;                         /* future float — ignore */
            }
            continue;
        }

        /* Unknown / future fields — skip by wire type. */
        switch (wire_type) {
        case 0: { uint64_t _ = 0; if (!pb_decode_varint(stream, &_)) return false; break; }
        case 1: if (!pb_read(stream, NULL, 8)) return false; break;
        case 2: {
            uint64_t len = 0;
            if (!pb_decode_varint(stream, &len)) return false;
            if (len > stream->bytes_left) return false;
            if (!pb_read(stream, NULL, (size_t)len)) return false;
            break;
        }
        default: return false;
        }
    }
    return true;
}

/*
 * ProtoHEFEdgeLayerBase decode — extract the three stream-config
 * fields we care about (sys_index, core_bytes_per_buffer,
 * core_buffers_per_frame). All other fields skipped by wire type.
 */
static bool decode_edge_layer_base_cb(pb_istream_t *stream,
                                      const pb_field_t *field,
                                      void **arg)
{
    (void)field;
    struct edge_layer_stage *st = (struct edge_layer_stage *)*arg;

    while (stream->bytes_left > 0) {
        uint64_t tag = 0;
        if (!pb_decode_varint(stream, &tag)) return false;
        uint32_t field_no  = (uint32_t)(tag >> 3);
        uint32_t wire_type = (uint32_t)(tag & 0x7u);

        if (wire_type == 0) {
            uint64_t v = 0;
            if (!pb_decode_varint(stream, &v)) return false;
            if (v > UINT32_MAX) continue;
            uint32_t u = (uint32_t)v;
            switch (field_no) {
            /* Shape fields (primary source when ops[] is empty). */
            case 1: st->height          = u; st->seen_shape  = true; break;
            case 2: st->padded_height   = u; st->seen_shape  = true; break;
            case 3: st->width           = u; st->seen_shape  = true; break;
            case 4: st->padded_width    = u; st->seen_shape  = true; break;
            case 5: st->features        = u; st->seen_shape  = true; break;
            case 6: st->padded_features = u; st->seen_shape  = true; break;
            /* Stream config — already captured. */
            case 8:  st->sys_index              = u;
                     st->seen_stream = true; st->seen_sys_index = true; break;
            case 9:  st->core_bytes_per_buffer  = u; st->seen_stream = true; break;
            case 10: st->core_buffers_per_frame = u; st->seen_stream = true; break;
            default: break;
            }
            continue;
        }

        switch (wire_type) {
        case 1: if (!pb_read(stream, NULL, 8)) return false; break;
        case 2: {
            uint64_t len = 0;
            if (!pb_decode_varint(stream, &len)) return false;
            if (len > stream->bytes_left) return false;
            if (!pb_read(stream, NULL, (size_t)len)) return false;
            break;
        }
        case 5: if (!pb_read(stream, NULL, 4)) return false; break;
        default: return false;
        }
    }
    return true;
}

/*
 * ProtoHEFEdgeLayerInfo decode — wires the two sub-message callbacks
 * that actually fill the stage struct.
 */
static bool decode_edge_layer_info_cb(pb_istream_t *stream,
                                      const pb_field_t *field,
                                      void **arg)
{
    (void)field;
    struct edge_layer_stage *st = (struct edge_layer_stage *)*arg;

    ProtoHEFEdgeLayerInfo info = ProtoHEFEdgeLayerInfo_init_default;
    info.edge_layer_base.funcs.decode = decode_edge_layer_base_cb;
    info.edge_layer_base.arg          = st;
    info.numeric_info.funcs.decode    = decode_numeric_info_cb;
    info.numeric_info.arg             = st;
    return pb_decode(stream, ProtoHEFEdgeLayerInfo_fields, &info);
}

/*
 * ProtoHEFEdgeLayer.pad_index is `optional uint32`. Capture it into
 * the stage — order not guaranteed relative to the oneof decode.
 */
static bool decode_edge_pad_index_cb(pb_istream_t *stream,
                                     const pb_field_t *field,
                                     void **arg)
{
    (void)field;
    struct edge_layer_stage *st = (struct edge_layer_stage *)*arg;
    uint64_t v = 0;
    if (!pb_decode_varint(stream, &v)) return false;
    if (v > UINT32_MAX) return false;
    st->pad_index = (uint32_t)v;
    st->seen_pad_index = true;
    return true;
}

/* ProtoHEFEdgeLayer.direction — varint enum (0 = H2D/input, 1 = D2H). */
static bool decode_edge_direction_cb(pb_istream_t *stream,
                                     const pb_field_t *field,
                                     void **arg)
{
    (void)field;
    struct edge_layer_stage *st = (struct edge_layer_stage *)*arg;
    uint64_t v = 0;
    if (!pb_decode_varint(stream, &v)) return false;
    st->direction = (uint32_t)v;
    st->seen_direction = true;
    return true;
}

struct edge_ctx {
    struct hef_info *info;
};

/*
 * ProtoHEFEdgeLayer decode. Stages all fields into a local struct,
 * then commits to the matching pad in hef_info.pads[] (if any) by
 * pad_index. Edge layers that don't boundary-map to a known pad are
 * silently skipped — they describe intermediate/DDR connections we
 * don't route at the top level.
 */
static bool decode_edge_layer_cb(pb_istream_t *stream,
                                 const pb_field_t *field,
                                 void **arg)
{
    (void)field;
    struct edge_ctx *ectx = (struct edge_ctx *)*arg;
    struct edge_layer_stage stage = {0};

    ProtoHEFEdgeLayer el = ProtoHEFEdgeLayer_init_default;
    /* layer_info lives in the `edge` oneof union; nanopb uses a shared
     * callback slot for all oneof branches. Attach to layer_info — if
     * the encoded branch is layer_mux or layer_planes the callback
     * sees a zero-length stream and exits clean. */
    el.edge.layer_info.funcs.decode = decode_edge_layer_info_cb;
    el.edge.layer_info.arg          = &stage;
    el.pad_index.funcs.decode       = decode_edge_pad_index_cb;
    el.pad_index.arg                = &stage;
    el.direction.funcs.decode       = decode_edge_direction_cb;
    el.direction.arg                = &stage;
    if (!pb_decode(stream, ProtoHEFEdgeLayer_fields, &el)) return false;

    /* Derive a pad identity. DFC 3.33.1 simple-MLP HEFs don't emit
     * ProtoHEFEdgeLayer.pad_index on their boundary edge_layers (the
     * input layer is proto3-default-stripped; the output layer also
     * omits it). Fall back to the edge_layer's sys_index as the pad
     * key — every boundary edge_layer has one, and matches the data_id
     * the firmware uses for DMA routing.
     *
     * Require seen_sys_index (f8 specifically), not just seen_stream:
     * f9/f10 can be emitted without f8 in malformed input, leaving
     * sys_index=0 and collapsing distinct edge_layers onto pad_key=0.
     * If neither pad_index nor sys_index is present, skip this
     * edge_layer (non-boundary / intermediate). */
    uint32_t pad_key;
    if (stage.seen_pad_index) {
        pad_key = stage.pad_index;
    } else if (stage.seen_sys_index) {
        pad_key = stage.sys_index;
    } else {
        return true;                                /* non-boundary layer */
    }

    /* Find-or-create the pad. */
    struct hef_pad_info *p = NULL;
    for (uint32_t i = 0; i < ectx->info->pad_count; i++) {
        if (ectx->info->pads[i].index == pad_key) {
            p = &ectx->info->pads[i];
            break;
        }
    }
    if (!p) {
        if (ectx->info->pad_count >= HEF_PARSER_MAX_PADS) {
            ectx->info->pads_truncated = true;
            return true;
        }
        p = &ectx->info->pads[ectx->info->pad_count++];
        memset(p, 0, sizeof(*p));
        p->index = pad_key;
        /* direction defaults to HOST_TO_DEVICE (input = 0) when the
         * field is absent — proto3 doesn't emit zero-valued enums.
         * We flip to output only on an explicit direction==1. */
        p->is_input = !(stage.seen_direction && stage.direction == 1);
        if (stage.seen_shape) {
            p->has_tensor_shape = true;
            p->height           = stage.height;
            p->padded_height    = stage.padded_height;
            p->width            = stage.width;
            p->padded_width     = stage.padded_width;
            p->features         = stage.features;
            p->padded_features  = stage.padded_features;
        }
    } else if (!p->has_tensor_shape && stage.seen_shape) {
        /* Existing pad (from ops[]) without shape — fill it in. */
        p->has_tensor_shape = true;
        p->height           = stage.height;
        p->padded_height    = stage.padded_height;
        p->width            = stage.width;
        p->padded_width     = stage.padded_width;
        p->features         = stage.features;
        p->padded_features  = stage.padded_features;
    }

    if (stage.seen_quant) {
        p->has_quant_info = true;
        p->qp_scale_raw   = stage.qp_scale_raw;
        p->qp_zp_raw      = stage.qp_zp_raw;
    }
    if (stage.seen_stream) {
        p->has_stream_info       = true;
        p->sys_index             = stage.sys_index;
        p->core_bytes_per_buffer = stage.core_bytes_per_buffer;
        p->core_buffers_per_frame= stage.core_buffers_per_frame;
    }
    return true;
}

static bool decode_context_metadata_cb(pb_istream_t *stream,
                                       const pb_field_t *field,
                                       void **arg)
{
    (void)field;
    struct edge_ctx *ectx = (struct edge_ctx *)*arg;

    ProtoHEFContextMetadata md = ProtoHEFContextMetadata_init_default;
    md.edge_layers.funcs.decode = decode_edge_layer_cb;
    md.edge_layers.arg          = ectx;
    return pb_decode(stream, ProtoHEFContextMetadata_fields, &md);
}

/* -------------------------------------------------------------------------- */
/* Phase 6.4e: per-context compute-action capture                              */
/*                                                                              */
/* Walks contexts[].operations[].actions[] and records the oneof-tag of each   */
/* action into info->context_actions[]. The dispatcher callback runs once per  */
/* action and — since the oneof branches share a single callback slot —        */
/* sees field->tag set to the active branch. That tag is exactly the proto    */
/* field number a translator needs to pick the corresponding                   */
/* CONTEXT_SWITCH_DEFS__* wire action.                                          */
/*                                                                              */
/* This is deliberately narrow: we capture "which action kinds, in what       */
/* order" per context, not the full per-action parameter body. The             */
/* translator (6.4f) builds on top of this by adding per-type field extraction */
/* once the minimum-viable HEF→wire mapping is understood on hardware.         */
/* -------------------------------------------------------------------------- */

struct ctx_actions_accum {
    struct hef_info *info;
    struct hef_context_actions *current;    /* NULL if current context overflowed */
};

/* Decode a ProtoHEFActionEnableLcu sub-message and capture its six
 * scalar fields into hef_info.enable_lcu_actions[]. Called from the
 * oneof inner callback when the enable_lcu branch (tag 8) fires.
 *
 * Returns true on success (parse advanced past the sub-message) and
 * false on decode error. Sub-message overflow relative to
 * HEF_PARSER_MAX_ENABLE_LCU_ACTIONS sets enable_lcu_truncated but
 * still consumes the bytes — parsing continues for the rest of the
 * context.
 */
static bool decode_enable_lcu_body(pb_istream_t *stream,
                                   struct ctx_actions_accum *acc)
{
    /* Stage the six fields + presence flags. All fields are uint32
     * varints per hef.proto:758-777. */
    struct hef_enable_lcu_action out;
    memset(&out, 0, sizeof(out));
    out.context_index = acc->current ? acc->current->context_index : 0;

    bool present_discard = false;  /* shared dummy for fields without
                                      an independent "was this field
                                      set?" check — EnableLcu scalars
                                      default-to-zero is fine. */

    struct u32_ctx lcu_idx_ctx   = { .dst = &out.lcu_index,
                                     .present = &present_discard };
    struct u32_ctx cluster_ctx   = { .dst = &out.cluster_index,
                                     .present = &present_discard };
    struct u32_ctx done_addr_ctx = { .dst = &out.lcu_kernel_done_address,
                                     .present = &present_discard };
    struct u32_ctx done_cnt_ctx  = { .dst = &out.lcu_kernel_done_count,
                                     .present = &present_discard };
    struct u32_ctx enable_ctx    = { .dst = &out.lcu_enable_address,
                                     .present = &present_discard };
    struct u32_ctx net_idx_ctx   = { .dst = &out.network_index,
                                     .present = &present_discard };

    ProtoHEFActionEnableLcu sub = ProtoHEFActionEnableLcu_init_default;
    sub.lcu_index.funcs.decode               = read_u32_cb;
    sub.lcu_index.arg                        = &lcu_idx_ctx;
    sub.cluster_index.funcs.decode           = read_u32_cb;
    sub.cluster_index.arg                    = &cluster_ctx;
    sub.lcu_kernel_done_address.funcs.decode = read_u32_cb;
    sub.lcu_kernel_done_address.arg          = &done_addr_ctx;
    sub.lcu_kernel_done_count.funcs.decode   = read_u32_cb;
    sub.lcu_kernel_done_count.arg            = &done_cnt_ctx;
    sub.lcu_enable_address.funcs.decode      = read_u32_cb;
    sub.lcu_enable_address.arg               = &enable_ctx;
    sub.network_index.funcs.decode           = read_u32_cb;
    sub.network_index.arg                    = &net_idx_ctx;

    if (!pb_decode(stream, ProtoHEFActionEnableLcu_fields, &sub)) return false;

    if (acc->info->enable_lcu_count < HEF_PARSER_MAX_ENABLE_LCU_ACTIONS) {
        acc->info->enable_lcu_actions[acc->info->enable_lcu_count] = out;
        acc->info->enable_lcu_count++;
    } else {
        acc->info->enable_lcu_truncated = true;
    }
    return true;
}

/* ProtoHEFActionDisableLcu: lcu_index + cluster_index +
 * lcu_enable_address. Body size 1 byte on the wire (packed_lcu_id
 * only). */
static bool decode_disable_lcu_body(pb_istream_t *stream,
                                    struct ctx_actions_accum *acc)
{
    struct hef_disable_lcu_action out;
    memset(&out, 0, sizeof(out));
    out.context_index = acc->current ? acc->current->context_index : 0;

    bool present_discard = false;
    struct u32_ctx lcu_idx_ctx   = { .dst = &out.lcu_index,          .present = &present_discard };
    struct u32_ctx cluster_ctx   = { .dst = &out.cluster_index,      .present = &present_discard };
    struct u32_ctx enable_ctx    = { .dst = &out.lcu_enable_address, .present = &present_discard };

    ProtoHEFActionDisableLcu sub = ProtoHEFActionDisableLcu_init_default;
    sub.lcu_index.funcs.decode          = read_u32_cb;
    sub.lcu_index.arg                   = &lcu_idx_ctx;
    sub.cluster_index.funcs.decode      = read_u32_cb;
    sub.cluster_index.arg               = &cluster_ctx;
    sub.lcu_enable_address.funcs.decode = read_u32_cb;
    sub.lcu_enable_address.arg          = &enable_ctx;

    if (!pb_decode(stream, ProtoHEFActionDisableLcu_fields, &sub)) return false;

    if (acc->info->disable_lcu_count < HEF_PARSER_MAX_DISABLE_LCU_ACTIONS) {
        acc->info->disable_lcu_actions[acc->info->disable_lcu_count] = out;
        acc->info->disable_lcu_count++;
    } else {
        acc->info->disable_lcu_truncated = true;
    }
    return true;
}

/* ProtoHEFActionWaitForSequencer: cluster_index only. Maps to
 * SEQUENCER_DONE_INTERRUPT (1-byte body = sequencer_index). */
static bool decode_wait_sequencer_body(pb_istream_t *stream,
                                       struct ctx_actions_accum *acc)
{
    struct hef_wait_sequencer_action out;
    memset(&out, 0, sizeof(out));
    out.context_index = acc->current ? acc->current->context_index : 0;

    bool present_discard = false;
    struct u32_ctx cluster_ctx = { .dst = &out.cluster_index, .present = &present_discard };

    ProtoHEFActionWaitForSequencer sub = ProtoHEFActionWaitForSequencer_init_default;
    sub.cluster_index.funcs.decode = read_u32_cb;
    sub.cluster_index.arg          = &cluster_ctx;

    if (!pb_decode(stream, ProtoHEFActionWaitForSequencer_fields, &sub)) return false;

    if (acc->info->wait_sequencer_count < HEF_PARSER_MAX_WAIT_SEQUENCER_ACTIONS) {
        acc->info->wait_sequencer_actions[acc->info->wait_sequencer_count] = out;
        acc->info->wait_sequencer_count++;
    } else {
        acc->info->wait_sequencer_truncated = true;
    }
    return true;
}

/* ProtoHEFActionAllowInputDataflow: sys_index + connection_type.
 * sys_index matches an edge_layer we've already captured in
 * hef_pad_info; the translator uses it to look up the boundary
 * VDMA channel. */
static bool decode_allow_input_dataflow_body(pb_istream_t *stream,
                                             struct ctx_actions_accum *acc)
{
    struct hef_allow_input_dataflow_action out;
    memset(&out, 0, sizeof(out));
    out.context_index = acc->current ? acc->current->context_index : 0;

    bool present_discard = false;
    struct u32_ctx sys_idx_ctx   = { .dst = &out.sys_index,       .present = &present_discard };
    struct u32_ctx conn_type_ctx = { .dst = &out.connection_type, .present = &present_discard };

    ProtoHEFActionAllowInputDataflow sub = ProtoHEFActionAllowInputDataflow_init_default;
    sub.sys_index.funcs.decode       = read_u32_cb;
    sub.sys_index.arg                = &sys_idx_ctx;
    sub.connection_type.funcs.decode = read_u32_cb;
    sub.connection_type.arg          = &conn_type_ctx;

    if (!pb_decode(stream, ProtoHEFActionAllowInputDataflow_fields, &sub)) return false;

    if (acc->info->allow_input_dataflow_count < HEF_PARSER_MAX_ALLOW_INPUT_DATAFLOW_ACTIONS) {
        acc->info->allow_input_dataflow_actions[acc->info->allow_input_dataflow_count] = out;
        acc->info->allow_input_dataflow_count++;
    } else {
        acc->info->allow_input_dataflow_truncated = true;
    }
    return true;
}

/* InitialL3 sub-sub-message inside ProtoHEFActionEnableSequencer.
 * Carries initial_l3_index + initial_l3_offset as u32s. The outer
 * EnableSequencer decoder wires this as a nested callback so both
 * fields land in the parent trigger_sequencer_action. */
struct l3_info_ctx {
    uint32_t *l3_cut_dst;
    uint32_t *l3_offset_dst;
};

static bool decode_l3_info_cb(pb_istream_t *stream,
                              const pb_field_t *field,
                              void **arg)
{
    (void)field;
    struct l3_info_ctx *lctx = (struct l3_info_ctx *)*arg;

    bool present_discard = false;
    struct u32_ctx idx_ctx = { .dst = lctx->l3_cut_dst,    .present = &present_discard };
    struct u32_ctx off_ctx = { .dst = lctx->l3_offset_dst, .present = &present_discard };

    InitialL3 sub = InitialL3_init_default;
    sub.initial_l3_index.funcs.decode  = read_u32_cb;
    sub.initial_l3_index.arg           = &idx_ctx;
    sub.initial_l3_offset.funcs.decode = read_u32_cb;
    sub.initial_l3_offset.arg          = &off_ctx;
    return pb_decode(stream, InitialL3_fields, &sub);
}

/* ProtoHEFActionEnableSequencer: cluster_index + 8 bitmap/offset
 * fields + a nested InitialL3 sub-message. Maps to firmware's
 * TRIGGER_SEQUENCER (u8 cluster_index + 36-byte sequencer_config_t). */
static bool decode_enable_sequencer_body(pb_istream_t *stream,
                                         struct ctx_actions_accum *acc)
{
    struct hef_trigger_sequencer_action out;
    memset(&out, 0, sizeof(out));
    out.context_index = acc->current ? acc->current->context_index : 0;

    bool present_discard = false;
    struct u32_ctx cluster_ctx  = { .dst = &out.cluster_index,     .present = &present_discard };
    struct u32_ctx apu_ctx      = { .dst = &out.active_apu_bitmap, .present = &present_discard };
    struct u32_ctx ia_ctx       = { .dst = &out.active_ia_bitmap,  .present = &present_discard };
    struct u64_ctx sc_ctx       = { .dst = &out.active_sc_bitmap,  .present = &present_discard };
    struct u64_ctx l2_ctx       = { .dst = &out.active_l2_bitmap,  .present = &present_discard };
    struct u64_ctx l2_off0_ctx  = { .dst = &out.l2_offset_0,       .present = &present_discard };
    struct u64_ctx l2_off1_ctx  = { .dst = &out.l2_offset_1,       .present = &present_discard };
    struct l3_info_ctx l3_ctx   = {
        .l3_cut_dst    = &out.initial_l3_cut,
        .l3_offset_dst = &out.initial_l3_offset,
    };

    ProtoHEFActionEnableSequencer sub = ProtoHEFActionEnableSequencer_init_default;
    sub.cluster_index.funcs.decode     = read_u32_cb;
    sub.cluster_index.arg              = &cluster_ctx;
    sub.active_apu_bitmap.funcs.decode = read_u32_cb;
    sub.active_apu_bitmap.arg          = &apu_ctx;
    sub.active_ia_bitmap.funcs.decode  = read_u32_cb;
    sub.active_ia_bitmap.arg           = &ia_ctx;
    sub.active_sc_bitmap.funcs.decode  = read_u64_cb;
    sub.active_sc_bitmap.arg           = &sc_ctx;
    sub.active_l2_bitmap.funcs.decode  = read_u64_cb;
    sub.active_l2_bitmap.arg           = &l2_ctx;
    sub.l2_write_0.funcs.decode        = read_u64_cb;
    sub.l2_write_0.arg                 = &l2_off0_ctx;
    sub.l2_write_1.funcs.decode        = read_u64_cb;
    sub.l2_write_1.arg                 = &l2_off1_ctx;
    sub.initial_l3_info.funcs.decode   = decode_l3_info_cb;
    sub.initial_l3_info.arg            = &l3_ctx;

    if (!pb_decode(stream, ProtoHEFActionEnableSequencer_fields, &sub)) return false;

    if (acc->info->trigger_sequencer_count < HEF_PARSER_MAX_TRIGGER_SEQUENCER_ACTIONS) {
        acc->info->trigger_sequencer_actions[acc->info->trigger_sequencer_count] = out;
        acc->info->trigger_sequencer_count++;
    } else {
        acc->info->trigger_sequencer_truncated = true;
    }
    return true;
}

/* Oneof inner callback: runs once per ProtoHEFAction.action oneof
 * branch. `field->tag` is the active branch's proto field number
 * (2=write_data, 5=enable_sequencer, 6=wait_for_sequencer,
 * 7=disable_lcu, 8=enable_lcu, 10=allow_input_dataflow, …). Every
 * action gets its kind recorded in context_actions[].action_types[];
 * branches with per-action parameter extraction have their scalar
 * fields pulled into hef_info.<kind>_actions[]. */
static bool decode_compute_action_inner_cb(pb_istream_t *stream,
                                           const pb_field_t *field,
                                           void **arg)
{
    struct ctx_actions_accum *acc = (struct ctx_actions_accum *)*arg;

    if (acc->current) {
        /* Guard the shift: field->tag is bounded by the proto schema at
         * compile-time today, but a future branch past 31 would invoke
         * UB on `1u << tag`. Clamp with a mask — tags >= 32 don't get a
         * bit in the mask but still record in action_types[] so the
         * translator can see them. */
        if (field->tag < 32u) {
            acc->current->action_type_mask |= (1u << field->tag);
        }
        if (acc->current->action_count < HEF_PARSER_MAX_CONTEXT_ACTIONS) {
            acc->current->action_types[acc->current->action_count] =
                (uint8_t)field->tag;
            acc->current->action_count++;
        } else {
            acc->current->truncated = true;
        }
    }

    /* Per-action extraction dispatchers. Every other oneof branch
     * falls through to the generic consume-and-skip path. */
    switch (field->tag) {
    case ProtoHEFAction_enable_lcu_tag:
        return decode_enable_lcu_body(stream, acc);
    case ProtoHEFAction_disable_lcu_tag:
        return decode_disable_lcu_body(stream, acc);
    case ProtoHEFAction_wait_for_seqeuncer_tag:
        return decode_wait_sequencer_body(stream, acc);
    case ProtoHEFAction_allow_input_dataflow_tag:
        return decode_allow_input_dataflow_body(stream, acc);
    case ProtoHEFAction_enable_sequencer_tag:
        return decode_enable_sequencer_body(stream, acc);
    default:
        break;
    }

    /* Drain the remaining bytes of THIS oneof branch's sub-message
     * substream. `stream` here is the substream nanopb created for
     * the active oneof branch (e.g. ProtoHEFActionEnableLcu), NOT
     * the parent ProtoHEFAction stream — pb_dec_submessage wraps
     * each sub-message in its own bounded stream before invoking
     * the callback, so `stream->bytes_left` is this branch's body
     * length only. Draining the substream does not affect the
     * parent's cursor. This is the idiomatic nanopb "skip unknown
     * sub-message content" pattern also used by decode_action_cb,
     * decode_network_group_metadata_cb, decode_context_metadata_cb,
     * and the unknown-field handlers throughout this file. */
    return pb_read(stream, NULL, stream->bytes_left);
}

/* Outer callback: fires once per repeated action within an Operation.
 * Decodes the ProtoHEFAction sub-message with the oneof callback
 * wired — same two-level pattern as decode_action_cb for the
 * preliminary_config chain. */
static bool decode_compute_action_outer_cb(pb_istream_t *stream,
                                           const pb_field_t *field,
                                           void **arg)
{
    (void)field;
    struct ctx_actions_accum *acc = (struct ctx_actions_accum *)*arg;

    ProtoHEFAction act = ProtoHEFAction_init_default;
    /* Any oneof branch wires the shared callback slot — see the
     * comment on decode_action_cb for why writing to a single
     * branch reaches every branch. */
    act.action.write_data_ccw.funcs.decode = decode_compute_action_inner_cb;
    act.action.write_data_ccw.arg          = acc;
    return pb_decode(stream, ProtoHEFAction_fields, &act);
}

static bool decode_compute_operation_cb(pb_istream_t *stream,
                                        const pb_field_t *field,
                                        void **arg)
{
    (void)field;
    struct ctx_actions_accum *acc = (struct ctx_actions_accum *)*arg;

    ProtoHEFOperation op = ProtoHEFOperation_init_default;
    op.actions.funcs.decode          = decode_compute_action_outer_cb;
    op.actions.arg                   = acc;
    return pb_decode(stream, ProtoHEFOperation_fields, &op);
}

struct ctx_walker {
    struct hef_info *info;
    struct edge_ctx *edge_ectx;   /* for the existing metadata walker */
};

static bool decode_context_cb(pb_istream_t *stream,
                              const pb_field_t *field,
                              void **arg)
{
    (void)field;
    struct ctx_walker *walker = (struct ctx_walker *)*arg;

    /* Claim a context_actions slot. Exceeding HEF_PARSER_MAX_CONTEXTS
     * sets the truncated flag but still decodes the body so the rest
     * of the sub-message is consumed cleanly. */
    struct hef_context_actions *slot = NULL;
    if (walker->info->context_actions_count < HEF_PARSER_MAX_CONTEXTS) {
        slot = &walker->info->context_actions[
                   walker->info->context_actions_count];
        memset(slot, 0, sizeof(*slot));
        slot->context_index = walker->info->context_actions_count;
    } else {
        walker->info->context_actions_truncated = true;
    }
    walker->info->context_actions_count++;

    struct ctx_actions_accum acc = { .info = walker->info, .current = slot };

    ProtoHEFContext ctx = ProtoHEFContext_init_default;
    ctx.metadata.funcs.decode   = decode_context_metadata_cb;
    ctx.metadata.arg            = walker->edge_ectx;
    ctx.operations.funcs.decode = decode_compute_operation_cb;
    ctx.operations.arg          = &acc;
    return pb_decode(stream, ProtoHEFContext_fields, &ctx);
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
    const uint8_t   *blob_base;
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
    struct ccw_ctx ccw_ctx = {
        .info = ng->info,
        .blob_base = ng->blob_base,
    };
    struct edge_ctx edge_ctx = { .info = ng->info };
    /* Phase 6.4e: ctx_walker outlives the `if (first_ng)` block
     * because pb_decode reads contexts.arg after we fall out of
     * that block — declared at function scope so the pointer
     * remains valid through the decode. */
    struct ctx_walker walker_ctx = {
        .info      = ng->info,
        .edge_ectx = &edge_ctx,
    };
    if (first_ng) {
        grp.network_group_name.funcs.decode = read_string_cb;
        grp.network_group_name.arg          = &name_ctx;
        grp.ops.funcs.decode                = decode_op_cb;
        grp.ops.arg                         = &op_ctx;
        grp.preliminary_config.funcs.decode = decode_preliminary_config_cb;
        grp.preliminary_config.arg          = &ccw_ctx;
        /* Phase 6.2b: walk contexts[].metadata.edge_layers[] and
         * back-fill per-pad quant + stream info. Wire-format ordering
         * means ops (which builds pads[]) decodes first in Hailo's
         * encoding, so pad_index-keyed lookups in decode_edge_layer_cb
         * find their targets.
         *
         * Phase 6.4e: the same context walker also captures per-
         * context compute actions (operations[].actions[]) — the
         * ctx_walker struct bundles both the edge_ctx for metadata
         * and the hef_info for actions[] accumulation. */
        grp.contexts.funcs.decode           = decode_context_cb;
        grp.contexts.arg                    = &walker_ctx;
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
 * the 16 KB kernel stack.
 *
 * This parser's callback chains are bounded by the proto schema's
 * structural depth:
 *   ops path:        Hef → NG → Op → Pad → TensorShape   (5 levels)
 *   ccw path:        Hef → NG → PrelimConfig → Op → Action
 *                        → WriteDataCcw                  (6 levels)
 *   edge-layer path: Hef → NG → Context → CtxMetadata
 *                        → EdgeLayer → EdgeLayerInfo
 *                        → EdgeLayerBase / NumericInfo   (7 levels)
 *
 * Well-formed Hailo-compiled HEFs have fixed structural depth in that
 * range, well within safe limits. Any future path that loads HEF blobs
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
    struct ng_ctx     ng   = {
        .info      = out,
        .blob_base = (const uint8_t *)blob,
    };

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
