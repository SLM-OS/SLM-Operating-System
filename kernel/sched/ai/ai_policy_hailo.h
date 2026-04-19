/*
 * ai_policy_hailo.h — public surface of the Hailo MLP scheduler policy.
 *
 * The policy itself lives in sched_ai.c and registers automatically
 * from sched_ai_init() (non-x86 platforms only). This header exposes
 * the one function external callers need — `ai_policy_hailo_set_model`
 * — so the shell's `hailo load` path can hand off the loaded model
 * handle + HEF-derived quantization parameters.
 *
 * Phase 6.2. Before a model is set, the policy falls back to the
 * heuristic policy for every assign_cpu call (and bumps its fallbacks
 * counter) rather than erroring out. This lets the policy be activated
 * via `sched policy ai_hailo` before or after a HEF is loaded.
 */

#ifndef AI_POLICY_HAILO_H
#define AI_POLICY_HAILO_H

#include "inference_device.h"
#include <stdint.h>

/*
 * Install a loaded model into the Hailo scheduler policy.
 *
 * @handle:        inference_device handle returned from inference_load_model
 *                 against the "hailo-8" backend. Pass INF_INVALID_HANDLE
 *                 to detach the current model (policy resumes
 *                 fallback-to-heuristic behavior).
 * @input_scale:   per-tensor dequantization scale for the input
 *                 (from the HEF's quantization metadata). Pass a
 *                 positive value; non-positive values are replaced
 *                 with a 1/128 placeholder so the path stays live.
 * @input_zp:      per-tensor zero-point for the input.
 * @output_scale:  per-tensor dequantization scale for output logits.
 * @output_zp:     per-tensor zero-point for output logits.
 * @input_n:       number of input elements (defaults to AI_STATE_DIM
 *                 if zero is passed).
 * @output_n:      number of output elements (defaults to
 *                 AI_SCHED_N_ACTIONS if zero is passed).
 *
 * Called from task context (shell thread). Not reentrant — concurrent
 * calls race; serialise at the call-site if multiple policy consumers
 * hot-swap models.
 */
void ai_policy_hailo_set_model(inference_model_handle_t handle,
                               float input_scale,  int8_t input_zp,
                               float output_scale, int8_t output_zp,
                               uint32_t input_n, uint32_t output_n);

/*
 * Integer-only wrapper for callers that can't touch float (e.g. files
 * compiled with -mgeneral-regs-only such as hailo_shell.c).
 *
 * Installs `handle` with the Phase 6.2a placeholder quantization
 * (scale=1/128, zero_point=0, both directions). Used as a fallback
 * by `hailo load <path> sched` when the loaded HEF has no
 * per-pad quant_info.
 *
 * Pass input_n=0 / output_n=0 to defer to the MLP defaults
 * (AI_STATE_DIM / AI_SCHED_N_ACTIONS).
 */
void ai_policy_hailo_set_model_placeholder(inference_model_handle_t handle,
                                           uint32_t input_n,
                                           uint32_t output_n);

/*
 * FP-capable caller passes HEF-derived quantization parameters as
 * raw IEEE-754 bit patterns (see `struct hef_pad_info.qp_scale_raw`
 * / `qp_zp_raw` for why). This file-scope wrapper reinterprets
 * the bit patterns as floats internally. Callers without access
 * to HEF pad info should use ai_policy_hailo_set_model_placeholder.
 *
 * Returns:
 *   0   — installed with HEF-derived quant
 *   -1  — at least one scale was zero / non-finite (caller should
 *          fall back to the placeholder path; no state mutated)
 */
int  ai_policy_hailo_set_model_from_raw(inference_model_handle_t handle,
                                        uint32_t input_scale_raw,
                                        uint32_t input_zp_raw,
                                        uint32_t output_scale_raw,
                                        uint32_t output_zp_raw,
                                        uint32_t input_n, uint32_t output_n);

/*
 * Return the currently installed model handle, or INF_INVALID_HANDLE
 * if no model has been set. Used by the bench command to decide
 * whether to exercise the Hailo backend at all.
 */
inference_model_handle_t ai_policy_hailo_get_model_handle(void);

#endif /* AI_POLICY_HAILO_H */
