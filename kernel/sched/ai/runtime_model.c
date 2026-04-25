#include "runtime_model.h"

#include "spinlock.h"
#include "string.h"

enum {
    OUTER_HEADER_LEN = 24,
    PAYLOAD_HEADER_LEN = 12,
    SCHED_MODEL_SLOT_COUNT = 3,
    SCHED_MODEL_SLOT_STAGED = 0,
    SCHED_MODEL_SLOT_ACTIVE = 1,
    SCHED_MODEL_SLOT_ROLLBACK = 2,
};

struct sched_model_slot {
    int present;
    uint32_t readers;
    struct sched_model_meta meta;
    union {
        struct sched_runtime_mlp_model dense;
        struct sched_runtime_balance_config balance;
        struct sched_runtime_deadline_thresholds thresholds;
    } payload;
};

struct sched_model_store {
    struct sched_model_slot slots[SCHED_MODEL_SLOT_COUNT];
    uint8_t staged_idx;
    uint8_t active_idx;
    uint8_t rollback_idx;
    uint16_t current_state;
};

enum {
    SCHED_MODEL_STORE_MLP = 0,
    SCHED_MODEL_STORE_PPO = 1,
    SCHED_MODEL_STORE_CONFIG = 2,
    SCHED_MODEL_STORE_THRESHOLDS = 3,
    SCHED_MODEL_STORE_COUNT = 4,
};

static spinlock_t sched_model_lock = SPINLOCK_INIT;
static spinlock_t sched_model_stage_lock = SPINLOCK_INIT;
static struct sched_model_store sched_model_stores[SCHED_MODEL_STORE_COUNT] = {
    {
        .staged_idx = SCHED_MODEL_SLOT_STAGED,
        .active_idx = SCHED_MODEL_SLOT_ACTIVE,
        .rollback_idx = SCHED_MODEL_SLOT_ROLLBACK,
        .current_state = SCHED_MODEL_EMPTY,
    },
    {
        .staged_idx = SCHED_MODEL_SLOT_STAGED,
        .active_idx = SCHED_MODEL_SLOT_ACTIVE,
        .rollback_idx = SCHED_MODEL_SLOT_ROLLBACK,
        .current_state = SCHED_MODEL_EMPTY,
    },
    {
        .staged_idx = SCHED_MODEL_SLOT_STAGED,
        .active_idx = SCHED_MODEL_SLOT_ACTIVE,
        .rollback_idx = SCHED_MODEL_SLOT_ROLLBACK,
        .current_state = SCHED_MODEL_EMPTY,
    },
    {
        .staged_idx = SCHED_MODEL_SLOT_STAGED,
        .active_idx = SCHED_MODEL_SLOT_ACTIVE,
        .rollback_idx = SCHED_MODEL_SLOT_ROLLBACK,
        .current_state = SCHED_MODEL_EMPTY,
    },
};
static struct sched_model_slot stage_scratch_slot;

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *p)
{
    return (uint64_t)read_u32_le(p)
        | ((uint64_t)read_u32_le(p + 4) << 32);
}

static float read_f32_le(const uint8_t *p)
{
    union {
        uint32_t u32;
        float f32;
    } v = { .u32 = read_u32_le(p) };
    return v.f32;
}

static uint32_t checksum32(const uint8_t *data, size_t len)
{
    uint32_t hash = 0x811C9DC5u;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

static int sched_model_store_index(uint16_t kind_id)
{
    switch (kind_id) {
        case SCHED_MODEL_KIND_MLP: return SCHED_MODEL_STORE_MLP;
        case SCHED_MODEL_KIND_PPO: return SCHED_MODEL_STORE_PPO;
        case SCHED_MODEL_KIND_CONFIG: return SCHED_MODEL_STORE_CONFIG;
        case SCHED_MODEL_KIND_THRESHOLDS: return SCHED_MODEL_STORE_THRESHOLDS;
        default: return -1;
    }
}

static size_t sched_dense_payload_len(uint16_t action_count)
{
    size_t floats = 0;

    floats += (size_t)AI_MLP_LAYER0_OUT * AI_MLP_LAYER0_IN;
    floats += AI_MLP_LAYER0_OUT;
    floats += (size_t)AI_MLP_LAYER1_OUT * AI_MLP_LAYER1_IN;
    floats += AI_MLP_LAYER1_OUT;
    floats += (size_t)AI_MLP_LAYER2_OUT * AI_MLP_LAYER2_IN;
    floats += AI_MLP_LAYER2_OUT;
    floats += (size_t)action_count * AI_MLP_LAYER3_IN;
    floats += action_count;

    return PAYLOAD_HEADER_LEN + floats * sizeof(float);
}

static size_t sched_balance_payload_len(void)
{
    return 32u;
}

static size_t sched_thresholds_payload_len(void)
{
    return 36u;
}

static int parse_sched_dense_payload(const uint8_t *payload, size_t payload_len,
                                     struct sched_model_meta *meta,
                                     struct sched_runtime_mlp_model *out)
{
    size_t cursor = 0;
    size_t count = 0;
    float *dst = NULL;

    if (!payload || !meta || !out) return -1;
    if (payload_len < PAYLOAD_HEADER_LEN) return -1;
    if (payload[0] != 'S' || payload[1] != 'M'
     || payload[2] != 'L' || payload[3] != '1') {
        return -1;
    }

    if (read_u16_le(payload + 4) != SCHED_MODEL_BLOB_VERSION_V1) return -1;

    meta->feature_version = read_u16_le(payload + 6);
    meta->action_version = read_u16_le(payload + 8);
    meta->action_count = read_u16_le(payload + 10);

    if (meta->feature_version != SCHED_MODEL_FEATURE_VERSION_V1) return -1;
    if (meta->action_version != SCHED_MODEL_ACTION_VERSION_V1) return -1;
    if (meta->action_count != AI_SCHED_N_ACTIONS) return -1;
    if (payload_len != sched_dense_payload_len(meta->action_count)) return -1;

    memset(out, 0, sizeof(*out));
    out->feature_version = meta->feature_version;
    out->action_version = meta->action_version;
    out->action_count = meta->action_count;

    cursor = PAYLOAD_HEADER_LEN;

#define PARSE_FLOAT_BLOCK(field, elems)                                      \
    do {                                                                     \
        count = (elems);                                                     \
        dst = (field);                                                       \
        for (size_t i = 0; i < count; i++) {                                 \
            dst[i] = read_f32_le(payload + cursor);                          \
            cursor += sizeof(float);                                         \
        }                                                                    \
    } while (0)

    PARSE_FLOAT_BLOCK(out->w0, (size_t)AI_MLP_LAYER0_OUT * AI_MLP_LAYER0_IN);
    PARSE_FLOAT_BLOCK(out->b0, AI_MLP_LAYER0_OUT);
    PARSE_FLOAT_BLOCK(out->w1, (size_t)AI_MLP_LAYER1_OUT * AI_MLP_LAYER1_IN);
    PARSE_FLOAT_BLOCK(out->b1, AI_MLP_LAYER1_OUT);
    PARSE_FLOAT_BLOCK(out->w2, (size_t)AI_MLP_LAYER2_OUT * AI_MLP_LAYER2_IN);
    PARSE_FLOAT_BLOCK(out->b2, AI_MLP_LAYER2_OUT);
    PARSE_FLOAT_BLOCK(out->w3, (size_t)meta->action_count * AI_MLP_LAYER3_IN);
    PARSE_FLOAT_BLOCK(out->b3, meta->action_count);

#undef PARSE_FLOAT_BLOCK

    return cursor == payload_len ? 0 : -1;
}

static int parse_sched_balance_payload(const uint8_t *payload, size_t payload_len,
                                       struct sched_model_meta *meta,
                                       struct sched_runtime_balance_config *out)
{
    if (!payload || !meta || !out) return -1;
    if (payload_len != sched_balance_payload_len()) return -1;
    if (payload[0] != 'S' || payload[1] != 'C'
     || payload[2] != 'F' || payload[3] != '1') {
        return -1;
    }
    if (read_u16_le(payload + 4) != SCHED_MODEL_BLOB_VERSION_V1) return -1;

    memset(out, 0, sizeof(*out));
    meta->feature_version = read_u16_le(payload + 6);
    meta->action_version = read_u16_le(payload + 8);
    meta->action_count = 0;
    if (read_u16_le(payload + 10) != 0) return -1;

    if (meta->feature_version != SCHED_MODEL_FEATURE_VERSION_V1) return -1;
    if (meta->action_version != SCHED_MODEL_ACTION_VERSION_V1) return -1;

    out->feature_version = meta->feature_version;
    out->action_version = meta->action_version;
    out->action_count = 0;
    out->enabled = read_u32_le(payload + 12);
    out->min_target_ready = read_u32_le(payload + 16);
    out->min_active_cpus = read_u32_le(payload + 20);
    out->imbalance_num = read_u32_le(payload + 24);
    out->imbalance_den = read_u32_le(payload + 28);

    if (out->enabled > 1u) return -1;
    if (out->min_active_cpus == 0u) return -1;
    if (out->imbalance_num == 0u || out->imbalance_den == 0u) return -1;
    return 0;
}

static int parse_sched_thresholds_payload(
    const uint8_t *payload, size_t payload_len,
    struct sched_model_meta *meta,
    struct sched_runtime_deadline_thresholds *out)
{
    if (!payload || !meta || !out) return -1;
    if (payload_len != sched_thresholds_payload_len()) return -1;
    if (payload[0] != 'S' || payload[1] != 'T'
     || payload[2] != 'H' || payload[3] != '1') {
        return -1;
    }
    if (read_u16_le(payload + 4) != SCHED_MODEL_BLOB_VERSION_V1) return -1;

    memset(out, 0, sizeof(*out));
    meta->feature_version = read_u16_le(payload + 6);
    meta->action_version = read_u16_le(payload + 8);
    meta->action_count = 0;
    if (read_u16_le(payload + 10) != 0) return -1;

    if (meta->feature_version != SCHED_MODEL_FEATURE_VERSION_V1) return -1;
    if (meta->action_version != SCHED_MODEL_ACTION_VERSION_V1) return -1;

    out->feature_version = meta->feature_version;
    out->action_version = meta->action_version;
    out->action_count = 0;
    out->critical_ns = read_u64_le(payload + 12);
    out->high_ns = read_u64_le(payload + 20);
    out->boost_ns = read_u64_le(payload + 28);

    if (out->critical_ns == 0u || out->high_ns == 0u || out->boost_ns == 0u) {
        return -1;
    }
    if (!(out->critical_ns < out->high_ns && out->high_ns < out->boost_ns)) {
        return -1;
    }
    return 0;
}

static int parse_sched_dense_blob(uint16_t expected_kind_id,
                                  const uint8_t *data, size_t len,
                                  struct sched_model_meta *meta,
                                  struct sched_runtime_mlp_model *out)
{
    const uint8_t *payload = data + OUTER_HEADER_LEN;
    uint16_t version;
    uint16_t kind_id;
    uint16_t schema_version;
    uint32_t payload_len;
    uint32_t checksum;

    if (!data || !meta || !out) return -1;
    if (len < OUTER_HEADER_LEN) return -1;
    if (data[0] != 'S' || data[1] != 'E' || data[2] != 'M' || data[3] != 'B') {
        return -1;
    }

    version = read_u16_le(data + 4);
    kind_id = read_u16_le(data + 6);
    schema_version = read_u16_le(data + 8);
    payload_len = read_u32_le(data + 12);
    checksum = read_u32_le(data + 16);

    if (version != SCHED_MODEL_BLOB_VERSION_V1) return -1;
    if (kind_id != expected_kind_id) return -1;
    if (schema_version != SCHED_MODEL_SCHEMA_VERSION_V1) return -1;
    if (read_u16_le(data + 10) != 0 || read_u32_le(data + 20) != 0) return -1;
    if (len != OUTER_HEADER_LEN + payload_len) return -1;
    if (checksum32(payload, payload_len) != checksum) return -1;

    memset(meta, 0, sizeof(*meta));
    meta->version = version;
    meta->schema_version = schema_version;
    meta->payload_len = payload_len;
    meta->checksum = checksum;

    return parse_sched_dense_payload(payload, payload_len, meta, out);
}

static int parse_sched_balance_blob(uint16_t expected_kind_id,
                                    const uint8_t *data, size_t len,
                                    struct sched_model_meta *meta,
                                    struct sched_runtime_balance_config *out)
{
    const uint8_t *payload = data + OUTER_HEADER_LEN;
    uint16_t version;
    uint16_t kind_id;
    uint16_t schema_version;
    uint32_t payload_len;
    uint32_t checksum;

    if (!data || !meta || !out) return -1;
    if (len < OUTER_HEADER_LEN) return -1;
    if (data[0] != 'S' || data[1] != 'E' || data[2] != 'M' || data[3] != 'B') {
        return -1;
    }

    version = read_u16_le(data + 4);
    kind_id = read_u16_le(data + 6);
    schema_version = read_u16_le(data + 8);
    payload_len = read_u32_le(data + 12);
    checksum = read_u32_le(data + 16);

    if (version != SCHED_MODEL_BLOB_VERSION_V1) return -1;
    if (kind_id != expected_kind_id) return -1;
    if (schema_version != SCHED_MODEL_SCHEMA_VERSION_V1) return -1;
    if (read_u16_le(data + 10) != 0 || read_u32_le(data + 20) != 0) return -1;
    if (len != OUTER_HEADER_LEN + payload_len) return -1;
    if (checksum32(payload, payload_len) != checksum) return -1;

    memset(meta, 0, sizeof(*meta));
    meta->version = version;
    meta->schema_version = schema_version;
    meta->payload_len = payload_len;
    meta->checksum = checksum;

    return parse_sched_balance_payload(payload, payload_len, meta, out);
}

static int parse_sched_thresholds_blob(
    uint16_t expected_kind_id, const uint8_t *data, size_t len,
    struct sched_model_meta *meta,
    struct sched_runtime_deadline_thresholds *out)
{
    const uint8_t *payload = data + OUTER_HEADER_LEN;
    uint16_t version;
    uint16_t kind_id;
    uint16_t schema_version;
    uint32_t payload_len;
    uint32_t checksum;

    if (!data || !meta || !out) return -1;
    if (len < OUTER_HEADER_LEN) return -1;
    if (data[0] != 'S' || data[1] != 'E' || data[2] != 'M' || data[3] != 'B') {
        return -1;
    }

    version = read_u16_le(data + 4);
    kind_id = read_u16_le(data + 6);
    schema_version = read_u16_le(data + 8);
    payload_len = read_u32_le(data + 12);
    checksum = read_u32_le(data + 16);

    if (version != SCHED_MODEL_BLOB_VERSION_V1) return -1;
    if (kind_id != expected_kind_id) return -1;
    if (schema_version != SCHED_MODEL_SCHEMA_VERSION_V1) return -1;
    if (read_u16_le(data + 10) != 0 || read_u32_le(data + 20) != 0) return -1;
    if (len != OUTER_HEADER_LEN + payload_len) return -1;
    if (checksum32(payload, payload_len) != checksum) return -1;

    memset(meta, 0, sizeof(*meta));
    meta->version = version;
    meta->schema_version = schema_version;
    meta->payload_len = payload_len;
    meta->checksum = checksum;

    return parse_sched_thresholds_payload(payload, payload_len, meta, out);
}

static int parse_sched_blob(uint16_t kind_id, const uint8_t *data, size_t len,
                            struct sched_model_meta *meta,
                            struct sched_model_slot *out)
{
    switch (kind_id) {
        case SCHED_MODEL_KIND_MLP:
        case SCHED_MODEL_KIND_PPO:
            return parse_sched_dense_blob(kind_id, data, len, meta,
                                          &out->payload.dense);
        case SCHED_MODEL_KIND_CONFIG:
            return parse_sched_balance_blob(kind_id, data, len, meta,
                                            &out->payload.balance);
        case SCHED_MODEL_KIND_THRESHOLDS:
            return parse_sched_thresholds_blob(kind_id, data, len, meta,
                                               &out->payload.thresholds);
        default:
            return -1;
    }
}

static int sched_store_role_busy(const struct sched_model_store *store, uint8_t idx)
{
    return store->slots[idx].readers != 0;
}

int sched_model_stage_blob(uint16_t kind_id, const uint8_t *data, size_t len)
{
    int store_idx = sched_model_store_index(kind_id);
    irq_flags_t stage_flags;
    irq_flags_t flags;
    struct sched_model_store *store;
    struct sched_model_slot *staged_slot;

    if (store_idx < 0) return -1;

    stage_flags = spin_lock_irqsave(&sched_model_stage_lock);
    memset(&stage_scratch_slot, 0, sizeof(stage_scratch_slot));
    if (parse_sched_blob(kind_id, data, len, &stage_scratch_slot.meta,
                         &stage_scratch_slot) != 0) {
        spin_unlock_irqrestore(&sched_model_stage_lock, stage_flags);
        return -1;
    }
    stage_scratch_slot.present = 1;

    flags = spin_lock_irqsave(&sched_model_lock);
    store = &sched_model_stores[store_idx];
    staged_slot = &store->slots[store->staged_idx];
    if (staged_slot->readers != 0) {
        spin_unlock_irqrestore(&sched_model_lock, flags);
        spin_unlock_irqrestore(&sched_model_stage_lock, stage_flags);
        return -1;
    }

    *staged_slot = stage_scratch_slot;
    store->current_state = SCHED_MODEL_STAGED;
    spin_unlock_irqrestore(&sched_model_lock, flags);
    spin_unlock_irqrestore(&sched_model_stage_lock, stage_flags);
    return 0;
}

int sched_model_validate_blob(uint16_t kind_id, const uint8_t *data, size_t len)
{
    int store_idx = sched_model_store_index(kind_id);
    irq_flags_t stage_flags;

    if (store_idx < 0) return -1;

    stage_flags = spin_lock_irqsave(&sched_model_stage_lock);
    memset(&stage_scratch_slot, 0, sizeof(stage_scratch_slot));
    if (parse_sched_blob(kind_id, data, len, &stage_scratch_slot.meta,
                         &stage_scratch_slot) != 0) {
        spin_unlock_irqrestore(&sched_model_stage_lock, stage_flags);
        return -1;
    }
    spin_unlock_irqrestore(&sched_model_stage_lock, stage_flags);
    return 0;
}

int sched_model_activate(uint16_t kind_id)
{
    int store_idx = sched_model_store_index(kind_id);
    irq_flags_t flags;
    struct sched_model_store *store;
    uint8_t old_staged;
    uint8_t old_active;
    uint8_t old_rollback;

    if (store_idx < 0) return -1;

    flags = spin_lock_irqsave(&sched_model_lock);
    store = &sched_model_stores[store_idx];
    if (!store->slots[store->staged_idx].present) {
        spin_unlock_irqrestore(&sched_model_lock, flags);
        return -1;
    }
    if (sched_store_role_busy(store, store->rollback_idx)) {
        spin_unlock_irqrestore(&sched_model_lock, flags);
        return -1;
    }

    old_staged = store->staged_idx;
    old_active = store->active_idx;
    old_rollback = store->rollback_idx;

    store->active_idx = old_staged;
    store->rollback_idx = old_active;
    store->staged_idx = old_rollback;
    memset(&store->slots[store->staged_idx], 0, sizeof(store->slots[store->staged_idx]));
    store->current_state = SCHED_MODEL_ACTIVE;
    spin_unlock_irqrestore(&sched_model_lock, flags);
    return 0;
}

int sched_model_rollback(uint16_t kind_id)
{
    int store_idx = sched_model_store_index(kind_id);
    irq_flags_t flags;
    struct sched_model_store *store;
    uint8_t old_staged;
    uint8_t old_active;
    uint8_t old_rollback;

    if (store_idx < 0) return -1;

    flags = spin_lock_irqsave(&sched_model_lock);
    store = &sched_model_stores[store_idx];
    if (!store->slots[store->rollback_idx].present) {
        spin_unlock_irqrestore(&sched_model_lock, flags);
        return -1;
    }
    if (sched_store_role_busy(store, store->staged_idx)
     || sched_store_role_busy(store, store->active_idx)) {
        spin_unlock_irqrestore(&sched_model_lock, flags);
        return -1;
    }

    old_staged = store->staged_idx;
    old_active = store->active_idx;
    old_rollback = store->rollback_idx;

    store->active_idx = old_rollback;
    store->rollback_idx = old_active;
    store->staged_idx = old_staged;
    memset(&store->slots[store->staged_idx], 0, sizeof(store->slots[store->staged_idx]));
    store->current_state = SCHED_MODEL_ROLLED_BACK;
    spin_unlock_irqrestore(&sched_model_lock, flags);
    return 0;
}

int sched_model_clear(uint16_t kind_id)
{
    int store_idx = sched_model_store_index(kind_id);
    irq_flags_t flags;
    struct sched_model_store *store;

    if (store_idx < 0) return -1;

    flags = spin_lock_irqsave(&sched_model_lock);
    store = &sched_model_stores[store_idx];
    if (sched_store_role_busy(store, store->staged_idx)
     || sched_store_role_busy(store, store->active_idx)
     || sched_store_role_busy(store, store->rollback_idx)) {
        spin_unlock_irqrestore(&sched_model_lock, flags);
        return -1;
    }

    memset(store->slots, 0, sizeof(store->slots));
    store->staged_idx = SCHED_MODEL_SLOT_STAGED;
    store->active_idx = SCHED_MODEL_SLOT_ACTIVE;
    store->rollback_idx = SCHED_MODEL_SLOT_ROLLBACK;
    store->current_state = SCHED_MODEL_EMPTY;
    spin_unlock_irqrestore(&sched_model_lock, flags);
    return 0;
}

int sched_model_status(uint16_t kind_id, struct sched_model_status *out)
{
    int store_idx = sched_model_store_index(kind_id);
    irq_flags_t flags;
    struct sched_model_store *store;

    if (store_idx < 0 || !out) return -1;

    flags = spin_lock_irqsave(&sched_model_lock);
    store = &sched_model_stores[store_idx];
    memset(out, 0, sizeof(*out));
    out->kind_id = kind_id;
    out->state = store->current_state;
    out->has_staged = store->slots[store->staged_idx].present ? 1u : 0u;
    out->has_active = store->slots[store->active_idx].present ? 1u : 0u;
    out->has_rollback = store->slots[store->rollback_idx].present ? 1u : 0u;
    if (out->has_staged) out->staged = store->slots[store->staged_idx].meta;
    if (out->has_active) out->active = store->slots[store->active_idx].meta;
    if (out->has_rollback) out->rollback = store->slots[store->rollback_idx].meta;
    spin_unlock_irqrestore(&sched_model_lock, flags);
    return 0;
}

static int sched_runtime_acquire_slot(uint16_t kind_id,
                                      const struct sched_model_slot **out_slot,
                                      sched_runtime_token_t *token)
{
    int store_idx = sched_model_store_index(kind_id);
    irq_flags_t flags;
    struct sched_model_store *store;
    struct sched_model_slot *slot;
    uint8_t active_idx;

    if (store_idx < 0 || !out_slot || !token) return 0;

    flags = spin_lock_irqsave(&sched_model_lock);
    store = &sched_model_stores[store_idx];
    active_idx = store->active_idx;
    slot = &store->slots[active_idx];
    if (!slot->present) {
        spin_unlock_irqrestore(&sched_model_lock, flags);
        *out_slot = NULL;
        *token = 0;
        return 0;
    }

    slot->readers++;
    *out_slot = slot;
    *token = (sched_runtime_token_t)((((store_idx & 0xFFu) << 8)
                                   | (active_idx & 0xFFu)) + 1u);
    spin_unlock_irqrestore(&sched_model_lock, flags);
    return 1;
}

static void sched_runtime_release(sched_runtime_token_t token)
{
    int store_idx;
    int slot_idx;
    irq_flags_t flags;
    struct sched_model_slot *slot;

    if (token == 0) return;

    token -= 1u;
    store_idx = (int)((token >> 8) & 0xFFu);
    slot_idx = (int)(token & 0xFFu);
    if (store_idx < 0 || store_idx >= SCHED_MODEL_STORE_COUNT) return;
    if (slot_idx < 0 || slot_idx >= SCHED_MODEL_SLOT_COUNT) return;

    flags = spin_lock_irqsave(&sched_model_lock);
    slot = &sched_model_stores[store_idx].slots[slot_idx];
    if (slot->readers > 0) slot->readers--;
    spin_unlock_irqrestore(&sched_model_lock, flags);
}

int sched_runtime_mlp_acquire(const struct sched_runtime_mlp_model **out,
                              sched_runtime_token_t *token)
{
    const struct sched_model_slot *slot = NULL;
    if (!out) return 0;
    if (!sched_runtime_acquire_slot(SCHED_MODEL_KIND_MLP, &slot, token)) {
        *out = NULL;
        return 0;
    }
    *out = &slot->payload.dense;
    return 1;
}

void sched_runtime_mlp_release(sched_runtime_token_t token)
{
    sched_runtime_release(token);
}

int sched_runtime_ppo_acquire(const struct sched_runtime_mlp_model **out,
                              sched_runtime_token_t *token)
{
    const struct sched_model_slot *slot = NULL;
    if (!out) return 0;
    if (!sched_runtime_acquire_slot(SCHED_MODEL_KIND_PPO, &slot, token)) {
        *out = NULL;
        return 0;
    }
    *out = &slot->payload.dense;
    return 1;
}

void sched_runtime_ppo_release(sched_runtime_token_t token)
{
    sched_runtime_release(token);
}

int sched_runtime_balance_config_acquire(
    const struct sched_runtime_balance_config **out,
    sched_runtime_token_t *token)
{
    const struct sched_model_slot *slot = NULL;
    if (!out) return 0;
    if (!sched_runtime_acquire_slot(SCHED_MODEL_KIND_CONFIG, &slot, token)) {
        *out = NULL;
        return 0;
    }
    *out = &slot->payload.balance;
    return 1;
}

void sched_runtime_balance_config_release(sched_runtime_token_t token)
{
    sched_runtime_release(token);
}

int sched_runtime_balance_config_snapshot(struct sched_runtime_balance_config *out)
{
    const struct sched_runtime_balance_config *active = NULL;
    sched_runtime_token_t token = 0;

    if (!out) return -1;
    if (!sched_runtime_balance_config_acquire(&active, &token) || !active) {
        return -1;
    }

    *out = *active;
    sched_runtime_balance_config_release(token);
    return 0;
}

int sched_runtime_deadline_thresholds_acquire(
    const struct sched_runtime_deadline_thresholds **out,
    sched_runtime_token_t *token)
{
    const struct sched_model_slot *slot = NULL;
    if (!out) return 0;
    if (!sched_runtime_acquire_slot(SCHED_MODEL_KIND_THRESHOLDS, &slot, token)) {
        *out = NULL;
        return 0;
    }
    *out = &slot->payload.thresholds;
    return 1;
}

void sched_runtime_deadline_thresholds_release(sched_runtime_token_t token)
{
    sched_runtime_release(token);
}

int sched_runtime_deadline_thresholds_snapshot(
    struct sched_runtime_deadline_thresholds *out)
{
    const struct sched_runtime_deadline_thresholds *active = NULL;
    sched_runtime_token_t token = 0;

    if (!out) return -1;
    if (!sched_runtime_deadline_thresholds_acquire(&active, &token) || !active) {
        return -1;
    }

    *out = *active;
    sched_runtime_deadline_thresholds_release(token);
    return 0;
}
