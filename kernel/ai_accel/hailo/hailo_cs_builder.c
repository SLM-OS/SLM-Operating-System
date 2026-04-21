/*
 * hailo_cs_builder.c — see hailo_cs_builder.h for the spec.
 *
 * This file is kept deliberately small: the builder is a cursor
 * over a caller-provided buffer, no more. All the wire-encoding
 * complexity lives in the struct definitions in hailo_cs_actions.h.
 */

#include <string.h>

#include "hailo_cs_builder.h"

void hailo_cs_builder_init(struct hailo_cs_builder *b,
                           uint8_t *buf, size_t capacity)
{
    if (!b) return;
    b->buf      = buf;
    b->capacity = capacity;
    b->used     = 0;
}

int hailo_cs_builder_append(struct hailo_cs_builder *b,
                            enum hailo_cs_action_type type,
                            const void *body, size_t body_len)
{
    if (!b || !b->buf) return HAILO_ERR_INVAL;
    if (body_len > 0 && !body) return HAILO_ERR_INVAL;

    const size_t need = sizeof(struct hailo_cs_common_action_header) + body_len;
    if (b->used + need > b->capacity) return HAILO_ERR_NOMEM;

    /* Write the 5-byte common header: action_type (u8) followed by
     * 4-byte time_stamp (u32 LE). HailoRT v4.23 sets time_stamp to
     * CONTEXT_SWITCH_DEFS__TIMESTAMP_INIT_VALUE (0xFFFFFFFF) for
     * every action, not 0. See the hailo_cs_common_action_header
     * comment for the wire-capture evidence behind both decisions. */
    struct hailo_cs_common_action_header hdr;
    hdr.action_type = (uint8_t)type;
    hdr.time_stamp  = HAILO_CS_TIMESTAMP_INIT_VALUE;
    memcpy(b->buf + b->used, &hdr, sizeof(hdr));
    b->used += sizeof(hdr);

    /* Then the body, memcpy'd raw (native LE — matches hailort's
     * memcpy-raw convention for context action bodies). */
    if (body_len > 0) {
        memcpy(b->buf + b->used, body, body_len);
        b->used += body_len;
    }
    return HAILO_OK;
}

int hailo_cs_builder_append_repeated(struct hailo_cs_builder *b,
                                     enum hailo_cs_action_type sub_action_type,
                                     uint8_t count,
                                     const void *sub_bodies,
                                     size_t sub_body_size)
{
    if (!b || !b->buf) return HAILO_ERR_INVAL;
    if (count == 0) return HAILO_ERR_INVAL;
    if (sub_body_size > 0 && !sub_bodies) return HAILO_ERR_INVAL;

    /* Overflow guard: count (u8, 1..255) * sub_body_size (size_t).
     * With count <= 255 and typical body sizes <= a few KB, this is
     * comfortably in range, but be explicit — a malformed HEF or
     * test could otherwise blow past the capacity check. */
    size_t sub_total;
    if (__builtin_mul_overflow((size_t)count, sub_body_size, &sub_total)) {
        return HAILO_ERR_INVAL;
    }
    const size_t need = sizeof(struct hailo_cs_common_action_header)
                      + sizeof(struct hailo_cs_repeated_action_header)
                      + sub_total;
    if (b->used + need > b->capacity) return HAILO_ERR_NOMEM;

    /* Common header with REPEATED_ACTION action_type + INIT timestamp. */
    struct hailo_cs_common_action_header hdr;
    hdr.action_type = (uint8_t)HAILO_CS_ACT_REPEATED_ACTION;
    hdr.time_stamp  = HAILO_CS_TIMESTAMP_INIT_VALUE;
    memcpy(b->buf + b->used, &hdr, sizeof(hdr));
    b->used += sizeof(hdr);

    /* Repeated-action header: count + last_executed=0 + sub_action_type.
     * Firmware overwrites last_executed as it processes sub-bodies;
     * emit zero. */
    struct hailo_cs_repeated_action_header rhdr;
    rhdr.count           = count;
    rhdr.last_executed   = 0;
    rhdr.sub_action_type = (uint8_t)sub_action_type;
    memcpy(b->buf + b->used, &rhdr, sizeof(rhdr));
    b->used += sizeof(rhdr);

    /* Sub-bodies packed back-to-back, no per-body common headers. */
    if (sub_total > 0) {
        memcpy(b->buf + b->used, sub_bodies, sub_total);
        b->used += sub_total;
    }
    return HAILO_OK;
}
