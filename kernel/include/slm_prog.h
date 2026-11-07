/*
 * slm_prog.h - Per-session GPU program for an SLM forward pass (#475,
 * docs/adr/0003-slm-gpu-graph.md).
 *
 * The Rust runtime (runtime/src/slm/gpu_program.rs) decides what runs:
 * ops, buffers, barriers, what a step patches. This layer owns the GPU
 * side of a session:
 *
 *   weights   the GGUF mapped in place, read-only and L2-cacheable, with
 *             64 KB pages when the memory allows it
 *   data      activations, KV cache, gammas, RoPE LUT, logits: PMM pages,
 *             L2-cacheable, CPU-visible after one L2 evict
 *   control   pushbuffers, QMDs, cbufs, semaphores, the step page:
 *             PMM pages, GPU-uncached (CPU writes are cleaned)
 *
 * and turns struct slm_prog_op descriptors into launches
 * (kernel/gpu/nvidia/ga10b_prog.c).
 *
 * Hardware access goes through struct slm_prog_backend. The Jetson
 * backend reaches the cold channel (UEFI-direct boot only; after kexec
 * none is registered and slm_prog_open reports SLM_PROG_ERR_NO_BACKEND).
 * make test registers a fake one.
 */
#ifndef SLM_PROG_H
#define SLM_PROG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Keep in step with runtime/src/slm/gpu_program.rs::Kernel. */
enum slm_prog_kernel {
    SLM_PROG_K_RMSNORM = 0,  /* ptr x, gamma, out; dim n; scal eps bits */
    SLM_PROG_K_ROPE,         /* ptr vec, positions, cos_sin; dim heads, head_dim */
    SLM_PROG_K_SWIGLU,       /* ptr gate, up, out; dim n per row */
    SLM_PROG_K_GQA,          /* ptr q, k, v, out; dim nq, nkv, hd; scal seq_len. rows=1 */
    SLM_PROG_K_GQA_BATCH,    /* same, rows>1: row r attends to seq_len + r + 1 positions */
    SLM_PROG_K_MM_Q4K,       /* ptr x, W, out, bias; dim K, N; scal flags */
    SLM_PROG_K_MM_Q6K,       /* same, Q6_K weights */
    SLM_PROG_K_ADD,          /* ptr a, b, out; dim n per row */
    SLM_PROG_K_COUNT,
};

/* scal[0] of the matmul kernels (scripts/cuda/slm/slm_common.cuh). */
#define SLM_PROG_MM_OUT_F16 1u

/* One operation, as Rust sends it. Keep in step with gpu_program.rs::ProgOp. */
struct slm_prog_op {
    uint32_t kernel;        /* enum slm_prog_kernel */
    uint32_t rows;          /* batch rows */
    uint32_t dim[4];
    uint64_t ptr[4];        /* GPU VAs */
    uint32_t scal[2];
};
_Static_assert(sizeof(struct slm_prog_op) == 64, "slm_prog_op is shared with Rust");

#define SLM_PROG_SHAPES 2u  /* 0 = decode (one row), 1 = prefill (batch) */
#define SLM_PROG_MAX_SESSIONS 4

#define SLM_PROG_FLAG_LEAN_QMD 1u
#define SLM_PROG_FLAG_PROFILE  2u

struct slm_prog_open_args {
    const void *weights;        /* CPU pointer to the file bytes */
    uint64_t weights_bytes;
    uint64_t data_bytes;
    uint32_t step_bytes;
    uint32_t max_launches[SLM_PROG_SHAPES];  /* 0: shape not used */
    uint32_t flags;
    uint32_t barrier;           /* enum ga10b_prog_barrier */
};

struct slm_prog_open_out {
    int32_t handle;
    uint32_t weights_big;       /* 1 when weights use 64 KB PTEs */
    uint64_t weights_gpu_va;
    uint64_t data_gpu_va;
    uint64_t step_gpu_va;
    uint8_t *data_cpu;
    uint8_t *step_cpu;
    uint64_t weights_map_us;    /* mapping + cleaning its page tables */
    uint32_t table_pages_added;
    uint32_t pad;
};

struct slm_prog_run_stats {
    uint64_t gpu_ns;            /* end - start semaphore timestamps */
    uint64_t wait_us;           /* CPU: doorbell to completion */
    uint64_t evict_us;          /* the one L2 evict */
};

/* Errors. Keep in step with gpu_program.rs. */
#define SLM_PROG_ERR_NO_BACKEND  (-1)  /* no cold channel: kexec, QEMU, Pi 5 */
#define SLM_PROG_ERR_ARG         (-2)
#define SLM_PROG_ERR_NOMEM       (-3)
#define SLM_PROG_ERR_MAP         (-4)
#define SLM_PROG_ERR_NO_KERNEL   (-5)  /* kernel missing from the operator library */
#define SLM_PROG_ERR_TIMEOUT     (-6)
#define SLM_PROG_ERR_SUBMIT      (-7)
#define SLM_PROG_ERR_DEAD        (-8)  /* an earlier step on this session failed */
#define SLM_PROG_ERR_EVICT       (-9)
#define SLM_PROG_ERR_IRQS_OFF    (-10)

/* Bounds a wait no step on this hardware should come near: the ADR 0001
 * bar is 23 ms per token and 74 ms for a 64-token prefill. */
#define SLM_PROG_STEP_TIMEOUT_MS 5000u

/* ---- API (called from Rust; all sleepable, IRQs on) ---- */

/* 1 if this boot has a channel programs can run on. */
int slm_prog_available(void);
int slm_prog_open(const struct slm_prog_open_args *a, struct slm_prog_open_out *out);
void slm_prog_close(int32_t h);

/* Build: append a launch (returns its index), a barrier, or finish. */
int slm_prog_add(int32_t h, uint32_t shape, const struct slm_prog_op *op);
int slm_prog_barrier(int32_t h, uint32_t shape);
int slm_prog_finish(int32_t h, uint32_t shape);

/* Step: rewrite a launch's cbuf args (patch) or also its QMD, for a
 * changed grid (update). Both clean what they wrote. */
int slm_prog_patch(int32_t h, uint32_t shape, uint32_t idx, const struct slm_prog_op *op);
int slm_prog_update(int32_t h, uint32_t shape, uint32_t idx, const struct slm_prog_op *op);

/* Clean / invalidate part of the data or step regions. */
void slm_prog_clean(int32_t h, const void *p, size_t len);
void slm_prog_invalidate(int32_t h, const void *p, size_t len);

/* Run a finished shape: submit, wait with IRQs on and no lock held,
 * evict L2. On a timeout the session is dead: the GR and MMU fault state
 * has been dumped and every later call returns SLM_PROG_ERR_DEAD. */
int slm_prog_run(int32_t h, uint32_t shape, struct slm_prog_run_stats *stats);

/* 1 if kernel `k` has SASS in the operator library for `rows` rows. */
int slm_prog_kernel_available(uint32_t kernel, uint32_t rows);

/* Largest batch the library's kernels support (1, 8 or 64). */
uint32_t slm_prog_max_batch(void);

/* Per-launch GPU time when opened with SLM_PROG_FLAG_PROFILE. Returns
 * nanoseconds between launch idx - 1 and idx ending, 0 otherwise. */
uint64_t slm_prog_profile_ns(int32_t h, uint32_t shape, uint32_t idx);

/* EMC and GPU clocks, in Hz (0 when unknown). */
void slm_prog_clocks(uint64_t *emc_hz, uint64_t *gpc_hz);

const char *slm_prog_strerror(int err);

/* ---- Pure: encode one op ---- */

struct ga10b_prog_launch;
/* Fill `l` and `args` (at least GA10B_PROG_CBUF_ARGS_MAX bytes; l->args
 * points at it) for `op`, with the kernel at shader_va. Negative if the
 * shape is invalid. */
int slm_prog_encode(const struct slm_prog_op *op, uint64_t shader_va,
                    struct ga10b_prog_launch *l, uint8_t *args);

/* ---- Backend ---- */

struct ga10b_gpu_ops;
struct ga10b_vm;

struct slm_prog_backend {
    const struct ga10b_gpu_ops *ops;
    struct ga10b_vm *vm;
    /* TLB invalidate for vm, serialised against dispatches. */
    int (*tlb_invalidate)(void);
    /* Kernel VA. Returns 0, or negative when the library has no such kernel. */
    int (*kernel_va)(uint32_t kernel, uint32_t rows, uint64_t *va);
    /* Physical address of a CPU address in the weights, 0 if unmapped. */
    uint64_t (*virt_to_phys)(const void *p);
    /* True when a CPU range is physically contiguous (identity-mapped RAM). */
    bool (*is_contiguous)(const void *p, uint64_t len);
    /* Write one GPFIFO entry and ring the doorbell, marking a program in
     * flight until slm_gpu_prog_done. The dispatch lock is held only
     * inside. `end_slot`: where other CPUs can see completion. */
    int (*submit)(uint64_t gp_entry, const volatile void *end_slot, uint32_t end_payload,
                  uint32_t timeout_ms);
    /* The wait is over (done or timed out). */
    void (*finished)(void);
    /* One L2 evict, under the dispatch lock. 0 on success. */
    int (*l2_evict)(void);
    uint64_t (*now_us)(void);
    /* Give the CPU to other tasks; returns quickly if none is ready. */
    void (*relax)(void);
    /* Timeout: GR + MMU fault state to the console. */
    void (*dump_fault)(const char *why);
    bool (*irqs_enabled)(void);
    /* Optional: raise memory clocks for compute, once per boot. */
    void (*prepare_clocks)(void);
};

/* Test hook; the Jetson backend is picked automatically on a cold boot.
 * NULL restores that. */
void slm_prog_backend_set(const struct slm_prog_backend *b);

#endif /* SLM_PROG_H */
