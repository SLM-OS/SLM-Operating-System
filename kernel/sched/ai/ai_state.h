/*
 * ai_state.h - State vector extraction for AI scheduler
 *
 * Extracts the 108-dimensional observation vector from kernel state
 * for input to the AI scheduling models. Full implementation in M4.
 */

#ifndef AI_STATE_H
#define AI_STATE_H

#include "ai_types.h"

/*
 * Extract the current scheduler state into a normalized feature vector.
 *
 * Fills state[0..AI_STATE_DIM-1] with per-core, per-task, and global
 * features matching the simulator's observation space.
 *
 * @state: Output buffer (must be at least AI_STATE_DIM floats)
 */
void ai_extract_state(float state[AI_STATE_DIM]);

#endif /* AI_STATE_H */
