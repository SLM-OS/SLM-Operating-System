/*
 * hailo_cs_builder.h — accumulator for wire-format action bytes.
 *
 * A SET_CONTEXT_INFO RPC carries `context_network_data`: a flat
 * byte stream of [5-byte common_action_header_t][fixed-size body]
 * tuples, one per action. (The header is 5 bytes — 1-byte action_type
 * + 4-byte time_stamp — per the v4.23 firmware wire format; see
 * hailo_cs_actions.h.) This builder accumulates those bytes into
 * a caller-owned buffer, returning the total length when done. The
 * buffer is then handed to hailo_control_set_context_info (which
 * chunks it at HAILO_CS_CONTEXT_CHUNK_MAX_BYTES internally).
 *
 * The builder is a plain in-memory assembler — no locking, no
 * allocation. Callers provide the backing buffer; the builder
 * tracks the cursor and returns HAILO_ERR_NOMEM if an append would
 * overflow it.
 */
#ifndef AI_ACCEL_HAILO_CS_BUILDER_H
#define AI_ACCEL_HAILO_CS_BUILDER_H

#include <stddef.h>
#include <stdint.h>

#include "hailo.h"
#include "hailo_cs_actions.h"

struct hailo_cs_builder {
    uint8_t *buf;
    size_t   capacity;
    size_t   used;
};

/*
 * Initialize a builder around a caller-provided buffer. The buffer
 * need not be pre-cleared; the builder tracks the write cursor.
 */
void hailo_cs_builder_init(struct hailo_cs_builder *b,
                           uint8_t *buf, size_t capacity);

/*
 * Append a single action: writes an 8-byte common_action_header_t
 * (with action_type=type, pad=0, time_stamp=0) followed by body_len
 * bytes of body data. `body_len` must match the fixed size the
 * firmware expects for that action_type (see struct sizes in
 * hailo_cs_actions.h); the builder does not validate this —
 * misuse produces a malformed action that firmware will reject.
 *
 * Returns HAILO_OK on success, HAILO_ERR_INVAL on null args, or
 * HAILO_ERR_NOMEM if the append would exceed the buffer capacity.
 */
int hailo_cs_builder_append(struct hailo_cs_builder *b,
                            enum hailo_cs_action_type type,
                            const void *body, size_t body_len);

/*
 * Bytes written so far. Pass to hailo_control_set_context_info as
 * `network_data_len`.
 */
static inline size_t hailo_cs_builder_size(const struct hailo_cs_builder *b)
{
    return b->used;
}

/*
 * Const pointer to the accumulated bytes. Valid until the next
 * hailo_cs_builder_append (which may advance the cursor but never
 * invalidates earlier content) or until the caller frees the
 * backing buffer.
 */
static inline const uint8_t *hailo_cs_builder_data(const struct hailo_cs_builder *b)
{
    return b->buf;
}

#endif /* AI_ACCEL_HAILO_CS_BUILDER_H */
