/*
 * ai_types.h - Shared types and constants for AI scheduler
 *
 * Defines the state vector dimensions, action structure, and other
 * types shared between the inference engine, state extractor, and
 * AI policy implementation.
 *
 * The state vector is fixed at 108 dimensions (trained model input shape).
 * Platforms with fewer cores zero-fill unused core slots.
 */

#ifndef AI_TYPES_H
#define AI_TYPES_H

#include <stdint.h>

/* ============================================================================
 * State vector dimensions
 *
 * These defaults match the simulator's observation space. When Plan A
 * delivers ai_config.h with platform-specific values, those will
 * override via #ifndef guards.
 * ============================================================================ */

#ifndef AI_STATE_DIM
#define AI_STATE_DIM          108
#endif

#ifndef AI_STATE_NUM_CORES
#define AI_STATE_NUM_CORES    6   /* Max cores in state vector (zero-fill if fewer) */
#endif

#define AI_FEATURES_PER_CORE  6

#ifndef AI_STATE_NUM_TASKS
#define AI_STATE_NUM_TASKS    8   /* Top-N tasks by priority */
#endif

#define AI_FEATURES_PER_TASK  8
#define AI_GLOBAL_FEATURES    8

/* Verify: 6x6 + 8x8 + 8 = 36 + 64 + 8 = 108 */
_Static_assert(AI_STATE_NUM_CORES * AI_FEATURES_PER_CORE +
               AI_STATE_NUM_TASKS * AI_FEATURES_PER_TASK +
               AI_GLOBAL_FEATURES == AI_STATE_DIM,
               "State dimension mismatch");

/* ============================================================================
 * Action space
 *
 * N_ACTIONS = (num_cores + gpu_available) x 3 priorities x 2 preempt
 * Default: Jetson 6 cores + GPU = 7 x 3 x 2 = 42
 * Pi 5:    4 cores, no GPU     = 4 x 3 x 2 = 24
 * ============================================================================ */

#ifndef AI_SCHED_N_ACTIONS
/* PLATFORM_RASPI5 is set on the compiler command line by CMake
 * (add_compile_definitions(PLATFORM_RASPI5=1) when PLATFORM=RASPI5);
 * the gate works because that define is live before any source
 * includes ai_types.h. Other PLATFORM_* macros follow the same
 * build-system contract. */
# if defined(PLATFORM_RASPI5)
#  define AI_SCHED_N_ACTIONS   24  /* Pi 5: 4 cores, no GPU → 4 × 3 × 2 */
# else
#  define AI_SCHED_N_ACTIONS   42  /* Jetson / QEMU default: 6 cores + GPU */
# endif
#endif

/* Action encoding constants: idx = core * AI_ACTIONS_PER_CORE + priority * 2 + preempt */
#define AI_SCHED_PRIORITY_LEVELS  3  /* 0=lower, 1=keep, 2=raise */
#define AI_SCHED_PREEMPT_OPTS     2  /* 0=no, 1=yes */
#define AI_ACTIONS_PER_CORE       (AI_SCHED_PRIORITY_LEVELS * AI_SCHED_PREEMPT_OPTS)  /* 6 */

/*
 * Decoded scheduling action from AI inference.
 */
struct ai_sched_action {
    uint8_t core_assignment;  /* 0 to num_cores-1 (or GPU slot) */
    uint8_t priority_adj;     /* 0=none, 1=boost, 2=reduce */
    uint8_t preempt;          /* 0=no, 1=yes */
};

/*
 * Decode a raw action index into its components.
 * Encoding: idx = core * 6 + priority_adj * 2 + preempt
 */
static inline void ai_decode_action(int idx, struct ai_sched_action *out)
{
    out->preempt = idx % 2;
    idx /= 2;
    out->priority_adj = idx % 3;
    idx /= 3;
    out->core_assignment = (uint8_t)idx;
}

/* ============================================================================
 * MLP model dimensions (4-layer network)
 *
 * Layer 0: W[256 x 108] + b[256]  -> ReLU
 * Layer 1: W[256 x 256] + b[256]  -> ReLU
 * Layer 2: W[128 x 256] + b[128]  -> ReLU
 * Layer 3: W[N_ACTIONS x 128] + b[N_ACTIONS]
 * ============================================================================ */

#define AI_MLP_LAYER0_IN     AI_STATE_DIM   /* 108 */
#define AI_MLP_LAYER0_OUT    256
#define AI_MLP_LAYER1_IN     256
#define AI_MLP_LAYER1_OUT    256
#define AI_MLP_LAYER2_IN     256
#define AI_MLP_LAYER2_OUT    128
#define AI_MLP_LAYER3_IN     128
#define AI_MLP_LAYER3_OUT    AI_SCHED_N_ACTIONS

/* Maximum hidden dimension (for stack-allocated scratch buffers) */
#define AI_MLP_MAX_DIM       256

#endif /* AI_TYPES_H */
