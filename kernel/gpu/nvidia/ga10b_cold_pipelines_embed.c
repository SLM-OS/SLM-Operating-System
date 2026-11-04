/*
 * ga10b_cold_pipelines_embed.c - Jetson glue for the cold-boot
 * scheduler-MLP and eviction-MLP GPU pipelines (#1092): sources from
 * the kernel image, build on the cold channel, per-kind install. See
 * ga10b_cold_pipelines.h.
 */
#include "ga10b_cold_pipelines.h"
#include "ga10b_bringup.h"

#include "debug.h"
#include "gpu_handoff.h"        /* SLM_GPU_TIER_*, SLM_GPU_DTYPE_* */
#include "oplib_pool.h"
#include "string.h"

#include <stdint.h>

#if defined(CONFIG_AI_SCHEDULER)
/* kernel/sched/ai/ai_weights_mlp.c (or _real.c under AI_WEIGHTS=real).
 * The sized declarations let ga10b_cold_pipeline_sources check them
 * against the pipeline's layer shapes, so a change to the scheduler
 * model's shape skips the GPU path instead of reading past an array.
 * Read as bytes only. */
#include "../../sched/ai/ai_weights.h"
#endif

static int shaders_from_oplib(const struct ga10b_cp_spec *s, struct ga10b_cp_sources *src)
{
    int rc = oplib_pool_init();
    if (rc != 0) {
        WARN("%s-gpu: operator library unusable (rc=%d)", s->name, rc);
        return -1;
    }
    for (unsigned i = 0; i < s->n_shaders; i++) {
        const uint8_t *sass = NULL;
        size_t size = 0;
        rc = oplib_pool_lookup(s->shader_op_kind[i], SLM_GPU_TIER_SIMT,
                               SLM_GPU_DTYPE_FP32, &sass, &size);
        if (rc != 0 || !sass || size == 0u || size > GA10B_CP_SHADER_BUF_BYTES) {
            WARN("%s-gpu: %s SASS (SIMT FP32) not in the operator library "
                 "(rc=%d size=%lu); copy scripts/cuda/operator_library/%s_shader.sass "
                 "from a Jetson and rebuild", s->name, s->shader_name[i], rc,
                 (unsigned long)size, s->shader_name[i]);
            return -1;
        }
        src->sass[i] = sass;
        src->sass_bytes[i] = (uint32_t)size;
    }
    return 0;
}

int ga10b_cold_pipeline_sources(uint32_t kind, struct ga10b_cp_sources *src)
{
    const struct ga10b_cp_spec *s = ga10b_cp_spec_for_kind(kind);
    if (!s || !src) return -1;
    memset(src, 0, sizeof(*src));

    if (kind == GA10B_PIPELINE_KIND_SCHED_MLP) {
#if defined(CONFIG_AI_SCHEDULER)
        const float *const w[8] = { ai_mlp_w0, ai_mlp_b0, ai_mlp_w1, ai_mlp_b1,
                                    ai_mlp_w2, ai_mlp_b2, ai_mlp_w3, ai_mlp_b3 };
        const size_t have[8] = { sizeof(ai_mlp_w0), sizeof(ai_mlp_b0),
                                 sizeof(ai_mlp_w1), sizeof(ai_mlp_b1),
                                 sizeof(ai_mlp_w2), sizeof(ai_mlp_b2),
                                 sizeof(ai_mlp_w3), sizeof(ai_mlp_b3) };
        for (unsigned i = 0; i < 8u && i < s->n_weights; i++) {
            if (have[i] != s->weights[i].bytes) {
                WARN("sched-mlp-gpu: ai_mlp weight %u is %lu B, the pipeline "
                     "expects %u B (model shape changed?)", i,
                     (unsigned long)have[i], (unsigned)s->weights[i].bytes);
                return -1;
            }
        }
        for (unsigned i = 0; i < 8u; i++) {
            src->weight[i] = (const uint8_t *)w[i];
        }
        src->input = (const uint8_t *)ga10b_cp_sched_input_synth;
        src->input_bytes = (uint32_t)sizeof(ga10b_cp_sched_input_synth);
#else
        WARN("sched-mlp-gpu: AI scheduler not built (AI_SCHED=OFF): no MLP weights");
        return -1;
#endif
    } else {
        const float *const w[8] = { ga10b_evict_mlp_w_l1, ga10b_evict_mlp_b_l1,
                                    ga10b_evict_mlp_w_l2, ga10b_evict_mlp_b_l2,
                                    ga10b_evict_mlp_w_l3, ga10b_evict_mlp_b_l3,
                                    ga10b_evict_mlp_w_out, ga10b_evict_mlp_b_out };
        for (unsigned i = 0; i < 8u; i++) {
            src->weight[i] = (const uint8_t *)w[i];
        }
        /* Row 0 = the synthetic vector (evict.c:316-318). */
        src->input = (const uint8_t *)ga10b_cp_evict_input_synth;
        src->input_bytes = (uint32_t)sizeof(ga10b_cp_evict_input_synth);
    }
    return shaders_from_oplib(s, src);
}

static struct ga10b_cp_pipeline cold_pipes[GA10B_PIPELINE_KIND_COUNT];

/* Make a built pipeline usable: drop stale GPU translations for its
 * new mappings and store its handoff, together under the dispatch lock.
 * `nvgpu cold` can re-run the step after boot while MNIST dispatches on
 * the channel; an all-VA invalidate mid-dispatch can disturb PBDMA
 * (#844), and ensure_bringup reads the store under the same lock. */
struct publish_args {
    const struct ga10b_gpu_ops *ops;
    struct ga10b_vm *vm;
    const struct ga10b_channel_handoff *h;
    int inval_rc;
};

static int publish_locked(void *arg)
{
    struct publish_args *a = arg;
    a->inval_rc = ga10b_vm_tlb_invalidate(a->ops, a->vm);
    if (a->inval_rc != GA10B_CHAN_OK) return GA10B_CP_STORE_ERR_ARG;
    return ga10b_cp_store_put(ga10b_cp_store_global(), a->h);
}

int ga10b_cold_pipeline_build(const struct ga10b_gpu_ops *ops, struct ga10b_chan *ch,
                              uint32_t kind)
{
    const struct ga10b_cp_spec *s = ga10b_cp_spec_for_kind(kind);
    if (!ops || !ch || !ch->enabled || !s) return GA10B_CP_ERR_ARG;
    struct ga10b_cp_pipeline *p = &cold_pipes[kind];
    if (p->built && ga10b_cp_store_get(ga10b_cp_store_global(), kind) != NULL) {
        return GA10B_CP_OK;   /* `nvgpu cold` re-running the step */
    }
    if (p->built) {
        WARN("%s-gpu: built but not stored (earlier store failure)", s->name);
        return GA10B_CP_ERR_ARG;
    }

    struct ga10b_cp_sources src;
    if (ga10b_cold_pipeline_sources(kind, &src) != 0) return GA10B_CP_SKIP;

    int rc = ga10b_cp_build(ops, &ch->vm, s, &src, p);
    if (rc != GA10B_CP_OK) {
        /* Pages already mapped in the channel's VM are leaked, not freed. */
        WARN("%s-gpu: pipeline build failed rc=%d", s->name, rc);
        return rc;
    }

    struct ga10b_channel_handoff h;
    ga10b_cp_fill_handoff(p, ch, &h);
    struct publish_args pa = { .ops = ops, .vm = &ch->vm, .h = &h };
    rc = slm_gpu_call_locked(publish_locked, &pa);
    if (rc != GA10B_CP_STORE_OK) {
        WARN("%s-gpu: v7 handoff not published (rc=%d, invalidate rc=%d)",
             s->name, rc, pa.inval_rc);
        return GA10B_CP_ERR_ARG;
    }
    INFO("%s-gpu: %u-op pipeline built (v7, kind %u, stored): ops 0x%lx, input 0x%lx "
         "(%u B), result 0x%lx (%u B), QMD pool 0x%lx (%u slots)",
         s->name, (unsigned)s->n_ops, (unsigned)s->kind,
         (unsigned long)h.pipeline_ops_phys, (unsigned long)h.input_buf_phys,
         (unsigned)h.input_buf_size, (unsigned long)p->out[s->n_ops - 1u].phys,
         (unsigned)s->result_bytes, (unsigned long)h.qmd_pool_phys,
         (unsigned)h.qmd_pool_n_slots);
    return 0;
}

/* Copy the installed handoff into the store under the dispatch lock:
 * after boot (a `nvgpu cold` retry) ensure_bringup may be rewriting the
 * installed handoff or reading the store on another CPU. */
static int store_active_locked(void *arg)
{
    (void)arg;
    if (!ga10b_bringup_handoff_is_local()) return GA10B_CP_STORE_ERR_ARG;
    return ga10b_cp_store_put(ga10b_cp_store_global(), ga10b_bringup_handoff());
}

int ga10b_cold_pipeline_store_active(void)
{
    return slm_gpu_call_locked(store_active_locked, NULL);
}

int ga10b_cold_pipeline_install(struct ga10b_bringup *b, uint32_t kind)
{
    if (!b) return GA10B_CP_STORE_ERR_ARG;
    struct ga10b_channel_handoff h;
    /* Carry the cursor only from a locally built (cold-channel)
     * handoff; anything else is not this channel. */
    const struct ga10b_channel_handoff *active =
        ga10b_bringup_handoff_is_local() ? ga10b_bringup_handoff() : NULL;
    int rc = ga10b_cp_switch_handoff(ga10b_cp_store_global(), kind, active, &h);
    if (rc != GA10B_CP_STORE_OK) return rc;
    uint32_t from_kind = active ? active->pipeline_kind : GA10B_PIPELINE_KIND_COUNT;
    uint32_t from_cursor = ga10b_bringup_qmd_pool_cursor();
    if (ga10b_bringup_install_handoff(b, &h) != 0) return -1;
    /* install resets the cursor; give the kind back its own. */
    ga10b_bringup_qmd_pool_cursor_set(ga10b_cp_store_swap_cursor(
        ga10b_cp_store_global(), from_kind, from_cursor, kind));
    return 0;
}
