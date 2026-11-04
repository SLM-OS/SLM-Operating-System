/*
 * oplib_cold.c - Operator library on the channel SLM-OS builds after a
 * UEFI-direct boot (#1092). See oplib_cold.h.
 *
 * Glue only: the address-space backend, staging and weights-pool
 * layout are the pure helpers in oplib_gpu_as.c; this file holds the
 * per-boot state and the order of operations.
 */

#include "oplib_cold.h"

#include "oplib_gpu_as.h"
#include "oplib_pool.h"
#include "oplib_probe.h"
#include "oplib_weights_pool.h"
#include "uart.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "nvidia/ga10b_bringup.h"          /* handoff accessors, install */
#include "nvidia/ga10b_chan.h"
#include "nvidia/ga10b_channel_handoff.h"
#include "string.h"

static struct {
    bool attached;
    const struct ga10b_gpu_ops *ops;
    struct ga10b_chan *ch;
    struct oplib_cold_as_ctx as_ctx;
    struct oplib_gpu_as as;
    bool qmd_pool_added;        /* attach supplied the handoff's QMD pool */
    bool wpool_valid;
    struct oplib_wpool wpool;
} g_cold;

bool oplib_cold_active(void)
{
    if (!g_cold.attached || !ga10b_bringup_handoff_is_local()) {
        return false;
    }
    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    return h != NULL && h->inst_block_phys == g_cold.ch->inst.phys;
}

const struct oplib_gpu_as *oplib_cold_as(void)
{
    return oplib_cold_active() ? &g_cold.as : NULL;
}

const struct oplib_wpool *oplib_cold_wpool(void)
{
    return (oplib_cold_active() && g_cold.wpool_valid) ? &g_cold.wpool : NULL;
}

uint64_t oplib_cold_lookup(uint64_t gpu_va)
{
    if (!oplib_cold_active()) {
        return 0;
    }
    return ga10b_vm_lookup(g_cold.ops, &g_cold.ch->vm, gpu_va);
}

/* Build + map the weights pool; no TLB invalidate (caller's). */
static int wpool_build(uint64_t bytes)
{
    struct oplib_wpool w;
    int rc = oplib_wpool_build(&g_cold.as, bytes, 0, &w);
    if (rc != OPLIB_AS_OK) {
        uart_printf("[oplib-cold] weights pool: %llu MB requested, build "
                    "failed rc=%d\n",
                    (unsigned long long)(bytes >> 20), rc);
        return rc;
    }
    g_cold.wpool = w;
    g_cold.wpool_valid = true;
    oplib_weights_pool_reset();
    uart_printf("[oplib-cold] weights pool: gpu_va=0x%llx size=%llu MB "
                "(%u x %llu MB chunks%s)\n",
                (unsigned long long)w.gpu_va,
                (unsigned long long)(w.size >> 20), (unsigned)w.n_chunks,
                (unsigned long long)(w.chunk_bytes >> 20),
                w.size < bytes ? ", smaller than requested: PMM ran out" : "");
    return 0;
}

static int tlb_invalidate(void)
{
    int rc = g_cold.as.tlb_invalidate(g_cold.as.ctx);
    if (rc != OPLIB_AS_OK) {
        uart_printf("[oplib-cold] TLB invalidate timed out rc=%d — new "
                    "mappings may not be visible to the GPU\n", rc);
    }
    return rc;
}

int oplib_cold_weights_pool_init(uint64_t bytes)
{
    if (!oplib_cold_active() || bytes == 0) {
        return -1;
    }
    if (g_cold.wpool_valid) {
        return 1;
    }
    int rc = wpool_build(bytes);
    if (rc != 0) {
        return rc;
    }
    return tlb_invalidate() == OPLIB_AS_OK ? 0 : -1;
}

int oplib_cold_attach(const struct ga10b_gpu_ops *ops, struct ga10b_chan *ch,
                      uint64_t weights_pool_bytes)
{
    if (ops == NULL || ch == NULL || !ch->enabled) {
        uart_puts("[oplib-cold] attach: channel not created/enabled\n");
        return -1;
    }
    if (g_cold.attached) {
        return g_cold.ch == ch ? 0 : -1;
    }
    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    if (h == NULL || !ga10b_bringup_handoff_is_local() ||
        h->inst_block_phys != ch->inst.phys) {
        uart_puts("[oplib-cold] attach: the channel's handoff is not the "
                  "active one (install it first)\n");
        return -1;
    }

    g_cold.ops = ops;
    g_cold.ch = ch;
    oplib_gpu_as_init_cold(&g_cold.as, &g_cold.as_ctx, ops, &ch->vm);

    /* 1. QMD pool for slm_oplib_dispatch (the MNIST v7 handoff has
     *    one; a channel-only handoff does not). The copy is
     *    re-installed under the dispatch lock. */
    struct ga10b_channel_handoff copy = *h;
    int q = oplib_gpu_as_ensure_qmd_pool(&g_cold.as, &copy);
    if (q < 0) {
        uart_printf("[oplib-cold] attach: QMD pool alloc/map failed rc=%d\n", q);
        return -1;
    }
    if (q == 1) {
        if (slm_gpu_install_handoff(ga10b_bringup_state(), &copy) != 0) {
            uart_puts("[oplib-cold] attach: re-installing the handoff with "
                      "a QMD pool failed\n");
            return -1;
        }
        g_cold.qmd_pool_added = true;
    }

    /* From here on oplib_cold_active() is true, so every consumer
     * routes to the cold VM and the ga10b_gmmu.c mapper is refused. */
    g_cold.attached = true;

    /* 2. SASS pool + scratch slots + cbuf. */
    int rc = oplib_pool_stage_with_as(&g_cold.as);
    if (rc < 0) {
        uart_printf("[oplib-cold] attach: SASS pool staging failed rc=%d\n", rc);
        return rc;
    }

    /* 3. Weights pool (optional; a failure leaves the CPU path for
     *    weight-bearing ops, it does not fail the attach). */
    if (weights_pool_bytes != 0) {
        (void)wpool_build(weights_pool_bytes);
    }

    /* 4. One invalidate for everything mapped above: the channel (and
     *    the MNIST pipeline) already ran, so the GPU has walked this
     *    VM. Nothing dispatches before the probe below flips tiers. */
    if (tlb_invalidate() != OPLIB_AS_OK) {
        return -1;
    }

    /* 5. Same static probe `nvgpu oplib stage` runs on the kexec path. */
    int simt = oplib_probe_run();
    uart_printf("[oplib-cold] attached to channel %u (inst 0x%llx): pool "
                "gpu_va=0x%llx, cbuf 0x%llx, QMD pool %s, weights pool "
                "%llu MB, %d ops at Simt\n",
                (unsigned)ch->p.chid, (unsigned long long)ch->inst.phys,
                (unsigned long long)oplib_pool_gpu_va_base(),
                (unsigned long long)oplib_pool_cbuf_gpu_va(),
                g_cold.qmd_pool_added ? "added" : "from handoff",
                (unsigned long long)(g_cold.wpool_valid ? g_cold.wpool.size >> 20 : 0),
                simt);
    return 0;
}

void oplib_cold_status_print(void)
{
    if (!g_cold.attached) {
        uart_puts("[oplib-cold] not attached (no UEFI-direct cold channel, "
                  "or oplib_cold_attach not called)\n");
        return;
    }
    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    uart_printf("[oplib-cold] attached: channel %u inst 0x%llx, active=%s, "
                "VM pdb 0x%llx (%u table pages, user VA next 0x%llx)\n",
                (unsigned)g_cold.ch->p.chid,
                (unsigned long long)g_cold.ch->inst.phys,
                oplib_cold_active() ? "yes" : "NO (handoff replaced)",
                (unsigned long long)g_cold.ch->vm.pdb.phys,
                (unsigned)g_cold.ch->vm.table_pages,
                (unsigned long long)g_cold.ch->vm.user_next);
    if (h != NULL) {
        uart_printf("[oplib-cold] handoff v%u: QMD pool gpu_va=0x%llx "
                    "(%u slots, %s), pipeline_n_ops=%u\n",
                    (unsigned)h->version,
                    (unsigned long long)h->qmd_pool_gpu_va,
                    (unsigned)h->qmd_pool_n_slots,
                    g_cold.qmd_pool_added ? "added by attach" : "from handoff",
                    (unsigned)h->pipeline_n_ops);
    }
    uint64_t sp = 0, sv = 0;
    if (oplib_pool_scratch_base(&sp, &sv) == 0) {
        uart_printf("[oplib-cold] SASS/scratch pool: phys=0x%llx gpu_va=0x%llx "
                    "(GPU walk -> 0x%llx); cbuf phys=0x%llx gpu_va=0x%llx "
                    "(GPU walk -> 0x%llx)\n",
                    (unsigned long long)sp, (unsigned long long)sv,
                    (unsigned long long)oplib_cold_lookup(sv),
                    (unsigned long long)oplib_pool_cbuf_phys(),
                    (unsigned long long)oplib_pool_cbuf_gpu_va(),
                    (unsigned long long)oplib_cold_lookup(oplib_pool_cbuf_gpu_va()));
    } else {
        uart_puts("[oplib-cold] SASS/scratch pool: not staged\n");
    }
    if (g_cold.wpool_valid) {
        uart_printf("[oplib-cold] weights pool: gpu_va=0x%llx size=%llu MB, "
                    "%u chunks, used %zu B\n",
                    (unsigned long long)g_cold.wpool.gpu_va,
                    (unsigned long long)(g_cold.wpool.size >> 20),
                    (unsigned)g_cold.wpool.n_chunks,
                    oplib_weights_pool_bytes_used());
    } else {
        uart_puts("[oplib-cold] weights pool: none (`nvgpu oplib wpool <MB>`)\n");
    }
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

int oplib_cold_attach(const struct ga10b_gpu_ops *ops, struct ga10b_chan *ch,
                      uint64_t weights_pool_bytes)
{
    (void)ops;
    (void)ch;
    (void)weights_pool_bytes;
    return -1;
}

bool oplib_cold_active(void) { return false; }
const struct oplib_gpu_as *oplib_cold_as(void) { return NULL; }
const struct oplib_wpool *oplib_cold_wpool(void) { return NULL; }

int oplib_cold_weights_pool_init(uint64_t bytes)
{
    (void)bytes;
    return -1;
}

uint64_t oplib_cold_lookup(uint64_t gpu_va)
{
    (void)gpu_va;
    return 0;
}

void oplib_cold_status_print(void)
{
    uart_puts("[oplib-cold] not available on this platform\n");
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
