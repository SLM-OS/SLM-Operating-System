/*
 * oplib_probe.h - Boot-time op-tier probe (#714, B.2).
 *
 * After SASS-pool staging, walks every SLM op_kind that has
 * registered dispatcher metadata, validates the cbuf-builder +
 * launch-shape accept a representative fixture, and flips the
 * Rust runtime's TIER_TABLE entry to Tier::Simt for ops that
 * pass. Ops without registered metadata (e.g. SLM_GPU_OP_Q4K_GEMM,
 * SLM_GPU_OP_LM_HEAD) stay at the boot default Tier::Cpu.
 *
 * Today's probe is a "static" validation (registry hit +
 * representative-fixture acceptance). Once #714 §B.3 lands the
 * real `Backend::execute` path, this layer also runs each op's
 * fixture through the dispatcher and compares against the
 * embedded CPU reference before flipping the tier — that's the
 * "session-launch smoke test" the issue describes.
 */

#ifndef OPLIB_PROBE_H
#define OPLIB_PROBE_H

#include <stdint.h>

/* Walk every SLM op_kind in [0, SLM_GPU_OP_LM_HEAD], probe the
 * dispatcher metadata for each, and set TIER_TABLE[op] = Simt for
 * the ones that pass. Prints a one-line summary on UART:
 *
 *   [oplib-probe] RMSNORM=Simt ROPE=Simt EMBEDDING=Simt Q4K_DOT=Simt
 *                 Q4K_GEMM=Cpu GQA_ATTN=Simt SWIGLU=Simt LM_HEAD=Cpu
 *
 * Returns the count of ops set to Simt (0..8). Never fails; an
 * unregistered op_kind is just left at the boot default Cpu. */
int oplib_probe_run(void);

#endif /* OPLIB_PROBE_H */
