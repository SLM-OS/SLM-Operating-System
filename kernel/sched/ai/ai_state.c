/*
 * ai_state.c - State vector extraction for AI scheduler
 *
 * Full implementation in M4. This stub zeros the state vector.
 *
 * This file is compiled WITHOUT -mgeneral-regs-only, so FP
 * instructions are available for float operations.
 */

#include "ai_state.h"

void ai_extract_state(float state[AI_STATE_DIM])
{
    /* Stub: zero-fill the entire state vector */
    for (int i = 0; i < AI_STATE_DIM; i++) {
        state[i] = 0.0f;
    }
}
