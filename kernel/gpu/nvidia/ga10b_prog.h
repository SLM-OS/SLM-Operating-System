/*
 * ga10b_prog.h - A prebuilt compute program: many launches in one
 * pushbuffer, launched by one GPFIFO entry (#475, ADR 0003).
 *
 * ga10b_dispatch_v7_pipeline_inline fits a handful of kernels per call:
 * 42 ops (GA10B_PIPELINE_V7_MAX_OPS), half the 512-entry ring (254, one
 * GPFIFO entry per op), the channel's shared pushbuffer, a 1024-slot
 * QMD pool it rewrites on every launch, and one shared cbuf page, so
 * nothing can stay queued. Each call also runs the post-kexec FECS
 * recovery and polls with IRQs masked. A forward pass needs 300 to
 * 1000 launches per step, so this builder gives a program its own
 * memory instead:
 *
 *   pushbuffer   prologue, SEND_PCAS per launch, barriers, end semaphore
 *   QMD array    256 B per launch, written once
 *   cbuf array   512 B per launch (GA10B_QMD_CBUF0_SIZE_BYTES)
 *   sema array   16 B report slots: start, end, optional one per launch
 *
 * A step then patches the few cbuf fields that change and writes one
 * GPFIFO entry. This file is pure: no MMIO, no allocator, no globals.
 *
 * Barrier method. Dependent launches need (a) the writer finished and
 * (b) its data visible to the reader. NVK does this for compute with
 * WAIT_FOR_IDLE plus INVALIDATE_SHADER_CACHES {DATA, FLUSH_DATA}
 * (nvk_cmd_flush_wait_dep; methods in
 * ~/slmos-ref/mesa/mesa-clc7c0.h:52,299). No SYSMEMBAR is needed between
 * launches: activations stay in the GPU L2, only the end of the program
 * has to reach DRAM. GA10B_PROG_BARRIER_WFI_FLUSH is that sequence.
 * The SLM program uses GA10B_PROG_BARRIER_WFI by default (gpu_program.rs
 * OPT_BARRIER): with the QMD's own shader-data invalidate the extra flush
 * added nothing on jetson-nano-2. `slm gpu opts` selects the others.
 *
 * Completion. REPORT_SEMAPHORE with STRUCTURE_SIZE_FOUR_WORDS writes
 * the payload and a GPU timestamp (ns) to a 16 B slot, so a step gets
 * its GPU time from the start and end slots with no CPU clock in it.
 * Launched work is not waited for by the release, so the program idles first.
 * The start slot uses FLUSH_DISABLE, so it fires as soon as the host
 * reaches it and separates "channel never ran" from "kernel hung" on a
 * timeout, as the v7 path's pre-launch semaphore does (#838).
 *
 * Marks (GA10B_PROG_F_MARKS, `slm stats --profile`). The pushbuffer is
 * written twice, into two runs of the same region, sharing QMDs and cbufs:
 * the plain run and a copy with a timestamp after chosen barriers. Choosing
 * between them is one GPFIFO entry, so a plain step runs exactly the commands
 * it would without marks, and profiling can start between two tokens. A mark
 * follows a WAIT_FOR_IDLE, so its release can't overtake launched work, and
 * it uses FLUSH_DISABLE, so it adds no L2 flush. GA10B_PROG_F_PROFILE stays
 * the heavier per-launch tool: it drains after every launch, which changes
 * what it measures.
 */
#ifndef GPU_NVIDIA_GA10B_PROG_H
#define GPU_NVIDIA_GA10B_PROG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GA10B_PROG_MAX_LAUNCHES 2048u
#define GA10B_PROG_CBUF_STRIDE  512u
#define GA10B_PROG_CBUF_ARGS_BASE 0x160u
#define GA10B_PROG_CBUF_ARGS_MAX (GA10B_PROG_CBUF_STRIDE - GA10B_PROG_CBUF_ARGS_BASE)
#define GA10B_PROG_SEMA_SLOT_BYTES 16u
#define GA10B_PROG_SEMA_START 0u
#define GA10B_PROG_SEMA_END   1u
#define GA10B_PROG_SEMA_OP0   2u
#define GA10B_PROG_PAYLOAD_START 0x51A47001u
#define GA10B_PROG_PAYLOAD_END   0x51A4700Eu
#define GA10B_PROG_PAYLOAD_OP    0x51A47002u
#define GA10B_PROG_PAYLOAD_MARK  0x51A47003u

enum ga10b_prog_barrier {
    GA10B_PROG_BARRIER_WFI_FLUSH = 0,  /* WAIT_FOR_IDLE + flush/invalidate data */
    GA10B_PROG_BARRIER_FLUSH,          /* INVALIDATE_SHADER_CACHES alone */
    GA10B_PROG_BARRIER_WFI,            /* WAIT_FOR_IDLE alone */
    GA10B_PROG_BARRIER_COUNT,
};

/* Flags. */
#define GA10B_PROG_F_LEAN_QMD  (1u << 0)  /* no per-launch icache/texture/const
                                             invalidate, no CWD SYSMEMBAR */
#define GA10B_PROG_F_PROFILE   (1u << 1)  /* drained timestamp after each launch */
#define GA10B_PROG_F_INJECT_HANG (1u << 2) /* wait on a semaphore nobody releases */
#define GA10B_PROG_F_MARKS     (1u << 3)  /* second pushbuffer with marks */

/* One region of GPU-visible memory. */
struct ga10b_prog_mem {
    uint8_t *cpu;
    uint64_t gpu_va;
    uint32_t size;
};

struct ga10b_prog_launch {
    uint64_t shader_gpu_va;
    uint32_t register_count_v;
    uint32_t grid_x, grid_y, grid_z;
    uint32_t block_x, block_y, block_z;
    uint32_t smem_size_bytes;
    uint32_t slm_size_bytes;
    uint32_t barrier_count;
    const void *args;       /* cbuf[0] bytes from GA10B_PROG_CBUF_ARGS_BASE */
    uint32_t args_len;
};

struct ga10b_prog {
    struct ga10b_prog_mem pb, qmd, cbuf, sema;
    uint32_t max_launches;
    uint32_t flags;
    enum ga10b_prog_barrier barrier;
    uint32_t pb_dwords;
    uint32_t marks_base;    /* dwords: where the marked copy starts in pb */
    uint32_t marks_dwords;
    uint32_t n_marks;
    uint32_t n_launches;
    uint32_t n_barriers;
    bool finished;
};

/* Error codes. */
#define GA10B_PROG_OK         0
#define GA10B_PROG_ERR_ARG   (-1)
#define GA10B_PROG_ERR_FULL  (-2)
#define GA10B_PROG_ERR_STATE (-3)

/* Bytes each region needs for `max_launches`. Any output may be NULL. */
void ga10b_prog_sizes(uint32_t max_launches, uint32_t flags, uint32_t *pb_bytes,
                      uint32_t *qmd_bytes, uint32_t *cbuf_bytes, uint32_t *sema_bytes);

/* Check capacities and alignment (QMDs and cbufs 256 B on the GPU) and
 * emit the prologue: SET_OBJECT, memory windows, cache invalidates,
 * start semaphore. */
int ga10b_prog_init(struct ga10b_prog *p, const struct ga10b_prog_mem *pb,
                    const struct ga10b_prog_mem *qmd, const struct ga10b_prog_mem *cbuf,
                    const struct ga10b_prog_mem *sema, uint32_t max_launches,
                    uint32_t flags, enum ga10b_prog_barrier barrier);

/* Append a launch. Returns its index, or a negative error. */
int ga10b_prog_add_launch(struct ga10b_prog *p, const struct ga10b_prog_launch *l);

/* Rewrite an existing launch's QMD and cbuf without touching the
 * pushbuffer. For a step patch, ga10b_prog_cbuf_args() is cheaper. */
int ga10b_prog_update_launch(struct ga10b_prog *p, uint32_t idx,
                             const struct ga10b_prog_launch *l);

/* Append a barrier: everything before it completes before anything
 * after it starts. The pushbuffer has room for one per launch, so the
 * max_launches + 1st returns GA10B_PROG_ERR_FULL. */
int ga10b_prog_add_barrier(struct ga10b_prog *p);

/* Append a timestamp to the marked copy only. Call it right after a barrier.
 * Returns the mark's index, or a negative error. */
int ga10b_prog_add_mark(struct ga10b_prog *p);

/* Append the end semaphore. Nothing can be added after this. */
int ga10b_prog_finish(struct ga10b_prog *p);

/* Ampere GPFIFO entry for the whole pushbuffer. */
uint64_t ga10b_prog_gp_entry(const struct ga10b_prog *p);
/* The same for the marked copy. 0 without GA10B_PROG_F_MARKS. */
uint64_t ga10b_prog_gp_entry_marked(const struct ga10b_prog *p);

/* CPU pointer to a launch's cbuf args, or NULL if out of range. */
uint8_t *ga10b_prog_cbuf_args(const struct ga10b_prog *p, uint32_t idx);
/* CPU pointer to a launch's QMD, or NULL if out of range. */
uint32_t *ga10b_prog_qmd(const struct ga10b_prog *p, uint32_t idx);

/* Zero the start, end and per-launch slots. Clean them before submitting.
 * Marks live after those and are armed apart, so a plain step doesn't pay
 * for them. */
void ga10b_prog_sema_arm(const struct ga10b_prog *p);
uint32_t ga10b_prog_sema_bytes(const struct ga10b_prog *p);
/* Slot of mark 0, and the bytes the finished program's marks use. */
uint32_t ga10b_prog_mark_slot0(const struct ga10b_prog *p);
uint32_t ga10b_prog_marks_bytes(const struct ga10b_prog *p);
/* Fill `ns` with the time up to each mark and from the last mark to the end
 * semaphore: n_marks + 1 values. A mark that didn't fire or a clock that went
 * backwards gives 0 for the intervals around it, not a huge number. Invalidate
 * the marks first. Returns the count written, 0 if `cap` is too small. */
uint32_t ga10b_prog_marks_ns(const struct ga10b_prog *p, uint64_t *ns, uint32_t cap);

/* Read a slot's payload and timestamp (invalidate the range first). */
static inline uint32_t ga10b_prog_sema_payload(const struct ga10b_prog *p, uint32_t slot)
{
    return *(volatile uint32_t *)(void *)(p->sema.cpu + slot * GA10B_PROG_SEMA_SLOT_BYTES);
}

static inline uint64_t ga10b_prog_sema_ts(const struct ga10b_prog *p, uint32_t slot)
{
    return *(volatile uint64_t *)(void *)(p->sema.cpu + slot * GA10B_PROG_SEMA_SLOT_BYTES + 8u);
}

#endif /* GPU_NVIDIA_GA10B_PROG_H */
