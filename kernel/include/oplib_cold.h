/*
 * oplib_cold.h - Operator library on the channel SLM-OS builds after a
 * UEFI-direct boot (#1092).
 *
 * On the kexec path the operator library maps its SASS pool, cbuf and
 * scratch into the channel Linux's helper created (ga10b_gmmu.c, keyed
 * off the inherited instance block) and stages weights into the
 * helper's IOVMM pool with the copy engine. None of that exists after
 * a UEFI-direct boot. Once ga10b_cold_run() has built its channel
 * (GA10B_COLD_CHANNEL, ideally also GA10B_COLD_MNIST), the cold
 * bring-up calls oplib_cold_attach(), which:
 *
 *   1. remembers the channel's ga10b_gpu_ops + struct ga10b_vm as the
 *      operator library's address space (struct oplib_gpu_as);
 *   2. gives the active handoff a QMD pool if it has none (channel-only
 *      v2 handoff, MNIST step skipped or failed) and re-installs it;
 *   3. stages the SASS pool (slot 0 + 3 scratch slots) and the cbuf
 *      page into the channel's VM (oplib_pool_stage_to_gpu then reports
 *      "staged");
 *   4. optionally allocates and maps a weights pool from SLM-OS memory
 *      (oplib_weights_pool_* then serve it, staging by memcpy);
 *   5. invalidates the VM's TLB once;
 *   6. runs oplib_probe_run(), flipping the Rust tier table, as
 *      `nvgpu oplib stage` does on the kexec path.
 *
 * After that the existing consumers run unchanged: the W-series FFIs
 * (slm_runtime_dispatch_*_simt, slm_runtime_stage_weight), the
 * `nvgpu oplib ...` shell verbs, and slm_oplib_dispatch, which
 * reaches the channel through the installed handoff.
 *
 * The cold path is active only while the active handoff is the local
 * one for the attached channel (ga10b_bringup_handoff_is_local() and
 * the same instance block); otherwise every consumer keeps the kexec
 * behaviour.
 *
 * Jetson-only. Other platforms get stubs (attach returns -1, active
 * is false).
 */
#ifndef OPLIB_COLD_H
#define OPLIB_COLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ga10b_gpu_ops;
struct ga10b_chan;
struct ga10b_vm;
struct oplib_gpu_as;
struct oplib_wpool;

/* Attach the operator library to the cold channel. `ops` and `ch` must
 * outlive the kernel (ga10b_cold.c's statics), `ch` must be enabled
 * and its handoff must be the active one. Idempotent for the same
 * channel (returns 0 without redoing anything); a different channel
 * is rejected. Must not be called with the GPU dispatch lock held.
 * `weights_pool_bytes` sizes a weights pool taken from PMM in 64 MB
 * chunks (a smaller pool is accepted if memory runs out); the cold
 * bring-up passes 0, like slmos-kexec's helper without
 * --weights-pool-size, and `nvgpu oplib wpool <MB>` sizes it later
 * (oplib_cold_weights_pool_init). Returns 0, or a negative code
 * (nothing published on failure before step 3; a failed weights pool
 * is reported but not fatal). */
int oplib_cold_attach(const struct ga10b_gpu_ops *ops, struct ga10b_chan *ch,
                      uint64_t weights_pool_bytes);

/* True when the attached cold channel's handoff is the active one. */
bool oplib_cold_active(void);

/* True when the embedded operator library holds no SASS: the kernel was built
 * without `make oplib-shaders`, so no GPU consumer can run. */
bool oplib_cold_library_empty(void);

/* The channel's ops and VM, for the SLM GPU program (kernel/gpu/slm_prog.c).
 * 0 when active, -1 otherwise. Anything that reserves VA in that VM or
 * writes its page tables holds slm_prog_vm_lock (slm_prog.h, #1173). */
int oplib_cold_vm(const struct ga10b_gpu_ops **ops, struct ga10b_vm **vm);

/* The cold address space, or NULL when not active. */
const struct oplib_gpu_as *oplib_cold_as(void);

/* Allocate + map the weights pool once (after attach). Returns 0, 1 if
 * one already exists (unchanged), or negative (OPLIB_AS_ERR_BUSY when
 * the VM lock stayed busy). Safe while other GPU work runs: the VA and
 * page-table writes hold the VM lock (#1173) and the TLB invalidate the
 * dispatch lock. */
int oplib_cold_weights_pool_init(uint64_t bytes);

/* The cold weights pool, or NULL when not active / not allocated. */
const struct oplib_wpool *oplib_cold_wpool(void);

/* Physical address the cold VM maps at `gpu_va` (software walk of the
 * page tables under the VM lock, so it never reads a half-written entry;
 * 0 if unmapped, not active or the lock stayed busy). For smoke tests. */
uint64_t oplib_cold_lookup(uint64_t gpu_va);

/* `nvgpu oplib vmrace` (#1173, test only): one round of the VM writes
 * `nvgpu oplib wpool` makes, for racing them against `slm launch --gpu
 * on`. oplib_wpool_build through the cold address space (each reserve
 * and map under the VM lock) on a reused 4 KB page, then, under the
 * lock, every page walked and unmapped. The GPU never uses these VAs.
 * Each round leaves 16 KB of VA and a page-table page every 128 rounds
 * behind. Returns 0, or -1 when the round found a fault or could not
 * run (counted in *st). */
struct oplib_cold_vmrace_stats {
    uint32_t rounds;
    uint32_t busy;       /* the VM lock stayed busy */
    uint32_t map_err;    /* the build failed, or mapped fewer chunks */
    uint32_t bad_walk;   /* a page did not walk to the page it mapped */
    uint32_t stale;      /* a page still walked after the unmap */
};
int oplib_cold_vmrace_round(struct oplib_cold_vmrace_stats *st);

/* `nvgpu oplib cold` status lines. */
void oplib_cold_status_print(void);

#endif /* OPLIB_COLD_H */
