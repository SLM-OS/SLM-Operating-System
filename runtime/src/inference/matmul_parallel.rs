//! Parallel Q4_K matmul via persistent per-CPU worker tasks.
//!
//! Background: PR-1 (`chore/neon-q4k-vec-dot`) made the per-block
//! `vec_dot_q4_k_q8_k` ~3.3× faster on Jetson Orin Nano via NEON
//! SDOT. The matmul outer loop in
//! [`crate::inference::ops_transformer::matmul_q4k_rows`] is
//! row-parallel (each output row is an independent dot product
//! against the same activation row), so fanning rows across the 5
//! idle secondary CPUs on Jetson stacks on top of the NEON win.
//!
//! Design: persistent worker tasks rather than per-call task spawn
//! (see PR #652 design discussion). Each remote CPU has a kernel
//! task pinned to it that idles in WFE waiting for work, runs its
//! row slice when signalled, then returns to idle. The dispatcher
//! fills per-CPU mailboxes (NC memory, instantly visible cross-CPU
//! without cache maintenance), broadcasts SEV to wake the workers,
//! runs its own slice on the calling CPU, then polls per-worker
//! `done_flag` until all complete.
//!
//! # SMPEN gating
//!
//! Pre-SMPEN on Jetson the per-CPU L2s are incoherent. Each remote
//! worker writes its slice of the caller's `out_fp32` from a
//! different CPU; without SMPEN the writer must `cache_clean_range`
//! (DC CVAC) and the reader must `cache_invalidate_range` (DC IVAC)
//! before reading. Both reduce to no-ops in the kernel inline
//! helpers once the Jetson SMPEN bringup flips
//! `PLATFORM_HAS_NC_MEMORY` off — see GH issue #655 for the
//! cleanup plan that drops the explicit calls + moves mailboxes
//! out of NC memory.

#![allow(clippy::needless_range_loop)]

use core::ffi::c_void;
use core::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, Ordering};

use crate::kernel_ffi;

// Non-cacheable memory access helpers. AtomicU32 / AtomicU64 use
// LDAR/STLR which are "implementation-defined and may fault" on
// non-cacheable memory per ARM ARM (and per the explicit warning in
// `kernel/include/ncmem.h` / `kernel/CLAUDE.md` §"Non-Cacheable
// Shared Memory"). The kernel-side `bench smp` driver uses plain
// `volatile uint32_t` loads/stores for the same reason — that's the
// safe primitive. Wrap the same pattern here.
//
// Each mailbox field is a single-writer / single-reader u32 or u64
// (writer flips; consumer polls then resets). Plain volatile access
// gives us the cross-CPU visibility we need without atomic-op-on-NC
// concerns. Post-SMPEN (#655) the mailboxes move to cacheable memory
// and these can switch to AtomicU32.
#[inline(always)]
fn vread_u32(p: *const u32) -> u32 {
    // SAFETY: caller passes a valid pointer to a u32 in NC memory
    // that lives for the kernel's lifetime.
    unsafe { core::ptr::read_volatile(p) }
}

#[inline(always)]
fn vwrite_u32(p: *mut u32, v: u32) {
    // SAFETY: same as vread_u32.
    unsafe { core::ptr::write_volatile(p, v) }
}

#[inline(always)]
fn vread_u64(p: *const u64) -> u64 {
    unsafe { core::ptr::read_volatile(p) }
}

#[inline(always)]
fn vwrite_u64(p: *mut u64, v: u64) {
    unsafe { core::ptr::write_volatile(p, v) }
}

#[inline(always)]
fn vread_ptr(p: *const usize) -> usize {
    unsafe { core::ptr::read_volatile(p) }
}

#[inline(always)]
fn vwrite_ptr(p: *mut usize, v: usize) {
    unsafe { core::ptr::write_volatile(p, v) }
}

/// Maximum CPUs we keep mailboxes for. Matches `MAX_CPUS` in
/// `kernel/include/config.h` (8). Sized statically so the mailbox
/// table doesn't need an allocator at first-use time.
const MAX_CPUS: usize = 8;

/// Below this row count we skip parallel dispatch and run the
/// single-threaded NEON matmul. The threshold is "is there enough
/// work to amortise even the cheap signal-and-wait overhead". For
/// Qwen2.5-1.5B every projection matmul has rows ≥ 1536, so this
/// only kicks in for synthetic / test shapes.
const MIN_PARALLEL_ROWS: usize = 64;

/// f32 outputs per cacheline. ARMv8-A cachelines are 64 bytes;
/// `64 / sizeof(f32) == 16`. Worker chunks must be multiples of
/// this so adjacent workers writing the same cacheline (false
/// sharing) is impossible — pre-SMPEN that would silently corrupt
/// the output because two CPUs' L2s would each have a partial
/// version of the line and whoever DC-CVACs second wins.
/// Post-SMPEN this becomes irrelevant (hardware coherency handles
/// concurrent writes to the same line); see GH issue #655.
const ROWS_PER_CACHELINE: usize = 16;

/// Per-worker mailbox. Lives in NC memory so dispatcher writes are
/// visible to the polling worker without cache maintenance. One per
/// remote CPU; allocated at init and never freed.
///
/// Padded to a cache line (64 B) so adjacent workers' mailboxes
/// don't share a line — would force false-share invalidations on
/// every dispatcher write.
///
/// Field types are plain integers (not `AtomicU32`/`AtomicU64`)
/// because LDAR/STLR on non-cacheable memory is "implementation-
/// defined and may fault" per ARM ARM — we use volatile load/store
/// via the helpers above instead. Single-writer / single-reader
/// per field, so plain volatile access is sufficient.
#[repr(C, align(64))]
struct Mailbox {
    /// 0 = idle (no work), 1 = work present, 2 = shutdown requested.
    /// Worker spins/WFEs on this; dispatcher writes 1 to wake it.
    state: u32,
    /// Worker writes 1 here when its slice is complete; dispatcher
    /// polls until all expected `done_flag`s are 1, then resets to 0.
    done_flag: u32,
    /// Pointer to the full weights buffer (stored as usize so the
    /// volatile-helper pair can transport it; cast at use site).
    weights_ptr: usize,
    /// Pointer to the pre-quantized Q8_K activation buffer.
    q8k_ptr: usize,
    /// Pointer to the caller's `out_fp32` buffer. Each worker
    /// writes only its assigned `row_start..row_start+row_count`
    /// slice — disjoint from other workers and the dispatcher.
    out_ptr: usize,
    /// Workload geometry. `cols_q8klen`: cols in low 32, q8k_len
    /// in high 32. `rows_lo_hi`: row_start in low 32, row_count in
    /// high 32. `row_bytes`: full 64.
    cols_q8klen: u64,
    rows_lo_hi: u64,
    row_bytes: u64,
    /// Diagnostic: number of work items completed by this worker.
    /// Read via `rust_matmul_par_diag` from the shell to confirm
    /// workers are actually waking + processing dispatched slices
    /// (vs sitting in WFE with the dispatcher doing all the work
    /// itself). Worker-only writes; dispatcher only reads.
    runs_completed: u64,
}

/// Static array of mailbox *pointers*. The mailboxes themselves
/// live in NC memory (allocated by `slm_ncmem_alloc` in
/// `rust_matmul_workers_init`); this table holds a stable pointer
/// to each. Index by logical CPU id.
///
/// Initialized once at boot via `rust_matmul_workers_init`. Worker
/// reads its own mailbox by indexing `cpu_id()`. Dispatcher reads
/// mailboxes for every remote CPU it intends to use.
static MAILBOXES: [AtomicPtr<Mailbox>; MAX_CPUS] = [
    AtomicPtr::new(core::ptr::null_mut()),
    AtomicPtr::new(core::ptr::null_mut()),
    AtomicPtr::new(core::ptr::null_mut()),
    AtomicPtr::new(core::ptr::null_mut()),
    AtomicPtr::new(core::ptr::null_mut()),
    AtomicPtr::new(core::ptr::null_mut()),
    AtomicPtr::new(core::ptr::null_mut()),
    AtomicPtr::new(core::ptr::null_mut()),
];

/// True after `rust_matmul_workers_init` has successfully spawned
/// the per-CPU worker tasks. Until then `matmul_q4k_rows_parallel`
/// just falls back to the single-threaded path.
static INITIALIZED: AtomicBool = AtomicBool::new(false);

/// Number of available worker CPUs (= cpu_count - 1, since CPU 0
/// is the dispatcher). Cached at init so the dispatcher doesn't
/// re-read `slm_cpu_count` per call.
static WORKER_COUNT: AtomicU32 = AtomicU32::new(0);

// ---------------------------------------------------------------------------
// Init — spawn worker tasks, one per remote CPU
// ---------------------------------------------------------------------------

/// Initialize the parallel matmul worker pool. Called once from C
/// during boot (after `scheduler_init`).
///
/// Spawns one persistent worker task per CPU 1..cpu_count, each
/// pinned to its target CPU. Each worker idles in WFE until its
/// mailbox is signalled. Returns the number of workers successfully
/// spawned (ideally cpu_count - 1).
///
/// Safe to call exactly once. Subsequent calls are no-ops (returns
/// the existing worker count).
#[no_mangle]
pub extern "C" fn rust_matmul_workers_init() -> u32 {
    if INITIALIZED.load(Ordering::Acquire) {
        return WORKER_COUNT.load(Ordering::Acquire);
    }

    let cpu_count = unsafe { kernel_ffi::slm_cpu_count() };
    if cpu_count <= 1 {
        // Single-CPU build (QEMU virt with -smp 1, x86 host harness)
        // — no workers to spawn, dispatcher will fall back to
        // single-threaded.
        INITIALIZED.store(true, Ordering::Release);
        WORKER_COUNT.store(0, Ordering::Release);
        return 0;
    }

    let usable_cpus = (cpu_count as usize).min(MAX_CPUS);
    let mut spawned: u32 = 0;
    for cpu in 1..usable_cpus {
        // Allocate the per-CPU mailbox in NC memory so the
        // dispatcher's writes are immediately visible to the worker
        // without cache maintenance.
        let mb_ptr = unsafe {
            kernel_ffi::slm_ncmem_alloc(
                core::mem::size_of::<Mailbox>(),
                core::mem::align_of::<Mailbox>(),
            )
        } as *mut Mailbox;
        if mb_ptr.is_null() {
            // NC region exhausted — skip this CPU. Dispatcher will
            // notice MAILBOXES[cpu] == null and not assign work.
            continue;
        }
        // SAFETY: mb_ptr is fresh from ncmem_alloc, exclusively
        // ours, sized for one Mailbox. Zero-init via byte writes —
        // can't use `core::ptr::write(mb_ptr, Mailbox { ... })`
        // because the field types are plain ints, not Atomics, and
        // writes through volatile would still be the right shape
        // for NC memory. Writing zeros via write_bytes is a single
        // store of 0 to each byte (compiler will likely vectorize).
        unsafe {
            core::ptr::write_bytes(mb_ptr as *mut u8, 0u8,
                                   core::mem::size_of::<Mailbox>());
        }
        MAILBOXES[cpu].store(mb_ptr, Ordering::Release);

        // Spawn the worker. Name format `mmw<cpu>` — short enough
        // to fit TASK_NAME_LEN (16) without truncation, identifies
        // it in `tasks` shell output.
        // SAFETY: name is a NUL-terminated byte string literal.
        let name: &[u8] = match cpu {
            1 => b"mmw1\0",
            2 => b"mmw2\0",
            3 => b"mmw3\0",
            4 => b"mmw4\0",
            5 => b"mmw5\0",
            6 => b"mmw6\0",
            7 => b"mmw7\0",
            _ => b"mmw?\0",
        };
        let id = unsafe {
            kernel_ffi::slm_task_create_pinned(
                name.as_ptr() as *const core::ffi::c_char,
                worker_entry,
                cpu as *mut c_void,  // pass logical cpu id as the arg
                cpu as u32,
            )
        };
        if id.0 != 0 {
            spawned += 1;
        }
    }

    WORKER_COUNT.store(spawned, Ordering::Release);
    INITIALIZED.store(true, Ordering::Release);
    spawned
}

// ---------------------------------------------------------------------------
// Worker — runs pinned to one CPU, polls its mailbox, exits never
// ---------------------------------------------------------------------------

/// Worker task entry. The kernel calls this once via
/// `slm_task_create_pinned`; it loops forever, idling in WFE
/// between work items. `arg` is the logical CPU id (passed as a
/// pointer-typed integer).
extern "C" fn worker_entry(arg: *mut c_void) {
    let cpu = arg as usize;
    if cpu == 0 || cpu >= MAX_CPUS {
        return;  // bad arg; let the task exit
    }
    let mb_ptr = MAILBOXES[cpu].load(Ordering::Acquire);
    if mb_ptr.is_null() {
        return;
    }
    // Field offsets — we access NC mailbox fields via volatile
    // helpers rather than through `&Mailbox` references because
    // (a) the compiler is allowed to reorder/elide non-volatile
    // reads of plain integers, and (b) the LLVM Rust backend
    // occasionally inserts atomic-flavored loads on `&AtomicU32`
    // even via `.load(Acquire)`, which is the very thing we're
    // avoiding on NC memory. Raw addressed volatile access keeps
    // the codegen exactly as written.
    let state_ptr       = unsafe { &raw mut (*mb_ptr).state };
    let done_ptr        = unsafe { &raw mut (*mb_ptr).done_flag };
    let weights_ptr_p   = unsafe { &raw mut (*mb_ptr).weights_ptr };
    let q8k_ptr_p       = unsafe { &raw mut (*mb_ptr).q8k_ptr };
    let out_ptr_p       = unsafe { &raw mut (*mb_ptr).out_ptr };
    let cols_q8klen_p   = unsafe { &raw mut (*mb_ptr).cols_q8klen };
    let rows_lo_hi_p    = unsafe { &raw mut (*mb_ptr).rows_lo_hi };
    let row_bytes_p     = unsafe { &raw mut (*mb_ptr).row_bytes };
    let runs_p          = unsafe { &raw mut (*mb_ptr).runs_completed };

    loop {
        // Spin-wait on the NC mailbox state. We tried WFE+SEV and
        // observed apparent serialization (workers' runs counter
        // grew correctly but wall-clock matched serial), suggesting
        // the SEV broadcast wasn't actually waking all 5 workers
        // simultaneously on this hardware. Spin-wait is a temporary
        // diagnostic — burns 100% CPU on the worker cores when
        // idle. The persistent worker model means this is forever
        // post-init; flip back to WFE once the SEV path is proven
        // (or stays as-is if WFE's wake fan-out really is one-CPU).
        while vread_u32(state_ptr) == 0 {
            core::hint::spin_loop();
        }
        let state = vread_u32(state_ptr);
        if state == 2 {
            // Shutdown — caller wants the worker to exit. Not used
            // today (workers are persistent for the kernel's
            // lifetime); leaving the path here for future module
            // unload / restart support.
            vwrite_u32(state_ptr, 0);
            return;
        }

        // Decode the mailbox into local values, then run the matmul
        // slice. After completion we MUST cache_clean the output
        // slice before signalling done so the dispatcher (running
        // on a different CPU) sees coherent data when it
        // subsequently invalidates and reads.
        let weights = vread_ptr(weights_ptr_p) as *mut u8;
        let q8k     = vread_ptr(q8k_ptr_p) as *mut u8;
        let out     = vread_ptr(out_ptr_p) as *mut f32;
        let cols_q8klen = vread_u64(cols_q8klen_p);
        let q8k_len = ((cols_q8klen >> 32) & 0xFFFF_FFFF) as usize;
        let rows_lh = vread_u64(rows_lo_hi_p);
        let row_start = (rows_lh & 0xFFFF_FFFF) as usize;
        let row_count = ((rows_lh >> 32) & 0xFFFF_FFFF) as usize;
        let row_bytes = vread_u64(row_bytes_p) as usize;

        run_slice(weights, row_start, row_count, row_bytes, q8k, q8k_len, out);

        // Push our slice of out_fp32 to PoC so the dispatcher's
        // subsequent invalidate + read sees coherent data.
        let out_slice_ptr = unsafe {
            (out as *mut u8).add(row_start * core::mem::size_of::<f32>())
        };
        let out_slice_bytes = row_count * core::mem::size_of::<f32>();
        unsafe {
            kernel_ffi::slm_cache_clean_range(out_slice_ptr, out_slice_bytes);
        }

        // Diagnostic: bump per-worker run counter so
        // `bench matmul_par_diag` can confirm workers are actually
        // processing dispatched work.
        let prev_runs = vread_u64(runs_p);
        vwrite_u64(runs_p, prev_runs.wrapping_add(1));

        // Reset state to idle BEFORE setting done_flag so a future
        // dispatcher writing state=1 again won't race with our
        // state=0 store.
        vwrite_u32(state_ptr, 0);
        vwrite_u32(done_ptr, 1);
    }
}

/// Run the matmul over `rows` rows starting at `row_start`. Pulled
/// out of `worker_entry` so the dispatcher can call the same code
/// path inline for its own slice on the calling CPU.
///
/// # Safety
/// All pointers must be valid for the indicated geometry; same
/// invariants as `matmul_q4k_rows` plus the slice-disjointness the
/// dispatcher establishes.
fn run_slice(
    weights: *mut u8,
    row_start: usize,
    row_count: usize,
    row_bytes: usize,
    q8k: *mut u8,
    q8k_len: usize,
    out: *mut f32,
) {
    if weights.is_null() || q8k.is_null() || out.is_null() || row_count == 0 {
        return;
    }
    let total_bytes = row_count.saturating_mul(row_bytes);
    // SAFETY: the dispatcher guarantees that
    // `weights[row_start*row_bytes .. (row_start+row_count)*row_bytes]`
    // and `out[row_start..row_start+row_count]` are inside the
    // caller's allocations and disjoint from other workers' slices.
    let weights_slice = unsafe {
        core::slice::from_raw_parts(weights.add(row_start * row_bytes), total_bytes)
    };
    let q8k_slice = unsafe { core::slice::from_raw_parts(q8k, q8k_len) };
    let out_slice = unsafe {
        core::slice::from_raw_parts_mut(out.add(row_start), row_count)
    };

    // Walk rows. Each row is one Q4_K-row × Q8_K-acts dot product —
    // the per-block dot is the NEON-SDOT kernel from PR-1.
    use crate::inference::quant::vec_dot_q4_k_q8_k;
    for r in 0..row_count {
        let w_row = &weights_slice[r * row_bytes..(r + 1) * row_bytes];
        if let Some(v) = vec_dot_q4_k_q8_k(w_row, q8k_slice) {
            out_slice[r] = v;
        }
    }
}

// ---------------------------------------------------------------------------
// Dispatcher — fan rows across CPUs
// ---------------------------------------------------------------------------

/// Parallel multi-row Q4_K matmul. Splits `rows` into chunks across
/// the calling CPU + every available remote worker, runs each chunk
/// in parallel, and joins.
///
/// Signature mirrors `crate::inference::ops_transformer::matmul_q4k_rows`
/// — the caller passes the FP16 activations + a Q8_K scratch buffer,
/// and this function does the one-time activation quantization
/// internally before fanning the row dots across workers. Returns
/// `None` on shape mismatch.
///
/// When the worker pool isn't initialized or there are no remote
/// workers (single-CPU builds), or `rows` is below
/// [`MIN_PARALLEL_ROWS`], or cacheline-alignment leaves no work for
/// the dispatcher, falls back to the single-threaded NEON path.
///
/// `weights`, `q8k_scratch`, and `out_fp32` MUST live for the full
/// duration of this call — the workers read/write them through
/// raw pointers stored in their NC mailboxes. The dispatcher
/// blocks until every worker signals done, so the lifetime is
/// trivially upheld.
pub fn matmul_q4k_rows_parallel(
    weights: &[u8],
    rows: usize,
    cols: usize,
    acts_fp16: &[u16],
    q8k_scratch: &mut [u8],
    out_fp32: &mut [f32],
) -> Option<()> {
    use crate::inference::quant::{quantize_row_q8_k, Q8_K_BLOCK_SIZE};
    use crate::slm::gguf::{f16_to_f32, q4_k_byte_size, Q4_K_BLOCK_ELEMENTS};

    // Shape checks identical to matmul_q4k_rows.
    if cols == 0 || cols % Q4_K_BLOCK_ELEMENTS != 0 {
        return None;
    }
    if acts_fp16.len() != cols || out_fp32.len() != rows {
        return None;
    }
    let row_bytes = q4_k_byte_size(cols)?;
    let total = row_bytes.checked_mul(rows)?;
    if weights.len() != total {
        return None;
    }
    let q8k_len = (cols / 256) * Q8_K_BLOCK_SIZE;
    if q8k_scratch.len() < q8k_len {
        return None;
    }

    // Quantize activations once. Workers consume the same Q8_K bytes
    // read-only, so this is a single-CPU op on the dispatcher.
    extern crate alloc;
    let mut acts_f32: alloc::vec::Vec<f32> = alloc::vec::Vec::with_capacity(cols);
    for &bits in acts_fp16.iter() {
        acts_f32.push(f16_to_f32(bits));
    }
    quantize_row_q8_k(&acts_f32, &mut q8k_scratch[..q8k_len])?;
    let acts_q8k: &[u8] = &q8k_scratch[..q8k_len];

    // Single-threaded fallback paths.
    if rows < MIN_PARALLEL_ROWS || !INITIALIZED.load(Ordering::Acquire) {
        return run_single_threaded(weights, rows, row_bytes, acts_q8k, out_fp32);
    }
    let worker_count = WORKER_COUNT.load(Ordering::Acquire) as usize;
    if worker_count == 0 {
        return run_single_threaded(weights, rows, row_bytes, acts_q8k, out_fp32);
    }

    // Split rows across (worker_count + 1) participants. Worker
    // chunks MUST be multiples of ROWS_PER_CACHELINE so two workers
    // writing adjacent rows can never share a cacheline (false
    // sharing — see ROWS_PER_CACHELINE doc above for why this is
    // a correctness issue pre-SMPEN, not just a perf one).
    //
    // Strategy: workers all get the same `worker_chunk` rows
    // (cacheline-aligned), starting at offset 0 of the output.
    // The dispatcher takes whatever remains at the end (which can
    // be unaligned since nothing comes after it that another CPU
    // would write to).
    let participants = worker_count + 1;
    let target_chunk = rows / participants;
    let worker_chunk = ((target_chunk + ROWS_PER_CACHELINE - 1)
        / ROWS_PER_CACHELINE) * ROWS_PER_CACHELINE;
    if worker_chunk == 0 {
        return run_single_threaded(weights, rows, row_bytes, acts_q8k, out_fp32);
    }
    let worker_total = worker_chunk.checked_mul(worker_count)?;
    if worker_total >= rows {
        // Cacheline alignment claimed all the rows; nothing left
        // for the dispatcher and possibly some workers idled. Fall
        // back to single-threaded — the parallelism win wouldn't
        // amortise the dispatch overhead at this size anyway.
        return run_single_threaded(weights, rows, row_bytes, acts_q8k, out_fp32);
    }
    let dispatcher_count = rows - worker_total;
    let dispatcher_row_start = worker_total;

    // Hand work to remote workers. Worker `w` (1-indexed by CPU)
    // takes rows `[(w-1) * worker_chunk, w * worker_chunk)`.
    let mut row_cursor: usize = 0;
    let mut signalled: [bool; MAX_CPUS] = [false; MAX_CPUS];
    let mut chunks_left = worker_count;
    for cpu in 1..MAX_CPUS {
        if chunks_left == 0 {
            break;
        }
        let mb_ptr = MAILBOXES[cpu].load(Ordering::Acquire);
        if mb_ptr.is_null() {
            continue;
        }
        // SAFETY: MAILBOXES[cpu] points to a Mailbox in NC memory
        // that lives for the kernel's lifetime; non-null check above.
        // Field offsets pinned to per-field raw pointers — see
        // worker_entry for the rationale (avoid LDAR/STLR on NC).
        let state_ptr     = unsafe { &raw mut (*mb_ptr).state };
        let done_ptr      = unsafe { &raw mut (*mb_ptr).done_flag };
        let weights_ptr_p = unsafe { &raw mut (*mb_ptr).weights_ptr };
        let q8k_ptr_p     = unsafe { &raw mut (*mb_ptr).q8k_ptr };
        let out_ptr_p     = unsafe { &raw mut (*mb_ptr).out_ptr };
        let cols_q8klen_p = unsafe { &raw mut (*mb_ptr).cols_q8klen };
        let rows_lo_hi_p  = unsafe { &raw mut (*mb_ptr).rows_lo_hi };
        let row_bytes_p   = unsafe { &raw mut (*mb_ptr).row_bytes };

        // Reset the worker's done flag BEFORE we hand it work, so
        // the post-work poll never sees a stale 1 from a previous
        // dispatch.
        vwrite_u32(done_ptr, 0);
        vwrite_ptr(weights_ptr_p, weights.as_ptr() as usize);
        vwrite_ptr(q8k_ptr_p, acts_q8k.as_ptr() as usize);
        vwrite_ptr(out_ptr_p, out_fp32.as_mut_ptr() as usize);
        vwrite_u64(cols_q8klen_p, (cols as u64) | ((q8k_len as u64) << 32));
        vwrite_u64(rows_lo_hi_p, (row_cursor as u64) | ((worker_chunk as u64) << 32));
        vwrite_u64(row_bytes_p, row_bytes as u64);
        // DSB before flipping state so the worker, waking on SEV,
        // sees a fully populated mailbox before it reads state==1.
        // NC stores complete to PoC but ordering between them
        // (relative to the state store the worker keys on) needs an
        // explicit barrier.
        unsafe { core::arch::asm!("dsb sy", options(nostack, preserves_flags)) };
        // Publish the work flag last.
        vwrite_u32(state_ptr, 1);
        signalled[cpu] = true;
        row_cursor += worker_chunk;
        chunks_left -= 1;
    }

    // Wake any worker currently in WFE.
    unsafe { kernel_ffi::slm_sev() };

    // Run the dispatcher's own slice on the calling CPU. Our slice
    // starts at `dispatcher_row_start` (cacheline-aligned by
    // construction since worker_chunk is a multiple of
    // ROWS_PER_CACHELINE and worker_total = worker_chunk *
    // worker_count is too). No cache maintenance needed for our
    // own writes — same CPU as the caller's read.
    use crate::inference::quant::vec_dot_q4_k_q8_k;
    for r in 0..dispatcher_count {
        let abs_row = dispatcher_row_start + r;
        let w_row = &weights[abs_row * row_bytes..(abs_row + 1) * row_bytes];
        out_fp32[abs_row] = vec_dot_q4_k_q8_k(w_row, acts_q8k)?;
    }

    // Join: poll every signalled worker's done_flag. Workers
    // pre-clean their slice with cache_clean_range before signalling
    // done; we invalidate the same range before reading.
    for cpu in 1..MAX_CPUS {
        if !signalled[cpu] {
            continue;
        }
        let mb_ptr = MAILBOXES[cpu].load(Ordering::Acquire);
        let done_ptr = unsafe { &raw mut (*mb_ptr).done_flag };
        let rows_lo_hi_p = unsafe { &raw mut (*mb_ptr).rows_lo_hi };
        while vread_u32(done_ptr) == 0 {
            // Workers run on different CPUs and complete on the
            // order of the matmul time (microseconds for small
            // shapes, low-millis for the largest projections in
            // Qwen2.5). Spin-poll the NC flag — it's instantly
            // visible cross-CPU and the wait is short. WFE here
            // would need worker-side SEV which adds latency.
            core::hint::spin_loop();
        }
        let mb_rows_lh = vread_u64(rows_lo_hi_p);
        let mb_row_start = (mb_rows_lh & 0xFFFF_FFFF) as usize;
        let mb_rows = ((mb_rows_lh >> 32) & 0xFFFF_FFFF) as usize;
        let slice_bytes = mb_rows * core::mem::size_of::<f32>();
        let slice_ptr = unsafe {
            (out_fp32.as_mut_ptr() as *mut u8).add(mb_row_start * core::mem::size_of::<f32>())
        };
        unsafe { kernel_ffi::slm_cache_invalidate_range(slice_ptr, slice_bytes) };
    }
    Some(())
}

/// Run the single-threaded NEON path. Used as the fallback when the
/// worker pool isn't ready or the workload is too small to benefit
/// from parallelism.
fn run_single_threaded(
    weights: &[u8],
    rows: usize,
    row_bytes: usize,
    acts_q8k: &[u8],
    out_fp32: &mut [f32],
) -> Option<()> {
    use crate::inference::quant::vec_dot_q4_k_q8_k;
    for r in 0..rows {
        let w_row = &weights[r * row_bytes..(r + 1) * row_bytes];
        out_fp32[r] = vec_dot_q4_k_q8_k(w_row, acts_q8k)?;
    }
    Some(())
}

// ---------------------------------------------------------------------------
// Diagnostic — dump per-worker run counts
// ---------------------------------------------------------------------------

/// Print the per-worker `runs_completed` counter to UART. Invoked
/// from the shell via `bench matmul_par_diag` to confirm workers
/// are actually waking + processing dispatched slices (vs sitting
/// in WFE while the dispatcher does all the work itself).
#[no_mangle]
pub extern "C" fn rust_matmul_par_diag() {
    extern "C" {
        fn shell_printf(fmt: *const u8, ...);
    }
    let initialized = INITIALIZED.load(Ordering::Acquire);
    let workers = WORKER_COUNT.load(Ordering::Acquire);
    unsafe {
        shell_printf(
            b"Parallel matmul diag: initialized=%lu workers=%lu\n\0".as_ptr(),
            initialized as u64, workers as u64,
        );
    }
    for cpu in 1..MAX_CPUS {
        let mb_ptr = MAILBOXES[cpu].load(Ordering::Acquire);
        if mb_ptr.is_null() {
            continue;
        }
        let runs_p   = unsafe { &raw mut (*mb_ptr).runs_completed };
        let state_p  = unsafe { &raw mut (*mb_ptr).state };
        let done_p   = unsafe { &raw mut (*mb_ptr).done_flag };
        let runs  = vread_u64(runs_p);
        let state = vread_u32(state_p);
        let done  = vread_u32(done_p);
        unsafe {
            shell_printf(
                b"  mmw%lu: runs=%lu state=%lu done=%lu\n\0".as_ptr(),
                cpu as u64, runs, state as u64, done as u64,
            );
        }
    }
}

// ---------------------------------------------------------------------------
// Bit-equality self-test (called from rust_run_tests)
// ---------------------------------------------------------------------------

/// Run a randomized check that `matmul_q4k_rows_parallel` produces
/// bit-identical results to `matmul_q4k_rows` across `trials`
/// random row-shape inputs. Returns the number of mismatches (0 ==
/// pass). Always returns 0 when the worker pool isn't initialized
/// (no parallelism to test).
///
/// Wired into `rust_run_tests` so on-target `make test` catches a
/// regression in the parallel dispatcher.
pub fn matmul_parallel_matches_serial_self_test(trials: u32) -> u32 {
    if !INITIALIZED.load(Ordering::Acquire) {
        return 0;
    }
    if WORKER_COUNT.load(Ordering::Acquire) == 0 {
        return 0;
    }

    use crate::inference::ops_transformer::matmul_q4k_rows;
    use crate::inference::quant::{Q8_K_BLOCK_ELEMENTS, Q8_K_BLOCK_SIZE};
    use crate::slm::gguf::{f32_to_f16, q4_k_byte_size, Q4_K_BLOCK_SIZE};

    extern crate alloc;

    fn lcg_next(state: &mut u64) -> u64 {
        *state = state
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        *state
    }

    let mut rng: u64 = 0xBADC0FFEE0DDF00Du64;
    let mut mismatches: u32 = 0;
    // Use 256-element cols (1 Q4_K block) so the synthetic shape is
    // small enough to keep test time low while still exercising the
    // full dispatcher path. Pick `rows` per trial above
    // MIN_PARALLEL_ROWS so the parallel branch always fires.
    let cols = Q8_K_BLOCK_ELEMENTS;
    let row_bytes = q4_k_byte_size(cols).unwrap_or(Q4_K_BLOCK_SIZE);

    for _ in 0..trials {
        let rows = MIN_PARALLEL_ROWS + ((lcg_next(&mut rng) as usize) & 0xFF);

        let mut weights = alloc::vec![0u8; rows * row_bytes];
        for byte in weights.iter_mut() {
            *byte = (lcg_next(&mut rng) & 0xFF) as u8;
        }

        // FP16 random activations. Both the serial reference
        // (matmul_q4k_rows) and the parallel dispatcher take FP16
        // acts and do the same internal Q8_K quantize, so bit-
        // equality is well-defined end-to-end.
        let mut acts_fp16 = alloc::vec![0u16; cols];
        for slot in acts_fp16.iter_mut() {
            // Random f32 in [-1, 1] then round-trip to FP16.
            let f = (lcg_next(&mut rng) as i32) as f32 / (i32::MAX as f32);
            *slot = f32_to_f16(f);
        }

        let mut q8k_scratch_serial = alloc::vec![0u8; Q8_K_BLOCK_SIZE];
        let mut q8k_scratch_par    = alloc::vec![0u8; Q8_K_BLOCK_SIZE];
        let mut out_serial = alloc::vec![0.0f32; rows];
        let mut out_parallel = alloc::vec![0.0f32; rows];

        if matmul_q4k_rows(&weights, rows, cols, &acts_fp16,
                           &mut q8k_scratch_serial, &mut out_serial).is_none() {
            mismatches += 1;
            continue;
        }
        if matmul_q4k_rows_parallel(&weights, rows, cols, &acts_fp16,
                                    &mut q8k_scratch_par, &mut out_parallel).is_none() {
            mismatches += 1;
            continue;
        }

        for r in 0..rows {
            if out_serial[r].to_bits() != out_parallel[r].to_bits() {
                mismatches += 1;
                break;
            }
        }
    }
    mismatches
}
