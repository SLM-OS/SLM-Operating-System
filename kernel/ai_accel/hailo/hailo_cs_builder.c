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

    /* Write the 5-byte common header. action_type is u8 on the wire
     * (hailort's enum is packed); time_stamp is u32 host-chosen and
     * zero is fine for non-tracing runs. */
    struct hailo_cs_common_action_header hdr = {
        .action_type = (uint8_t)type,
        .time_stamp  = 0,
    };
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
