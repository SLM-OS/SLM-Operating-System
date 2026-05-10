//! Bare-metal SLM-OS GPU backend (M6.A scaffolding).
//!
//! Reads the `slm_gpu_handoff_v1` struct staged by the pre-kexec L4T
//! loader (`scripts/slm-gpu-bringup.c`, M6.A-2 — not yet authored)
//! and exposes a `Backend` trait the M5 decoder dispatches through.
//!
//! Status: **structural skeleton only**. The actual SASS kernels
//! (M6.B Tier-1 HMMA, M6.C Tier-2 CUDA-core, M6.D element-wise) and
//! the pushbuffer / semaphore-poll dispatch are weeks of work
//! deferred per `docs/design/gpu-slm-handoff.md`. Every entry point
//! currently returns `BackendError::NotAvailable`, which the
//! M4-CPU-fallback path catches.
//!
//! See `docs/design/gpu-slm-handoff.md` for the full design and
//! per-section bring-up plan.

#![cfg(feature = "slm")]

use core::sync::atomic::{AtomicU8, Ordering};

/// Op kind. Mirror of `enum slm_gpu_op_kind` in
/// `kernel/include/gpu_handoff.h` — keep the discriminants in sync.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum OpKind {
    RmsNorm   = 0,
    Rope      = 1,
    Embedding = 2,
    Q4kDot    = 3,
    Q4kGemm   = 4,
    GqaAttn   = 5,
    SwiGlu    = 6,
    LmHead    = 7,
}

impl OpKind {
    /// Total number of op kinds. Keep in sync with the enum.
    pub const COUNT: usize = 8;
}

/// Tier preference. Mirror of `SLM_GPU_TIER_*` in
/// `kernel/include/gpu_handoff.h`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Tier {
    /// Pick Tier1 if available; fall back to Tier2 then CPU.
    Auto = 0,
    /// Tier 1: HMMA tensor cores. Highest throughput.
    Hmma = 1,
    /// Tier 2: CUDA-core FP16 SIMT. Slower but always present.
    Simt = 2,
    /// Skip GPU entirely; route to the M4 CPU NEON path.
    Cpu  = 3,
}

/// Backend error codes. The CPU fallback path treats every variant
/// the same — any error means "fall back to CPU". Differentiated
/// for diagnostics in `slm gpu`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BackendError {
    /// The handoff page was never staged or has bad magic / version.
    NoHandoff,
    /// The op kind has no kernel for the requested tier.
    NotAvailable,
    /// Slice / dimension mismatch with the op descriptor.
    BadShape,
    /// The semaphore poll timed out.
    Timeout,
}

// =====================================================================
// Handoff parser
// =====================================================================

const SLM_GPU_HANDOFF_MAGIC: u32 = 0x534C4D47;
const SLM_GPU_HANDOFF_VERSION: u32 = 1;

/// Full mirror of `slm_gpu_handoff_v1_t` from
/// `kernel/include/gpu_handoff.h`. The C side pins the header at
/// exactly 120 bytes via `_Static_assert(sizeof(...) == 120)`; the
/// Rust mirror has a matching compile-time check below
/// (`HANDOFF_HEADER_SIZE_BYTES`). The whole struct is reproduced —
/// not just the leading fields — so `op_desc_ptr` arithmetic
/// advances by the *real* header size, not a stale subset.
#[repr(C)]
struct HandoffHeader {
    /* Schema gates. */
    magic: u32,
    version: u32,
    op_count: u32,
    arch_kind: u32,

    /* Architecture dimensions (mirrors ArchInfo from gguf.rs). */
    block_count: u32,
    embedding_length: u32,
    head_count: u32,
    head_count_kv: u32,
    head_dim: u32,
    feed_forward_length: u32,
    context_length: u32,
    vocab_size: u32,

    /* Channel resources (inherited from L4T's nvgpu). */
    channel_id: u64,
    pushbuffer_va: u64,
    pushbuffer_size: u64,
    semaphore_page_va: u64,
    doorbell_page_va: u64,

    /* Weight pool. */
    weight_pool_va: u64,
    weight_pool_size: u64,

    /* SASS kernel pool. */
    sass_kernel_pool_va: u64,
    sass_kernel_pool_size: u64,
}

/// Size in bytes of the C `slm_gpu_handoff_v1_t`. Pinned by both
/// `_Static_assert` on the C side and the const-assert below; if a
/// future field addition trips this, bump `SLM_GPU_HANDOFF_VERSION`
/// and update both anchors.
pub const HANDOFF_HEADER_SIZE_BYTES: usize = 120;

const _: () = {
    if core::mem::size_of::<HandoffHeader>() != HANDOFF_HEADER_SIZE_BYTES {
        panic!("HandoffHeader size drifted from C slm_gpu_handoff_v1_t");
    }
};

/// Read-only view onto the staged handoff page.
///
/// Returns `None` from `try_from_kernel` whenever the kernel-side
/// FFI returns 0 (no page staged) or magic/version don't match.
/// M5 callers fall back to CPU on `None`.
pub struct Handoff {
    /// Op count from the header.
    op_count: u32,
    /// Architecture kind (0 = qwen2, 1 = llama, …).
    arch_kind: u32,
    /// Pointer to the first `slm_gpu_op_desc_t`. The op_descs follow
    /// the header in memory; this struct does not own them — the
    /// kernel-managed page does.
    _op_desc_ptr: *const u8,
    /// Phantom: this struct is logically `'static` because the
    /// underlying page is permanent for the kernel's lifetime.
    _phantom: core::marker::PhantomData<&'static ()>,
}

// SAFETY: `Handoff` is a read-only window into a kernel-managed
// page that is never mutated post-kexec, so concurrent reads from
// multiple threads are sound. The raw pointer is never used for
// mutation.
unsafe impl Send for Handoff {}
unsafe impl Sync for Handoff {}

impl Handoff {
    /// Try to attach to the staged handoff page. Returns `None` if
    /// no page is available (kernel hasn't staged it yet, or M6.A-2
    /// isn't wired) or the page header fails magic/version checks.
    /// M5 callers fall back to CPU when this returns `None`.
    ///
    /// # M6.A-3 re-review hook
    ///
    /// Today this is a stub: `slm_gpu_get_handoff_phys` returns 0 on
    /// every platform, so the magic/version check + later reads in
    /// the `Some(Self { ... })` arm are unreachable. When M6.A-3
    /// wires the real pre-kexec handoff loader, the magic/version
    /// validation below has a TOCTOU window — the kernel could in
    /// principle re-stage the page between the magic check (line
    /// 179-183) and downstream reads of `op_count` / `arch_kind`.
    /// At that point either:
    ///   1. Document that the kernel must not mutate the staged
    ///      page after `slm_gpu_get_handoff_phys` first returns
    ///      non-zero (matches the read-only `'static` bound on
    ///      `Handoff`'s PhantomData), OR
    ///   2. Read the entire header into a local copy via
    ///      `core::ptr::read_volatile` and validate the local
    ///      copy's fields before constructing `Self`.
    /// Pinning this here so the M6.A-3 PR reviewer doesn't have to
    /// rediscover the constraint.
    pub fn try_from_kernel() -> Option<Self> {
        // SAFETY: bare-metal FFI; the kernel-side stub returns either
        // 0 (no page staged) or a kernel-vouched physical address that
        // is mapped readable.
        let phys = unsafe { ffi::slm_gpu_get_handoff_phys() };
        if phys == 0 {
            return None;
        }

        // SAFETY: `phys` is non-zero and the kernel guarantees it
        // points at a mapped, readable, struct-aligned page
        // containing exactly one `slm_gpu_handoff_v1_t` followed by
        // its op_descs. Validate magic + version BEFORE reading any
        // other field (DoS gate — see CLAUDE.md "Checked arithmetic
        // at boundaries").
        let header_ptr = phys as *const HandoffHeader;
        let header = unsafe { &*header_ptr };
        if header.magic != SLM_GPU_HANDOFF_MAGIC
            || header.version != SLM_GPU_HANDOFF_VERSION
        {
            return None;
        }

        // SAFETY: `phys` is non-zero. Adding the size of a header
        // produces the address of the first op_desc; the kernel
        // staged at least the header bytes here.
        let op_desc_ptr = unsafe {
            (phys as *const u8).add(core::mem::size_of::<HandoffHeader>())
        };

        Some(Self {
            op_count: header.op_count,
            arch_kind: header.arch_kind,
            _op_desc_ptr: op_desc_ptr,
            _phantom: core::marker::PhantomData,
        })
    }

    /// Number of per-op descriptors that follow the header.
    pub fn op_count(&self) -> u32 {
        self.op_count
    }

    /// Architecture kind from the header (0 = qwen2, 1 = llama).
    pub fn arch_kind(&self) -> u32 {
        self.arch_kind
    }
}

// =====================================================================
// Backend trait + stub
// =====================================================================

/// The trait M5 dispatches through. M6.A-3 wires the actual
/// pushbuffer generation / semaphore polling for each op kind on
/// top of this; until then, callers see `StubBackend` which always
/// reports `NotAvailable` so the engine falls through to CPU.
pub trait Backend {
    fn execute(
        &self,
        op_kind: OpKind,
        tier: Tier,
        weight_va: u64,
        input: &[u8],
        output: &mut [u8],
    ) -> Result<(), BackendError>;
}

/// Stub backend used when no handoff is available. Always errors
/// out to `NotAvailable` so M5 falls through to CPU.
pub struct StubBackend;

impl Backend for StubBackend {
    fn execute(
        &self,
        _op_kind: OpKind,
        _tier: Tier,
        _weight_va: u64,
        _input: &[u8],
        _output: &mut [u8],
    ) -> Result<(), BackendError> {
        Err(BackendError::NotAvailable)
    }
}

// =====================================================================
// OperatorLibraryBackend — real GPU-dispatch backend (#714 §B.3)
// =====================================================================
//
// The original `Backend` trait above is one-input/one-output shaped,
// which doesn't fit ops like RMSNORM (x + gamma → out) or GQA_ATTN
// (Q + K + V → out). `OperatorLibraryBackend` instead exposes per-op
// methods whose signatures match each op's natural shape; forward.rs
// calls the one matching the op it's about to execute. The methods
// route through per-op extern "C" FFI shims that handle the
// CPU↔GPU staging round-trip via the helper-staged scratch slots.
//
// Self-gating: each method first reads `select_tier(op_kind)` and
// returns `BackendError::NotAvailable` if the entry is `Tier::Cpu`
// (i.e. the boot probe in #714 §B.2 didn't flip it to `Tier::Simt`).
// That keeps the dispatch path inert on platforms / boot states
// where the operator library isn't staged — forward.rs's CPU
// fallback then handles the op as it does today.

/// Real backend for the SLM operator library. Sits behind every
/// hybrid-forward call site in `runtime/src/slm/forward.rs`; per-op
/// methods route through FFI to `slm_oplib_dispatch` on the kernel
/// side.
pub struct OperatorLibraryBackend;

#[cfg(not(test))]
unsafe extern "C" {
    /// See `kernel/include/slm_ffi.h` for the full contract.
    fn slm_runtime_dispatch_rmsnorm_simt(
        x_cpu_in: *const u8,
        gamma_cpu_in: *const u8,
        out_cpu_out: *mut u8,
        n_rows: u32,
        n: u32,
        eps_bits: u32,
    ) -> i32;

    /// W2: stage `len` bytes of CPU-resident weight data into the
    /// GPU weights pool. Returns 0 + writes `*out_gpu_va` on success;
    /// returns -1 on null arg, zero len, missing v8 handoff, pool
    /// exhausted, or per-page GMMU walk failure. See
    /// `kernel/include/slm_ffi.h` for the full contract.
    fn slm_runtime_stage_weight(
        cpu_bytes: *const u8,
        len: u64,
        out_gpu_va: *mut u64,
    ) -> i32;
}

/// `cargo test` doesn't link the kernel-side FFI; the host-side
/// stub returns -1 so backend tests can verify the
/// `BackendError::NotAvailable` fall-through path without staging
/// a real GPU channel. Marked `unsafe` to match the production
/// `extern "C"` signature so callers don't trip the
/// `unused_unsafe` lint when both branches compile.
#[cfg(test)]
unsafe fn slm_runtime_dispatch_rmsnorm_simt(
    _x_cpu_in: *const u8,
    _gamma_cpu_in: *const u8,
    _out_cpu_out: *mut u8,
    _n_rows: u32,
    _n: u32,
    _eps_bits: u32,
) -> i32 {
    -1
}

/// `cargo test` stub for `slm_runtime_stage_weight`. Same
/// "always-fail" shape as the dispatch stubs so backend tests can
/// verify the `BackendError::NotAvailable` fall-through without
/// staging a real GPU channel.
#[cfg(test)]
unsafe fn slm_runtime_stage_weight(
    _cpu_bytes: *const u8,
    _len: u64,
    _out_gpu_va: *mut u64,
) -> i32 {
    -1
}

impl OperatorLibraryBackend {
    /// Dispatch an RMSNORM through the GPU operator library. Returns
    /// `Ok(())` on success (output bytes written to `out`), or
    /// `BackendError::NotAvailable` if the tier table says CPU or
    /// the FFI failed (no handoff, dispatch error, etc.). Caller —
    /// usually `forward.rs` — falls back to the CPU NEON kernel on
    /// any error.
    ///
    /// Shapes: `x` and `out` are `[n_rows × n]` FP16; `gamma` is
    /// `[n]` FP16. `eps` is converted to its IEEE 754 FP32 bit
    /// pattern at the FFI boundary because the kernel build forbids
    /// FP types (`-mgeneral-regs-only`); the Rust side handles the
    /// conversion since FP arithmetic is allowed here.
    pub fn dispatch_rmsnorm(
        x: &[u16],
        gamma: &[u16],
        out: &mut [u16],
        n_rows: u32,
        n: u32,
        eps: f32,
    ) -> Result<(), BackendError> {
        if select_tier(OpKind::RmsNorm) != Tier::Simt {
            return Err(BackendError::NotAvailable);
        }
        /* Defensive shape checks: the FFI itself validates these but
         * surfacing them here as `BadShape` rather than `NotAvailable`
         * helps diagnose forward.rs bugs vs missing-staging. */
        let expected = (n_rows as usize) * (n as usize);
        if x.len() != expected || out.len() != expected ||
           gamma.len() != (n as usize) {
            return Err(BackendError::BadShape);
        }
        if n_rows == 0 || n == 0 {
            return Err(BackendError::BadShape);
        }

        let eps_bits = eps.to_bits();
        // SAFETY: `x`, `gamma` are valid for `expected*2 / n*2` bytes
        // respectively; `out` is valid for `expected*2` bytes mutable.
        // The FFI copies these into GPU scratch slots and writes the
        // result back into `out`'s memory; no aliasing because `out`
        // is a `&mut` slice and `x` / `gamma` are `&` slices. The
        // FFI never retains the pointers past return.
        let rc = unsafe {
            slm_runtime_dispatch_rmsnorm_simt(
                x.as_ptr() as *const u8,
                gamma.as_ptr() as *const u8,
                out.as_mut_ptr() as *mut u8,
                n_rows,
                n,
                eps_bits,
            )
        };
        if rc != 0 {
            return Err(BackendError::NotAvailable);
        }
        Ok(())
    }

    /// W2: stage `bytes` into the GPU weights pool. Returns the
    /// `(gpu_va, size)` pair that `slm load` (W3) records in the
    /// model's `gpu_tensor_map`. On any failure (no v8 handoff,
    /// pool exhausted, walk failure) returns
    /// `BackendError::NotAvailable` — the load path treats it as
    /// "skip this tensor; forward.rs falls through to CPU".
    ///
    /// Empty input is rejected with `BadShape` to surface caller
    /// bugs rather than silently no-op.
    pub fn stage_tensor(bytes: &[u8]) -> Result<crate::slm::registry::GpuTensorRef,
                                                BackendError> {
        if bytes.is_empty() {
            return Err(BackendError::BadShape);
        }
        let mut gpu_va: u64 = 0;
        // SAFETY: `bytes` is valid for `bytes.len()` bytes for the
        // duration of the call; the FFI copies and does not retain
        // the pointer. `&mut gpu_va` is exclusive.
        let rc = unsafe {
            slm_runtime_stage_weight(
                bytes.as_ptr(),
                bytes.len() as u64,
                &mut gpu_va as *mut u64,
            )
        };
        if rc != 0 {
            return Err(BackendError::NotAvailable);
        }
        Ok(crate::slm::registry::GpuTensorRef {
            gpu_va,
            size_bytes: bytes.len(),
        })
    }
}

// =====================================================================
// Per-op tier preference table
// =====================================================================

/// Per-op tier preference, defaulting to CPU on every op. The
/// session-launch smoke test (M6.A-4) flips entries to `Hmma` /
/// `Simt` after dispatching a tiny test kernel for each op kind.
/// On a fresh boot — and during M6.A-1 scaffolding — the table
/// stays all-CPU so the engine routes every op to the M4 NEON
/// path.
static TIER_TABLE: [AtomicTier; OpKind::COUNT] = [
    AtomicTier::new(Tier::Cpu), // RmsNorm
    AtomicTier::new(Tier::Cpu), // Rope
    AtomicTier::new(Tier::Cpu), // Embedding
    AtomicTier::new(Tier::Cpu), // Q4kDot
    AtomicTier::new(Tier::Cpu), // Q4kGemm
    AtomicTier::new(Tier::Cpu), // GqaAttn
    AtomicTier::new(Tier::Cpu), // SwiGlu
    AtomicTier::new(Tier::Cpu), // LmHead
];

/// Look up the preferred tier for an op. Reads are lock-free.
pub fn select_tier(op_kind: OpKind) -> Tier {
    TIER_TABLE[op_kind as usize].load()
}

/// Set the preferred tier for an op. The session-launch smoke
/// test calls this after probing each Tier-1 kernel.
pub fn set_tier(op_kind: OpKind, tier: Tier) {
    TIER_TABLE[op_kind as usize].store(tier);
}

/// Reset every op back to CPU. Used by tests; future uses include
/// a `slm gpu reset` shell verb that re-runs the smoke probes.
#[cfg(test)]
fn reset_tier_table() {
    for slot in TIER_TABLE.iter() {
        slot.store(Tier::Cpu);
    }
}

/// FFI shim for the C-side oplib probe (#714 §B.2). Called once per
/// successfully-probed op_kind; flips the matching `TIER_TABLE`
/// entry from `Tier::Cpu` (the boot default) to `Tier::Simt` so the
/// SLM forward path routes that op through the GPU dispatcher
/// instead of the CPU NEON kernel.
///
/// Returns 0 on success, -1 if `op_kind` is out of range. Never
/// panics; safe to call from C without an unwind handler. Caller
/// responsibility: only invoke after the dispatcher has validated
/// the op (today: registry hit + cbuf/launch-shape build accepted
/// a fixture; once #714 §B.3 lands: real Backend::execute against
/// the embedded CPU reference).
///
/// SAFETY: no pointer arguments; bounds-checks `op_kind` before
/// indexing `TIER_TABLE`. The atomic store is lock-free, so
/// concurrent callers can't corrupt the table.
#[cfg(not(test))]
#[unsafe(no_mangle)]
pub extern "C" fn slm_runtime_set_tier_simt(op_kind: u32) -> i32 {
    if (op_kind as usize) >= OpKind::COUNT {
        return -1;
    }
    TIER_TABLE[op_kind as usize].store(Tier::Simt);
    0
}

// =====================================================================
// FFI shim
// =====================================================================
//
// Real bare-metal builds link against the C symbol declared in
// `kernel/include/slm_ffi.h`. Hosted unit tests (cargo test) use
// the `#[cfg(test)]` override below so they don't need the kernel
// linked in.

#[cfg(not(test))]
mod ffi {
    unsafe extern "C" {
        /// Returns the physical address of the staged handoff page,
        /// or 0 if no page is staged. Defined in C kernel code
        /// (`kernel/src/slm_ffi.c`, weak stub today; M6.A-2 will
        /// replace it with the real lookup). For now the C side
        /// returns 0 unconditionally — the FFI is wired so this
        /// module compiles into the kernel image.
        pub fn slm_gpu_get_handoff_phys() -> u64;
    }
}

#[cfg(test)]
mod ffi {
    use core::sync::atomic::{AtomicU64, Ordering};

    /// Test-side override of the C extern. Tests can drive this
    /// with `set_test_phys` to simulate "no handoff" vs "handoff at
    /// PA X" without linking the kernel.
    static TEST_HANDOFF_PHYS: AtomicU64 = AtomicU64::new(0);

    pub unsafe fn slm_gpu_get_handoff_phys() -> u64 {
        TEST_HANDOFF_PHYS.load(Ordering::Relaxed)
    }

    pub fn set_test_phys(phys: u64) {
        TEST_HANDOFF_PHYS.store(phys, Ordering::Relaxed);
    }
}

// =====================================================================
// AtomicTier — atomic wrapper around `Tier`
// =====================================================================

/// `core::sync::atomic` does not have a generic enum atomic, so wrap
/// `AtomicU8` with safe `Tier` accessors.
struct AtomicTier(AtomicU8);

impl AtomicTier {
    const fn new(t: Tier) -> Self {
        Self(AtomicU8::new(t as u8))
    }

    fn load(&self) -> Tier {
        match self.0.load(Ordering::Relaxed) {
            0 => Tier::Auto,
            1 => Tier::Hmma,
            2 => Tier::Simt,
            _ => Tier::Cpu,
        }
    }

    fn store(&self, t: Tier) {
        self.0.store(t as u8, Ordering::Relaxed);
    }
}

// =====================================================================
// Tests (hosted)
// =====================================================================

#[cfg(test)]
mod tests {
    use super::*;

    /// Helper: every test that touches `TIER_TABLE` should reset it
    /// first since the table is global. `cargo test` runs tests
    /// concurrently by default — combined with the global FFI
    /// override these tests must run single-threaded
    /// (`cargo test -- --test-threads=1`); document that here so a
    /// future maintainer doesn't get confused by spurious flakes.
    fn reset_globals() {
        ffi::set_test_phys(0);
        reset_tier_table();
    }

    #[test]
    fn try_from_kernel_returns_none_when_phys_is_zero() {
        reset_globals();
        ffi::set_test_phys(0);
        assert!(Handoff::try_from_kernel().is_none());
    }

    #[test]
    fn tier_table_defaults_to_cpu() {
        reset_globals();
        let kinds = [
            OpKind::RmsNorm,   OpKind::Rope,    OpKind::Embedding,
            OpKind::Q4kDot,    OpKind::Q4kGemm, OpKind::GqaAttn,
            OpKind::SwiGlu,    OpKind::LmHead,
        ];
        for k in kinds {
            assert_eq!(select_tier(k), Tier::Cpu, "{:?} should default to Cpu", k);
        }
    }

    #[test]
    fn tier_table_set_then_get_round_trips() {
        reset_globals();
        set_tier(OpKind::Q4kDot, Tier::Hmma);
        assert_eq!(select_tier(OpKind::Q4kDot), Tier::Hmma);

        set_tier(OpKind::Q4kDot, Tier::Simt);
        assert_eq!(select_tier(OpKind::Q4kDot), Tier::Simt);

        set_tier(OpKind::Q4kDot, Tier::Auto);
        assert_eq!(select_tier(OpKind::Q4kDot), Tier::Auto);

        set_tier(OpKind::Q4kDot, Tier::Cpu);
        assert_eq!(select_tier(OpKind::Q4kDot), Tier::Cpu);
    }

    #[test]
    fn stub_backend_always_returns_not_available() {
        reset_globals();
        let backend = StubBackend;
        let kinds = [
            OpKind::RmsNorm,   OpKind::Rope,    OpKind::Embedding,
            OpKind::Q4kDot,    OpKind::Q4kGemm, OpKind::GqaAttn,
            OpKind::SwiGlu,    OpKind::LmHead,
        ];
        let tiers = [Tier::Auto, Tier::Hmma, Tier::Simt, Tier::Cpu];
        let mut input  = [0u8; 16];
        let mut output = [0u8; 16];
        for k in kinds {
            for t in tiers {
                let r = backend.execute(k, t, 0, &mut input, &mut output);
                assert_eq!(r, Err(BackendError::NotAvailable),
                           "kind={:?} tier={:?} should be NotAvailable", k, t);
            }
        }
    }

    /* OperatorLibraryBackend::dispatch_rmsnorm — #714 §B.3 hosted
     * tests. The kernel-side FFI is stubbed to always return -1 in
     * `cargo test` (the `#[cfg(test)]` shim above), so dispatch
     * goes through the early `Tier::Cpu` short-circuit when the
     * tier table holds the boot default, and falls through to the
     * "FFI returned -1 → NotAvailable" branch when the tier is
     * forced to Simt. Both paths surface as `NotAvailable` to the
     * caller so forward.rs's CPU fallback fires; the tests below
     * verify that mapping plus the BadShape pre-flight checks. */

    #[test]
    fn oplib_backend_returns_not_available_when_tier_is_cpu() {
        reset_globals();
        /* Boot default for every op is Tier::Cpu. */
        assert_eq!(select_tier(OpKind::RmsNorm), Tier::Cpu);
        let x = [0u16; 256];
        let gamma = [0u16; 256];
        let mut out = [0u16; 256];
        let r = OperatorLibraryBackend::dispatch_rmsnorm(
            &x, &gamma, &mut out, 1, 256, 1.0e-6,
        );
        assert_eq!(r, Err(BackendError::NotAvailable));
    }

    #[test]
    fn oplib_backend_returns_not_available_when_ffi_fails() {
        reset_globals();
        /* Force Simt so the tier short-circuit doesn't fire; the
         * cfg(test) FFI stub returns -1, which dispatch_rmsnorm
         * maps to NotAvailable. */
        set_tier(OpKind::RmsNorm, Tier::Simt);
        let x = [0u16; 256];
        let gamma = [0u16; 256];
        let mut out = [0u16; 256];
        let r = OperatorLibraryBackend::dispatch_rmsnorm(
            &x, &gamma, &mut out, 1, 256, 1.0e-6,
        );
        assert_eq!(r, Err(BackendError::NotAvailable));
    }

    #[test]
    fn oplib_backend_rejects_shape_mismatch() {
        reset_globals();
        set_tier(OpKind::RmsNorm, Tier::Simt);
        let x = [0u16; 256];
        let gamma = [0u16; 128];   /* gamma.len() != n */
        let mut out = [0u16; 256];
        let r = OperatorLibraryBackend::dispatch_rmsnorm(
            &x, &gamma, &mut out, 1, 256, 1.0e-6,
        );
        assert_eq!(r, Err(BackendError::BadShape));
    }

    #[test]
    fn oplib_backend_rejects_zero_dims() {
        reset_globals();
        set_tier(OpKind::RmsNorm, Tier::Simt);
        let x: [u16; 0]     = [];
        let gamma: [u16; 0] = [];
        let mut out: [u16; 0] = [];
        let r = OperatorLibraryBackend::dispatch_rmsnorm(
            &x, &gamma, &mut out, 0, 0, 1.0e-6,
        );
        assert_eq!(r, Err(BackendError::BadShape));
    }

    /* W2 stage_tensor — backend-side shape gate + FFI fall-through. */

    #[test]
    fn stage_tensor_rejects_empty_input() {
        let r = OperatorLibraryBackend::stage_tensor(&[]);
        assert_eq!(r, Err(BackendError::BadShape));
    }

    #[test]
    fn stage_tensor_returns_not_available_when_ffi_fails() {
        /* cfg(test) stub returns -1 unconditionally, mirroring the
         * kernel-side path on a non-Jetson build (no v8 handoff,
         * pool size = 0). */
        let bytes = [0u8; 64];
        let r = OperatorLibraryBackend::stage_tensor(&bytes);
        assert_eq!(r, Err(BackendError::NotAvailable));
    }

    #[test]
    fn handoff_header_size_matches_c_struct() {
        // Pinned by `_Static_assert(sizeof(slm_gpu_handoff_v1_t) == 120)`
        // in kernel/include/gpu_handoff.h and the const-assert at the
        // top of this module. Catches drift early so `op_desc_ptr`
        // arithmetic doesn't silently land inside the header.
        assert_eq!(
            core::mem::size_of::<HandoffHeader>(),
            HANDOFF_HEADER_SIZE_BYTES
        );
        assert_eq!(HANDOFF_HEADER_SIZE_BYTES, 120);
    }
}
