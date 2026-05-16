#ifndef SCHED_RUNTIME_MODEL_H
#define SCHED_RUNTIME_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "ai_types.h"

#define SCHED_MODEL_KIND_MLP              0x1001u
#define SCHED_MODEL_KIND_PPO              0x1002u
#define SCHED_MODEL_KIND_CONFIG           0x1003u
#define SCHED_MODEL_KIND_THRESHOLDS       0x1004u
#define SCHED_MODEL_KIND_REBALANCE        0x1005u
/* XGBoost 3-classifier cascade (#855). Payload is too large
 * (~9 MB on the trained model) for the static MLP/PPO dense pool;
 * storage lives Rust-side and the staged/active/rollback dance
 * forwards through the Rust FFI in xgb_ffi.rs. */
#define SCHED_MODEL_KIND_XGBOOST          0x1006u
#define SCHED_MODEL_BLOB_VERSION_V1       1u
#define SCHED_MODEL_SCHEMA_VERSION_V1     1u
#define SCHED_MODEL_FEATURE_VERSION_V1    1u
#define SCHED_MODEL_ACTION_VERSION_V1     1u

enum sched_model_state {
    SCHED_MODEL_EMPTY = 0,
    SCHED_MODEL_STAGED = 1,
    SCHED_MODEL_ACTIVE = 2,
    SCHED_MODEL_ROLLED_BACK = 3,
};

struct sched_model_meta {
    uint16_t version;
    uint16_t schema_version;
    uint16_t feature_version;
    uint16_t action_version;
    uint16_t action_count;
    uint32_t payload_len;
    uint32_t checksum;
};

struct sched_model_status {
    uint16_t kind_id;
    uint16_t state;
    uint32_t has_staged;
    uint32_t has_active;
    uint32_t has_rollback;
    struct sched_model_meta staged;
    struct sched_model_meta active;
    struct sched_model_meta rollback;
};

/* Byte-for-byte layout pin against `SchedModelMetaC` / `SchedModelStatusC`
 * in `runtime/src/sched/xgb.rs`. The Rust side has the matching
 * `const _: () = assert!(size_of::<...>() == N)` asserts at module scope;
 * keeping both halves of the pin live means any future struct reorder
 * (here or in xgb.rs) breaks the build instead of silently mis-reading
 * the FFI status payload at runtime. */
_Static_assert(sizeof(struct sched_model_meta) == 20,
    "struct sched_model_meta must be 20 bytes for SchedModelMetaC FFI layout");
_Static_assert(sizeof(struct sched_model_status) == 76,
    "struct sched_model_status must be 76 bytes for SchedModelStatusC FFI layout");

struct sched_runtime_mlp_model {
    uint16_t feature_version;
    uint16_t action_version;
    uint16_t action_count;
    uint16_t reserved;
    float w0[AI_MLP_LAYER0_OUT * AI_MLP_LAYER0_IN];
    float b0[AI_MLP_LAYER0_OUT];
    float w1[AI_MLP_LAYER1_OUT * AI_MLP_LAYER1_IN];
    float b1[AI_MLP_LAYER1_OUT];
    float w2[AI_MLP_LAYER2_OUT * AI_MLP_LAYER2_IN];
    float b2[AI_MLP_LAYER2_OUT];
    float w3[AI_SCHED_N_ACTIONS * AI_MLP_LAYER3_IN];
    float b3[AI_SCHED_N_ACTIONS];
};

struct sched_runtime_balance_config {
    uint16_t feature_version;
    uint16_t action_version;
    uint16_t action_count;
    uint16_t reserved;
    uint32_t enabled;
    uint32_t min_target_ready;
    uint32_t min_active_cpus;
    uint32_t imbalance_num;
    uint32_t imbalance_den;
};

struct sched_runtime_deadline_thresholds {
    uint16_t feature_version;
    uint16_t action_version;
    uint16_t action_count;
    uint16_t reserved;
    uint64_t critical_ns;
    uint64_t high_ns;
    uint64_t boost_ns;
};

struct sched_runtime_rebalance_config {
    uint16_t feature_version;
    uint16_t action_version;
    uint16_t action_count;
    uint16_t reserved;
    uint32_t enabled;
    uint32_t interval_ticks;
    uint32_t imbalance_min;
};

int sched_model_stage_blob(uint16_t kind_id, const uint8_t *data, size_t len);
int sched_model_validate_blob(uint16_t kind_id, const uint8_t *data, size_t len);
int sched_model_activate(uint16_t kind_id);
int sched_model_rollback(uint16_t kind_id);
int sched_model_clear(uint16_t kind_id);
int sched_model_status(uint16_t kind_id, struct sched_model_status *out);

typedef uintptr_t sched_runtime_token_t;

int sched_runtime_mlp_acquire(const struct sched_runtime_mlp_model **out,
                              sched_runtime_token_t *token);
void sched_runtime_mlp_release(sched_runtime_token_t token);
int sched_runtime_ppo_acquire(const struct sched_runtime_mlp_model **out,
                              sched_runtime_token_t *token);
void sched_runtime_ppo_release(sched_runtime_token_t token);
int sched_runtime_balance_config_acquire(
    const struct sched_runtime_balance_config **out,
    sched_runtime_token_t *token);
void sched_runtime_balance_config_release(sched_runtime_token_t token);
int sched_runtime_balance_config_snapshot(struct sched_runtime_balance_config *out);
int sched_runtime_deadline_thresholds_acquire(
    const struct sched_runtime_deadline_thresholds **out,
    sched_runtime_token_t *token);
void sched_runtime_deadline_thresholds_release(sched_runtime_token_t token);
int sched_runtime_deadline_thresholds_snapshot(
    struct sched_runtime_deadline_thresholds *out);
int sched_runtime_rebalance_config_acquire(
    const struct sched_runtime_rebalance_config **out,
    sched_runtime_token_t *token);
void sched_runtime_rebalance_config_release(sched_runtime_token_t token);
int sched_runtime_rebalance_config_snapshot(
    struct sched_runtime_rebalance_config *out);

#endif
