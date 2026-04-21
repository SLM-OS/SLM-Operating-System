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
 * Append a single action: writes a 5-byte common_action_header_t
 * (with action_type=type, time_stamp=HAILO_CS_TIMESTAMP_INIT_VALUE)
 * followed by body_len bytes of body data. `body_len` must match
 * the fixed size the firmware expects for that action_type (see
 * struct sizes in hailo_cs_actions.h); the builder does not
 * validate this — misuse produces a malformed action that firmware
 * will reject.
 *
 * Returns HAILO_OK on success, HAILO_ERR_INVAL on null args, or
 * HAILO_ERR_NOMEM if the append would exceed the buffer capacity.
 */
int hailo_cs_builder_append(struct hailo_cs_builder *b,
                            enum hailo_cs_action_type type,
                            const void *body, size_t body_len);

/*
 * Append a REPEATED_ACTION wrapper: writes the 5-byte common_action_
 * header_t (action_type = HAILO_CS_ACT_REPEATED_ACTION), the 3-byte
 * repeated_action_header_t (count + last_executed=0 + sub_action_type),
 * then `count` sub-bodies of `sub_body_size` bytes each, packed
 * back-to-back with no interleaved common_action_headers.
 *
 * Total bytes written: 8 + count * sub_body_size.
 *
 * Used by PRELIMINARY to wrap AddCcwBurst sub-actions on Hailo-8L,
 * where direct FETCH_CCW_BURSTS is rejected with
 * CONFIG_MANAGER_WRAPPER_STATUS_ACTION_TYPE_NOT_SUPPORTED but the
 * REPEATED_ACTION-wrapped form is accepted (per HailoRT v4.23 wire
 * capture cached at docs/reference/hailort-v4.23.0-wire-capture-
 * mobilenet.txt).
 *
 * Returns HAILO_OK on success, HAILO_ERR_INVAL on null args or
 * count == 0, HAILO_ERR_NOMEM if the append would exceed capacity.
 */
int hailo_cs_builder_append_repeated(struct hailo_cs_builder *b,
                                     enum hailo_cs_action_type sub_action_type,
                                     uint8_t count,
                                     const void *sub_bodies,
                                     size_t sub_body_size);

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
