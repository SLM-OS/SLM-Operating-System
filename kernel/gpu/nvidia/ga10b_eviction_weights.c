/*
 * ga10b_eviction_weights.c - The eviction MLP's fp32 weights for the
 * cold-boot GPU eviction pipeline (#1092).
 *
 * The values are the literals of
 * runtime/src/mm/eviction/generated/mlp_policy_f32.rs (the model the
 * Rust CPU eviction policy runs), turned into macros at build time by
 * cmake/gen_eviction_mlp_weights.cmake. Layout is the Rust one,
 * [OUT][IN] row-major; ga10b_cp_build transposes the matrices into the
 * [IN][OUT] layout gemm_fp32 reads. Pure data: no FP instructions are
 * generated, so -mgeneral-regs-only is fine.
 */
#include "ga10b_cold_pipelines.h"
#include "ga10b_eviction_mlp_weights_gen.h"

const float ga10b_evict_mlp_w_l1[64 * 27] = { GA10B_EVICT_MLP_W_L1 };
const float ga10b_evict_mlp_b_l1[64] = { GA10B_EVICT_MLP_B_L1 };
const float ga10b_evict_mlp_w_l2[32 * 64] = { GA10B_EVICT_MLP_W_L2 };
const float ga10b_evict_mlp_b_l2[32] = { GA10B_EVICT_MLP_B_L2 };
const float ga10b_evict_mlp_w_l3[16 * 32] = { GA10B_EVICT_MLP_W_L3 };
const float ga10b_evict_mlp_b_l3[16] = { GA10B_EVICT_MLP_B_L3 };
const float ga10b_evict_mlp_w_out[1 * 16] = { GA10B_EVICT_MLP_W_OUT };
const float ga10b_evict_mlp_b_out[1] = { GA10B_EVICT_MLP_B_OUT };

/* The generator already checks the counts; this catches a header from
 * a different generator version. */
static_assert(GA10B_EVICT_MLP_W_L1_COUNT == 64 * 27, "W_L1 size");
static_assert(GA10B_EVICT_MLP_B_L1_COUNT == 64, "B_L1 size");
static_assert(GA10B_EVICT_MLP_W_L2_COUNT == 32 * 64, "W_L2 size");
static_assert(GA10B_EVICT_MLP_B_L2_COUNT == 32, "B_L2 size");
static_assert(GA10B_EVICT_MLP_W_L3_COUNT == 16 * 32, "W_L3 size");
static_assert(GA10B_EVICT_MLP_B_L3_COUNT == 16, "B_L3 size");
static_assert(GA10B_EVICT_MLP_W_OUT_COUNT == 16, "W_OUT size");
static_assert(GA10B_EVICT_MLP_B_OUT_COUNT == 1, "B_OUT size");
