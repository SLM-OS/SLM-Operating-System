//! SLM registry — slot-table for loaded GGUF models.
//!
//! Phase-5's `loader::registry` is purpose-built for ONNX vision
//! models (parsed graph, weight-table-into-pool, MNIST shortcuts).
//! GGUFs have a different shape: ~339 tensors organized by name,
//! Q4_K-packed weights (decoded on-the-fly during MatMul), and a
//! per-arch `ArchInfo` that drives the M5 decoder. Rather than
//! widening the ONNX entry to carry both worlds, M1.4 ships a
//! parallel slot table here.
//!
//! M1.4 stored **only metadata** — enough for `slm info` to report
//! architecture, layer count, hidden size, vocab size, etc. M5.3.1
//! extends each slot to **own the GGUF byte buffer in the weight
//! pool** plus a pre-built [`Bbpe`] tokenizer and a snapshot of
//! tensor descriptors. Sessions opened against a slot reuse the
//! tokenizer and look up tensor bytes by name — there's no separate
//! reparse, and the source buffer the caller passed to [`load_slm`]
//! can be freed immediately after the call returns.

use core::sync::atomic::{AtomicBool, Ordering};

use alloc::string::String;
use alloc::vec::Vec;

use crate::kernel_ffi;
use crate::mm;
use crate::mm::model_loader::LoadError;
use crate::mm::ModelHandle;
use crate::slm::gguf::{
    q4_k_byte_size, ArchInfo, ArchKind, GgmlType, Gguf, GgufError, MetaValue,
};
use crate::slm::tokenizer::Bbpe;

/// Maximum number of SLMs that can be loaded concurrently. The
/// Jetson SLM weight pool sized in M0.2 (2 GB) realistically only
/// fits two or three models at this quant; the slot count is small
/// on purpose.
pub const SLM_MAX_SLOTS: usize = 4;

/// Maximum length (in bytes) of the architecture string in
/// [`SlmModelInfoC`]. "qwen2" / "llama" fit easily.
pub const SLM_ARCH_LEN: usize = 16;

/// Maximum length of a model's user-facing name.
pub const SLM_NAME_LEN: usize = 32;

/// Reject GGUFs whose source-byte size exceeds 2 GB. Mirrors the
/// shell-side cap (`docs/plans/slm-integration-plan.md` §M7) and the
/// telemetry `source_bytes: u32` field. Anything above this is almost
/// certainly a corrupted header field rather than a genuine model.
pub const MAX_PLAUSIBLE_GGUF_BYTES: usize = 2 * 1024 * 1024 * 1024;

/// Snapshot of one tensor descriptor, owned by the registry slot.
///
/// The borrow-based [`crate::slm::gguf::TensorInfo`] ties tensor names
/// to the lifetime of the parsed GGUF buffer. M5.3.1 needs to free
/// that parser as soon as the load completes, so each tensor's
/// metadata is cloned into an owning struct here. The `offset` is
/// **relative to the tensor-data section start** — combine with
/// [`LoadedSlm::tensor_data_start`] to get the file-absolute offset
/// inside the owned weight buffer.
#[derive(Debug, Clone)]
pub struct OwnedTensorInfo {
    /// Tensor name (e.g. `"blk.0.attn_q.weight"`).
    pub name: String,
    /// Per-dimension element counts (typically 1-4 entries).
    pub dims: Vec<u64>,
    /// GGML element-type tag (raw `u32`; well-known values in
    /// [`GgmlType`]).
    pub ggml_type: u32,
    /// Offset (in bytes) of this tensor's data, relative to the
    /// tensor-data section start.
    pub offset: u64,
}

/// Per-slot record in the SLM registry. Exposed publicly only via
/// the closure-based [`with_loaded_slm`] accessor — the slot table
/// owns the value, callers borrow it under the registry lock.
pub struct LoadedSlm {
    name: String,
    info: ArchInfo,
    vocab_size: u32,
    /// On-disk size of the GGUF bytes used to load this model (for
    /// telemetry; not the resident memory size).
    source_bytes: u32,
    /// Total number of tensor descriptors in the GGUF — proxy for
    /// "complexity" the shell can surface to the user.
    tensor_count: u32,

    /// Owned copy of the entire GGUF byte buffer in the weight pool.
    /// Held for the lifetime of this slot; freed in
    /// [`unload_slm`]. Allocated via [`mm::alloc_weights`], which
    /// returns a 2 MB-aligned block. The first
    /// [`Self::weight_data_len`] bytes are valid; the rest is padding
    /// up to the next 2 MB block boundary.
    ///
    /// `None` only in degenerate states (allocation failed during
    /// load and the slot was never inserted) — once a slot is
    /// occupied, this is always `Some`.
    weight_block: Option<ModelHandle>,
    /// Number of valid bytes inside [`Self::weight_block`].
    weight_data_len: usize,

    /// Pre-built tokenizer for this model. Sessions reuse this via
    /// [`with_loaded_slm`] / [`LoadedSlm::tokenizer`] rather than
    /// reparsing the GGUF on every prompt.
    tokenizer: Option<Bbpe>,

    /// Cached tensor descriptors. Looking up a tensor by name during
    /// `forward_step` doesn't have to re-walk the GGUF.
    tensors: Vec<OwnedTensorInfo>,

    /// File-absolute offset where the tensor-data section begins in
    /// [`Self::weight_block`]. Adding a tensor's relative `offset`
    /// gives the absolute byte address of that tensor's data.
    tensor_data_start: usize,
}

impl LoadedSlm {
    /// The architecture metadata extracted at load time. The decoder
    /// caches a copy on the [`crate::slm::session::Session`]; this
    /// accessor is for consumers (tests, `slm info`) that need to
    /// read it through the registry.
    pub fn arch(&self) -> &ArchInfo {
        &self.info
    }

    /// Tokenizer associated with this loaded model. Always `Some`
    /// once [`load_slm`] has succeeded.
    pub fn tokenizer(&self) -> Option<&Bbpe> {
        self.tokenizer.as_ref()
    }

    /// Look up a tensor descriptor by name. Linear scan over the
    /// snapshot — fine for ~340 tensors per Qwen2.5-1.5B.
    pub fn tensor_info(&self, name: &str) -> Option<&OwnedTensorInfo> {
        self.tensors.iter().find(|t| t.name == name)
    }

    /// All cached tensor descriptors in file order.
    pub fn tensors(&self) -> &[OwnedTensorInfo] {
        &self.tensors
    }

    /// Borrow this tensor's raw bytes from the owned weight buffer.
    /// Returns `None` if the tensor is missing, the slot's
    /// [`Self::weight_block`] isn't backed (degenerate state), or
    /// the computed byte range falls outside the buffer.
    pub fn tensor_bytes(&self, name: &str) -> Option<&[u8]> {
        let info = self.tensor_info(name)?;
        let n_elements = elements_of(&info.dims)?;
        let size = ggml_type_byte_size(info.ggml_type, n_elements)? as usize;
        let abs_start = self.tensor_data_start.checked_add(info.offset as usize)?;
        let abs_end = abs_start.checked_add(size)?;
        let bytes = self.weight_buffer()?;
        if abs_end > bytes.len() {
            return None;
        }
        Some(&bytes[abs_start..abs_end])
    }

    /// Borrow the full owned weight buffer (header + tensor data).
    /// Returns `None` only in the degenerate "load failed before slot
    /// insert" state.
    pub fn weight_bytes(&self) -> Option<&[u8]> {
        self.weight_buffer()
    }

    /// File-absolute offset where the tensor-data section begins.
    /// Useful for tests; callers usually prefer
    /// [`Self::tensor_bytes`].
    pub fn tensor_data_start(&self) -> usize {
        self.tensor_data_start
    }

    fn weight_buffer(&self) -> Option<&[u8]> {
        let handle = self.weight_block.as_ref()?;
        let ptr = mm::get_ptr(*handle)?;
        if ptr.is_null() {
            return None;
        }
        // SAFETY: the ModelHandle was returned by `mm::alloc_weights`
        // and lives until `unload_slm` frees it. `weight_data_len`
        // bytes were written by `load_slm` via `copy_nonoverlapping`
        // and remain valid + initialized for the slot's lifetime.
        // The slice is read-only and never mutated through this view.
        let slice = unsafe { core::slice::from_raw_parts(ptr, self.weight_data_len) };
        Some(slice)
    }
}

/// Empty slot constant — usable in `static` initializers because
/// `Option::None` is `const`. A method form (`LoadedSlm::empty()`)
/// would require const-fn, which doesn't extend cleanly to `Option`
/// of a struct that owns a `String`.
const EMPTY_SLOT: Option<LoadedSlm> = None;

/// C-friendly view returned by [`get_info`]. Mirrors the layout the
/// `slm info <handle>` shell command expects.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct SlmModelInfoC {
    /// Null-padded ASCII architecture name (e.g. b"qwen2\0\0\0…").
    pub architecture: [u8; SLM_ARCH_LEN],
    /// Null-padded ASCII model name (whatever the caller passed to
    /// `rust_slm_load`).
    pub name: [u8; SLM_NAME_LEN],
    /// Decoder block (layer) count.
    pub block_count: u32,
    /// Hidden-state width.
    pub embedding_length: u32,
    /// Attention query head count.
    pub head_count: u32,
    /// Attention KV head count (== head_count for vanilla MHA).
    pub head_count_kv: u32,
    /// Per-head dimension (`embedding_length / head_count`).
    pub head_dim: u32,
    /// SwiGLU intermediate width.
    pub feed_forward_length: u32,
    /// Trained context length.
    pub context_length: u32,
    /// Tokenizer vocab size.
    pub vocab_size: u32,
    /// Number of tensor descriptors in the source GGUF.
    pub tensor_count: u32,
    /// On-disk size of the source GGUF in bytes (for telemetry).
    pub source_bytes: u32,
    /// RoPE base frequency.
    pub rope_freq_base: f32,
}

// SAFETY: SlmModelInfoC is `#[repr(C)]` with only POD fields; the
// fixed-size byte arrays are standalone bytes (not pointers). It
// crosses the FFI boundary as a value type and never contains
// borrowed data.

// ---------------------------------------------------------------------------
// Slot table + spinlock
// ---------------------------------------------------------------------------

static SLM_LOCK: AtomicBool = AtomicBool::new(false);
static mut SLOTS: [Option<LoadedSlm>; SLM_MAX_SLOTS] = [EMPTY_SLOT; SLM_MAX_SLOTS];

struct SpinGuard;

impl SpinGuard {
    fn new() -> Self {
        while SLM_LOCK
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        SpinGuard
    }
}

impl Drop for SpinGuard {
    fn drop(&mut self) {
        SLM_LOCK.store(false, Ordering::Release);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Attempt to load a GGUF buffer into the SLM registry. On success
/// returns the slot index; on error a typed [`LoadError`].
///
/// M5.3.1 owns the source bytes in the weight pool, builds the
/// [`Bbpe`] tokenizer, and snapshots tensor descriptors so the
/// caller can drop `data` immediately after this call returns.
///
/// `name` is a UTF-8 byte slice (not null-terminated) used for
/// display in `slm list` / `slm info`. It's clamped to
/// [`SLM_NAME_LEN`] - 1 bytes; truncation is silent and recorded as
/// a UART warning.
pub fn load_slm(name: &[u8], data: &[u8]) -> Result<usize, LoadError> {
    if data.len() > MAX_PLAUSIBLE_GGUF_BYTES {
        // Reject implausibly large GGUFs early. Matches the M7 shell
        // cap; also stops a u32 source_bytes overflow path with one
        // clear error code instead of two paths.
        return Err(LoadError::ModelTooLarge);
    }

    let gguf = Gguf::parse(data).map_err(map_gguf_err)?;
    let info = gguf.validate_for_inference().map_err(map_gguf_err)?;
    let vocab_size = vocab_size_of(&gguf)?;
    let tensor_count = u32::try_from(gguf.tensor_count())
        .map_err(|_| LoadError::CorruptedData)?;
    // `data.len()` is already bounded above by `MAX_PLAUSIBLE_GGUF_BYTES`
    // (2 GiB), so the u32 cast is provably loss-free. Earlier revisions
    // had a saturating fallback for > 4 GiB inputs; the M5.3.1 review
    // pointed out it was dead code now that the upstream cap is 2 GiB.
    let source_bytes = data.len() as u32;

    // Build the tokenizer up-front. If this fails the GGUF lacked a
    // tokens/merges array — surface as CorruptedData rather than a
    // separate variant, mirroring the existing GgufError mapping.
    let tokenizer = Bbpe::from_gguf(&gguf).map_err(|_| LoadError::CorruptedData)?;

    // Snapshot tensor descriptors with owned `String` names so the
    // GGUF-borrowed `'a` lifetime can be dropped after this function.
    let tensor_data_start = gguf.tensor_data_start();
    let mut tensors: Vec<OwnedTensorInfo> = Vec::with_capacity(gguf.tensors().len());
    for t in gguf.tensors() {
        tensors.push(OwnedTensorInfo {
            name: String::from(t.name),
            dims: t.dims.clone(),
            ggml_type: t.ggml_type.0,
            offset: t.offset,
        });
    }

    // Allocate a weight-pool block for the source bytes. As of
    // Phase 3 the model-mem pool returns a fixed 2 MB single block
    // regardless of the requested size — multi-block allocation is
    // tracked as a follow-up for real Qwen-sized GGUFs (~1 GB). The
    // explicit bounds check below catches the discrepancy at load
    // time so a real GGUF fails fast with a clear error instead of
    // silently overrunning the block during `copy_nonoverlapping`.
    let block = mm::alloc_weights(data.len()).map_err(map_alloc_err)?;
    let block_size = match mm::get_size(block) {
        Some(s) => s,
        None => {
            let _ = mm::free(block);
            return Err(LoadError::AllocFailed);
        }
    };
    if data.len() > block_size {
        // Reject before any unsafe copy — see runtime/CLAUDE.md
        // "Checked arithmetic at boundaries". `ModelTooLarge`
        // surfaces back through the FFI as -1 with a UART log.
        unsafe {
            kernel_ffi::uart_puts(
                b"[slm] GGUF too large for current single-block weight pool;\n  multi-block allocator landing in M5.3.3 follow-up.\n\0"
                    .as_ptr(),
            );
        }
        let _ = mm::free(block);
        return Err(LoadError::ModelTooLarge);
    }
    let block_ptr = match mm::get_ptr(block) {
        Some(p) if !p.is_null() => p,
        _ => {
            let _ = mm::free(block);
            return Err(LoadError::AllocFailed);
        }
    };

    // Copy `data` into the owned block. Once this returns the caller
    // can drop the source buffer; everything the registry/decoder
    // touches lives inside the pool block from here on.
    //
    // SAFETY: `block_ptr` was just returned by `mm::alloc_weights`
    // and the bounds check above guarantees `block_size >= data.len()`,
    // so writing `data.len()` bytes stays inside the allocation.
    // Source and destination cannot overlap (the pool block is
    // distinct from the caller's input buffer). `data` is
    // initialized for `data.len()` bytes per the caller's contract.
    unsafe {
        core::ptr::copy_nonoverlapping(data.as_ptr(), block_ptr, data.len());
    }

    // Drop the temporary `Gguf<'a>` parser before stashing the entry
    // — its borrowed slices into `data` go out of scope here, so the
    // caller's source buffer is no longer aliased.
    drop(gguf);

    let entry = LoadedSlm {
        name: clamp_name(name),
        info,
        vocab_size,
        source_bytes,
        tensor_count,
        weight_block: Some(block),
        weight_data_len: data.len(),
        tokenizer: Some(tokenizer),
        tensors,
        tensor_data_start,
    };
    match insert_entry(entry) {
        Ok(idx) => Ok(idx),
        Err(e) => {
            // Insert failed (registry full); release the pool block
            // we just took so we don't leak the allocation.
            let _ = mm::free(block);
            Err(e)
        }
    }
}

/// Free the slot at `index`. Returns `LoadError::InvalidFormat` if
/// the slot is empty or the index is out of range.
pub fn unload_slm(index: usize) -> Result<(), LoadError> {
    if index >= SLM_MAX_SLOTS {
        return Err(LoadError::InvalidFormat);
    }
    // Take the slot out under the lock so we can free its weight
    // block without holding the registry lock during the
    // `mm::free` call (the pool has its own lock).
    let taken = {
        let _g = SpinGuard::new();
        // SAFETY: SpinGuard held — exclusive access to SLOTS.
        let slot = unsafe { &mut *core::ptr::addr_of_mut!(SLOTS) };
        slot[index].take()
    };
    let entry = match taken {
        Some(e) => e,
        None => return Err(LoadError::InvalidFormat),
    };
    if let Some(block) = entry.weight_block {
        let _ = mm::free(block);
    }
    Ok(())
}

/// Return a C-layout snapshot of the slot at `index`, or `None` if
/// the slot is empty.
pub fn get_info(index: usize) -> Option<SlmModelInfoC> {
    if index >= SLM_MAX_SLOTS {
        return None;
    }
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to SLOTS.
    let slot = unsafe { &*core::ptr::addr_of!(SLOTS) };
    let entry = slot[index].as_ref()?;
    Some(slm_info_to_c(entry))
}

/// Run a closure with read access to a loaded SLM's owned state.
/// Returns `None` if the slot is empty or `index` is out of range.
///
/// The registry lock is held for the duration of `f`, so the closure
/// must not re-enter the registry (would deadlock). The closure may
/// return any value; typical use is to look up the tokenizer or a
/// tensor by name and copy out the result.
pub fn with_loaded_slm<F, R>(index: usize, f: F) -> Option<R>
where
    F: FnOnce(&LoadedSlm) -> R,
{
    if index >= SLM_MAX_SLOTS {
        return None;
    }
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to SLOTS for the
    // duration of `f`.
    let slot = unsafe { &*core::ptr::addr_of!(SLOTS) };
    let entry = slot[index].as_ref()?;
    Some(f(entry))
}

/// Number of currently-occupied slots.
pub fn count() -> usize {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held.
    let slot = unsafe { &*core::ptr::addr_of!(SLOTS) };
    slot.iter().filter(|s| s.is_some()).count()
}

#[cfg(test)]
pub(crate) fn reset_for_tests() {
    // First, free any pool blocks held by occupied slots so we don't
    // leak weight-pool capacity across tests.
    {
        let _g = SpinGuard::new();
        // SAFETY: SpinGuard held; only used in cfg(test).
        let slot = unsafe { &mut *core::ptr::addr_of_mut!(SLOTS) };
        for s in slot.iter_mut() {
            if let Some(entry) = s.take() {
                if let Some(handle) = entry.weight_block {
                    // Release outside the slot, but we still hold the
                    // registry lock — `mm::free` takes its own lock,
                    // so the order doesn't deadlock.
                    let _ = mm::free(handle);
                }
            }
        }
    }
}

/// Lazy one-shot init of the host-side weight pool. Tests call this
/// before [`load_slm`] because the pool requires
/// [`crate::mm::model_mem_init`] before any `alloc_weights` call.
/// The host `slm_alloc_pages` stub in `lib.rs::test_ffi_stubs`
/// services the underlying page request via `std::alloc`.
#[cfg(test)]
pub(crate) fn ensure_mm_initialized_for_tests() {
    use core::sync::atomic::AtomicBool;
    static MM_INITED: AtomicBool = AtomicBool::new(false);
    if !MM_INITED.load(Ordering::Acquire) {
        // Idempotent on success; if a different test thread has
        // already raced ahead the second call sees an already-
        // initialized state and returns OK.
        let _ = crate::mm::model_mem_init(64, 8);
        MM_INITED.store(true, Ordering::Release);
    }
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

fn insert_entry(entry: LoadedSlm) -> Result<usize, LoadError> {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held.
    let slot = unsafe { &mut *core::ptr::addr_of_mut!(SLOTS) };
    for (i, s) in slot.iter_mut().enumerate() {
        if s.is_none() {
            *s = Some(entry);
            return Ok(i);
        }
    }
    // TODO(M5): replace with `LoadError::RegistryFull`. Reusing
    // `ModelTooLarge` here keeps the FFI shape stable for M1.4 but
    // misreads as "the model is too large" rather than "the
    // registry is full" — once M5 widens `LoadError`, callers can
    // discriminate.
    Err(LoadError::ModelTooLarge)
}

fn clamp_name(name: &[u8]) -> String {
    let limit = SLM_NAME_LEN - 1;
    let bytes = if name.len() > limit { &name[..limit] } else { name };
    // Replace non-UTF-8 bytes with '?' rather than failing. The name
    // is for display only.
    match core::str::from_utf8(bytes) {
        Ok(s) => String::from(s),
        Err(_) => {
            unsafe {
                kernel_ffi::uart_puts(b"[slm] non-UTF-8 model name; replacing with '?'\n\0".as_ptr());
            }
            let mut out = String::with_capacity(bytes.len());
            for b in bytes {
                // `is_ascii_graphic()` on a `char` already implies
                // ASCII, so the previous `b.is_ascii() && …` was
                // redundant. Parenthesize the disjunction explicitly
                // so a future edit can't accidentally repartition it.
                if (*b as char).is_ascii_graphic() || *b == b' ' {
                    out.push(*b as char);
                } else {
                    out.push('?');
                }
            }
            out
        }
    }
}

fn vocab_size_of(g: &Gguf<'_>) -> Result<u32, LoadError> {
    let v = g
        .metadata("tokenizer.ggml.tokens")
        .ok_or(LoadError::CorruptedData)?;
    match v {
        MetaValue::Array(arr) => u32::try_from(arr.values.len()).map_err(|_| LoadError::CorruptedData),
        _ => Err(LoadError::CorruptedData),
    }
}

fn slm_info_to_c(entry: &LoadedSlm) -> SlmModelInfoC {
    let mut arch_buf = [0u8; SLM_ARCH_LEN];
    let arch = match entry.info.architecture {
        ArchKind::Qwen2 => b"qwen2".as_ref(),
        ArchKind::Llama => b"llama".as_ref(),
    };
    copy_into(&mut arch_buf, arch);
    let mut name_buf = [0u8; SLM_NAME_LEN];
    copy_into(&mut name_buf, entry.name.as_bytes());
    SlmModelInfoC {
        architecture: arch_buf,
        name: name_buf,
        block_count: entry.info.block_count,
        embedding_length: entry.info.embedding_length,
        head_count: entry.info.head_count,
        head_count_kv: entry.info.head_count_kv,
        head_dim: entry.info.head_dim,
        feed_forward_length: entry.info.feed_forward_length,
        context_length: entry.info.context_length,
        vocab_size: entry.vocab_size,
        tensor_count: entry.tensor_count,
        source_bytes: entry.source_bytes,
        rope_freq_base: entry.info.rope_freq_base,
    }
}

fn copy_into(dst: &mut [u8], src: &[u8]) {
    let n = if src.len() < dst.len() { src.len() } else { dst.len() };
    dst[..n].copy_from_slice(&src[..n]);
}

fn map_gguf_err(_e: GgufError) -> LoadError {
    LoadError::CorruptedData
}

fn map_alloc_err(_e: crate::mm::AllocError) -> LoadError {
    LoadError::AllocFailed
}

/// On-disk byte size of a GGML tensor with `n_elements` of type
/// `ggml_type`. Returns `None` for unsupported types so callers
/// (M5.3.2's forward pass) can branch on "fall back to CPU".
///
/// Currently understood:
/// - F32 (id 0): 4 × n_elements
/// - F16 (id 1): 2 × n_elements
/// - Q4_K (id 12): 144 × ceil(n_elements / 256) bytes
/// - Q8_K (id 15): 256 × ceil(n_elements / 256) bytes  (1 super-block
///   header byte + 256 quant bytes — the `ggml.h` `block_q8_K` struct
///   is laid out as `{f32 d; int8_t qs[256]; int16_t bsums[16]} = 292`
///   in newer GGML; SLM-OS doesn't dequant Q8_K yet, so we conservatively
///   refuse to size it here. The caller should use F32/F16/Q4_K paths.)
pub fn ggml_type_byte_size(ggml_type: u32, n_elements: u64) -> Option<u64> {
    let n = n_elements as usize;
    let bytes = match GgmlType(ggml_type) {
        GgmlType::F32 => n.checked_mul(4)?,
        GgmlType::F16 => n.checked_mul(2)?,
        GgmlType::Q4_K => q4_k_byte_size(n)?,
        // Other quant types are out of scope for M5.3.1's plumbing —
        // M5.3.2 picks them up if the forward pass needs them.
        _ => return None,
    };
    u64::try_from(bytes).ok()
}

/// Element count for a tensor with the given dim shape, returning
/// `None` on multiplication overflow.
fn elements_of(dims: &[u64]) -> Option<u64> {
    dims.iter().try_fold(1u64, |acc, &d| acc.checked_mul(d))
}

// ---------------------------------------------------------------------------
// Test-fixture builder (callable from kernel tests via FFI)
// ---------------------------------------------------------------------------

/// Build a synthetic Qwen2.5-shaped GGUF with the given vocab size
/// and write the bytes into `out`. Returns the number of bytes
/// written, or `None` if the buffer was too small.
///
/// Public so the kernel C tests can call it via an FFI shim
/// (`rust_slm_test_build_qwen_fixture`). The shape exactly matches
/// Qwen2.5-1.5B-Instruct so the same fixture exercises every key
/// `validate_for_inference` reads.
///
/// **M5.3.1 update.** The fixture now ships with two tiny tensors
/// — `output_norm.weight` (4-element F32, bytes `[1.0, 2.0, 3.0,
/// 4.0]`) and `token_embd.weight` (Q4_K, 32 × 4 = 1 super-block × 4
/// rows = 144 bytes, zero-filled). The exact shapes don't matter for
/// the metadata path; they just give `tensor_bytes` lookups
/// something concrete to check against.
pub fn build_qwen_test_fixture(vocab_size: usize, out: &mut [u8]) -> Option<usize> {
    use crate::slm::gguf::{
        DEFAULT_ALIGNMENT, GGUF_MAGIC, GGUF_VERSION, MetaArray, MetaType, Q4_K_BLOCK_SIZE,
    };

    // Build the wire format into a Vec, then memcpy into the
    // caller's buffer if it fits.
    //
    // To keep `Bbpe::encode("hi")` produce non-empty output the
    // first 128 entries (when the vocab is large enough) are the
    // direct ASCII byte fallback — id `i` maps to the single-byte
    // token whose UTF-8 representation is the byte `i`. This
    // mirrors how production GGUFs lay out byte-fallback tokens at
    // the start of the vocab. For tiny vocab counts (< 128 entries)
    // we skip past the byte fallback and fall back to the original
    // `t0/t1/...` form so the `from_gguf` path still has names to
    // populate.
    let mut tokens = Vec::with_capacity(vocab_size);
    let byte_fallback = vocab_size >= 128;
    for i in 0..vocab_size {
        if byte_fallback && i < 128 {
            // ASCII byte (0..127) — emit as a single-byte string.
            let s: String = core::iter::once((i as u8) as char).collect();
            tokens.push(MetaValue::String(s));
        } else {
            let mut s = String::with_capacity(8);
            s.push('t');
            s.push_str(itoa_simple(i).as_str());
            tokens.push(MetaValue::String(s));
        }
    }

    let kvs: Vec<(&'static str, MetaValue)> = alloc::vec![
        ("general.alignment", MetaValue::Uint32(DEFAULT_ALIGNMENT as u32)),
        ("general.architecture", MetaValue::String(String::from("qwen2"))),
        ("qwen2.block_count", MetaValue::Uint32(28)),
        ("qwen2.embedding_length", MetaValue::Uint32(1536)),
        ("qwen2.attention.head_count", MetaValue::Uint32(12)),
        ("qwen2.attention.head_count_kv", MetaValue::Uint32(2)),
        ("qwen2.feed_forward_length", MetaValue::Uint32(8960)),
        ("qwen2.context_length", MetaValue::Uint32(32_768)),
        ("qwen2.rope.freq_base", MetaValue::Float32(1_000_000.0)),
        (
            "tokenizer.ggml.tokens",
            MetaValue::Array(MetaArray {
                elem_type: MetaType::String,
                values: tokens,
            }),
        ),
        // M5.3.1: `Bbpe::from_gguf` requires a merges array. Empty
        // merges are valid (encode falls back to per-byte tokens).
        (
            "tokenizer.ggml.merges",
            MetaValue::Array(MetaArray {
                elem_type: MetaType::String,
                values: Vec::new(),
            }),
        ),
    ];

    // Tensor payloads. Each `(name, ggml_type, dims, data)` entry is
    // emitted in `tensors[]` order; the offsets in the descriptors
    // are computed below. F32 4-element norm vector + Q4_K
    // (32 cols × 4 rows = 1 super-block per row × 4 rows = 144 ×
    // 1 = 144 bytes, since 32 < 256 GGML still rounds up to one
    // 256-element block per row → 4 × 144 = 576 bytes). The exact
    // numerics aren't used by M5.3.1 — only the byte-pattern
    // round-trip matters.
    let f32_data: [u8; 16] = {
        let mut buf = [0u8; 16];
        buf[0..4].copy_from_slice(&1.0f32.to_le_bytes());
        buf[4..8].copy_from_slice(&2.0f32.to_le_bytes());
        buf[8..12].copy_from_slice(&3.0f32.to_le_bytes());
        buf[12..16].copy_from_slice(&4.0f32.to_le_bytes());
        buf
    };
    // Spec calls for `[32, 4]` Q4_K — 32 × 4 = 128 elements total.
    // GGML's q4_k block holds 256 elements; 128 < 256 rounds up to
    // one super-block, so the tensor occupies a single 144-byte
    // block on disk (zero-filled).
    let q4k_data = alloc::vec![0u8; Q4_K_BLOCK_SIZE];

    // Order matches the descriptor list below. Keep tensor data
    // packed back-to-back so offsets are simply running sums.
    let tensor_specs: alloc::vec::Vec<(&'static str, u32, alloc::vec::Vec<u64>, &[u8])> = alloc::vec![
        ("output_norm.weight", GgmlType::F32.0, alloc::vec![4u64], &f32_data[..]),
        ("token_embd.weight", GgmlType::Q4_K.0, alloc::vec![32u64, 4u64], &q4k_data[..]),
    ];

    let mut buf: Vec<u8> = Vec::with_capacity(2048 + vocab_size * 12);
    buf.extend_from_slice(&GGUF_MAGIC.to_le_bytes());
    buf.extend_from_slice(&GGUF_VERSION.to_le_bytes());
    buf.extend_from_slice(&(tensor_specs.len() as u64).to_le_bytes());
    buf.extend_from_slice(&(kvs.len() as u64).to_le_bytes());
    for (k, v) in &kvs {
        write_string(&mut buf, k);
        write_meta_value(&mut buf, v);
    }
    // -- Tensor descriptors -----------------------------------------
    // Compute offsets as running sums so the on-disk layout matches
    // what `Gguf::parse` reconstructs from `(this offset .. next
    // offset)` deltas.
    let mut running_offset: u64 = 0;
    for (name, ggml_type, dims, data) in &tensor_specs {
        write_string(&mut buf, name);
        buf.extend_from_slice(&(dims.len() as u32).to_le_bytes());
        for d in dims {
            buf.extend_from_slice(&d.to_le_bytes());
        }
        buf.extend_from_slice(&ggml_type.to_le_bytes());
        buf.extend_from_slice(&running_offset.to_le_bytes());
        running_offset = running_offset.checked_add(data.len() as u64)?;
    }
    let pad = (DEFAULT_ALIGNMENT - (buf.len() as u64 % DEFAULT_ALIGNMENT))
        % DEFAULT_ALIGNMENT;
    buf.extend(core::iter::repeat_n(0u8, pad as usize));
    // -- Tensor data section ---------------------------------------
    for (_, _, _, data) in &tensor_specs {
        buf.extend_from_slice(data);
    }

    if out.len() < buf.len() {
        return None;
    }
    out[..buf.len()].copy_from_slice(&buf);
    Some(buf.len())
}

fn write_string(out: &mut Vec<u8>, s: &str) {
    out.extend_from_slice(&(s.len() as u64).to_le_bytes());
    out.extend_from_slice(s.as_bytes());
}

fn write_meta_value(out: &mut Vec<u8>, v: &MetaValue) {
    use crate::slm::gguf::MetaArray;
    out.extend_from_slice(&v.meta_type().as_u32().to_le_bytes());
    match v {
        MetaValue::Uint32(x) => out.extend_from_slice(&x.to_le_bytes()),
        MetaValue::Float32(x) => out.extend_from_slice(&x.to_le_bytes()),
        MetaValue::String(s) => write_string(out, s),
        MetaValue::Array(MetaArray { elem_type, values }) => {
            out.extend_from_slice(&elem_type.as_u32().to_le_bytes());
            out.extend_from_slice(&(values.len() as u64).to_le_bytes());
            for elem in values {
                if let MetaValue::String(s) = elem {
                    write_string(out, s);
                } else {
                    // Fixture builder only emits string arrays.
                    // Anything else is a programming error in the
                    // builder itself, not a runtime input.
                    return;
                }
            }
        }
        // Other variants aren't needed by the Qwen fixture; bail
        // silently rather than write garbage.
        _ => {}
    }
}

/// Tiny integer-to-string for short token names. Avoids dragging in
/// `format!` here just for token labels.
fn itoa_simple(n: usize) -> String {
    if n == 0 {
        return String::from("0");
    }
    let mut digits: [u8; 20] = [0; 20];
    let mut idx = digits.len();
    let mut x = n;
    while x > 0 {
        idx -= 1;
        digits[idx] = b'0' + (x % 10) as u8;
        x /= 10;
    }
    let mut s = String::with_capacity(digits.len() - idx);
    for &d in &digits[idx..] {
        s.push(d as char);
    }
    s
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    /// `cargo test` runs tests in parallel by default. All five
    /// tests in this module mutate the global `SLOTS` table; without
    /// serialization they race on `reset_for_tests()`. A separate
    /// `TEST_SERIAL` flag (independent of `SLM_LOCK`, which is
    /// reentered by every registry call) holds for the duration of
    /// each test body. The guard's `Drop` releases it on every exit
    /// path including assertion panics (panics still abort under
    /// `panic = "abort"` per Cargo.toml, but `Drop` runs first).
    static TEST_SERIAL: AtomicBool = AtomicBool::new(false);

    struct TestSerialGuard;

    impl TestSerialGuard {
        fn new() -> Self {
            while TEST_SERIAL
                .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
                .is_err()
            {
                core::hint::spin_loop();
            }
            TestSerialGuard
        }
    }

    impl Drop for TestSerialGuard {
        fn drop(&mut self) {
            TEST_SERIAL.store(false, Ordering::Release);
        }
    }

    /// Build a Qwen-shaped fixture in a Vec for test convenience.
    /// Wraps the public `build_qwen_test_fixture` (which takes a
    /// caller-supplied buffer) so tests don't have to size it
    /// themselves.
    pub(crate) fn build_qwen_gguf_with_vocab(vocab_size: usize) -> Vec<u8> {
        // Headroom for metadata, tensor descriptors, alignment pad,
        // and tensor data (≈ 600 B). 4 KB + per-token slack covers
        // any reasonable vocab.
        let mut buf = vec![0u8; 4096 + vocab_size * 16];
        let n = build_qwen_test_fixture(vocab_size, &mut buf).expect("fixture fits");
        buf.truncate(n);
        buf
    }

    pub(crate) use super::ensure_mm_initialized_for_tests as ensure_mm_initialized;

    // -- Tests --

    #[test]
    fn load_qwen_records_arch_info_and_returns_handle() {
        let _serial = TestSerialGuard::new();
        ensure_mm_initialized();
        reset_for_tests();
        // M5.3.1: trimmed vocab from the production 152 064 down to
        // a value whose serialized GGUF fits in the single-block
        // (2 MB) weight pool. The full Qwen vocab needs the
        // multi-block allocator (M5.3.3 follow-up). The shape
        // assertions below pin the architecture metadata, which is
        // what the test was originally exercising — vocab size is
        // an architecture-independent dial.
        let bytes = build_qwen_gguf_with_vocab(2048);
        let idx = load_slm(b"qwen2.5-1.5b", &bytes).expect("load");
        assert_eq!(idx, 0);

        let info = get_info(idx).expect("info");
        assert_eq!(&info.architecture[..5], b"qwen2");
        assert_eq!(&info.name[..12], b"qwen2.5-1.5b");
        assert_eq!(info.block_count, 28);
        assert_eq!(info.embedding_length, 1536);
        assert_eq!(info.head_count, 12);
        assert_eq!(info.head_count_kv, 2);
        assert_eq!(info.head_dim, 128);
        assert_eq!(info.feed_forward_length, 8960);
        assert_eq!(info.context_length, 32768);
        assert_eq!(info.vocab_size, 2048);
        assert_eq!(info.rope_freq_base, 1_000_000.0);
        // M5.3.1: fixture now ships output_norm.weight + token_embd.weight.
        assert_eq!(info.tensor_count, 2);
        assert_eq!(info.source_bytes, bytes.len() as u32);
    }

    #[test]
    fn unload_frees_slot_for_reuse() {
        let _serial = TestSerialGuard::new();
        ensure_mm_initialized();
        reset_for_tests();
        let bytes = build_qwen_gguf_with_vocab(64);
        let idx = load_slm(b"a", &bytes).expect("load a");
        assert_eq!(idx, 0);
        unload_slm(idx).expect("unload");
        assert!(get_info(idx).is_none());
        let idx2 = load_slm(b"b", &bytes).expect("load b");
        assert_eq!(idx2, 0, "slot 0 should be reused after unload");
    }

    #[test]
    fn registry_overflow_returns_error() {
        let _serial = TestSerialGuard::new();
        ensure_mm_initialized();
        reset_for_tests();
        let bytes = build_qwen_gguf_with_vocab(8);
        for i in 0..SLM_MAX_SLOTS {
            let idx = load_slm(format!("m{i}").as_bytes(), &bytes).expect("load");
            assert_eq!(idx, i);
        }
        match load_slm(b"overflow", &bytes) {
            Err(LoadError::ModelTooLarge) => {}
            other => panic!("expected ModelTooLarge, got {other:?}"),
        }
    }

    #[test]
    fn load_rejects_non_gguf_bytes() {
        let _serial = TestSerialGuard::new();
        ensure_mm_initialized();
        reset_for_tests();
        let mut bad = Vec::from(*b"NOTAFILE");
        bad.resize(64, 0);
        match load_slm(b"bad", &bad) {
            Err(LoadError::CorruptedData) => {}
            other => panic!("expected CorruptedData, got {other:?}"),
        }
    }

    #[test]
    fn long_name_is_clamped_not_overflowed() {
        let _serial = TestSerialGuard::new();
        ensure_mm_initialized();
        reset_for_tests();
        let bytes = build_qwen_gguf_with_vocab(8);
        let long = vec![b'x'; SLM_NAME_LEN + 32];
        let idx = load_slm(&long, &bytes).expect("load long");
        let info = get_info(idx).expect("info");
        // First (SLM_NAME_LEN - 1) bytes are 'x', last byte is '\0'.
        for &b in &info.name[..SLM_NAME_LEN - 1] {
            assert_eq!(b, b'x');
        }
        assert_eq!(info.name[SLM_NAME_LEN - 1], 0);
    }

    // -- M5.3.1 tests ---------------------------------------------

    #[test]
    fn load_slm_owns_weight_block() {
        let _serial = TestSerialGuard::new();
        ensure_mm_initialized();
        reset_for_tests();
        let bytes = build_qwen_gguf_with_vocab(64);
        let idx = load_slm(b"qwen-test", &bytes).expect("load");
        let backed = with_loaded_slm(idx, |slm| slm.weight_block.is_some())
            .expect("with_loaded_slm");
        assert!(backed, "expected weight_block to be allocated");
        // The owned buffer round-trips the GGUF prologue (magic+version).
        let header_ok = with_loaded_slm(idx, |slm| {
            let buf = slm.weight_bytes().expect("weight_bytes");
            buf.len() >= 8 && &buf[..4] == b"GGUF"
        })
        .unwrap_or(false);
        assert!(header_ok);
        unload_slm(idx).expect("unload");
    }

    #[test]
    fn tensor_bytes_returns_correct_slice() {
        let _serial = TestSerialGuard::new();
        ensure_mm_initialized();
        reset_for_tests();
        let bytes = build_qwen_gguf_with_vocab(8);
        let idx = load_slm(b"qwen-tb", &bytes).expect("load");

        // The fixture's output_norm.weight is F32 [1.0, 2.0, 3.0, 4.0].
        let observed = with_loaded_slm(idx, |slm| {
            slm.tensor_bytes("output_norm.weight").map(|b| b.to_vec())
        })
        .flatten()
        .expect("output_norm.weight bytes");
        assert_eq!(observed.len(), 16);
        assert_eq!(&observed[0..4], &1.0f32.to_le_bytes());
        assert_eq!(&observed[4..8], &2.0f32.to_le_bytes());
        assert_eq!(&observed[8..12], &3.0f32.to_le_bytes());
        assert_eq!(&observed[12..16], &4.0f32.to_le_bytes());

        // Q4_K tensor — 32×4 = 128 elements, padded to a single
        // 256-element super-block on disk (144 bytes).
        let q4k_len = with_loaded_slm(idx, |slm| {
            slm.tensor_bytes("token_embd.weight").map(|b| b.len())
        })
        .flatten();
        assert_eq!(q4k_len, Some(crate::slm::gguf::Q4_K_BLOCK_SIZE));
        unload_slm(idx).expect("unload");
    }

    #[test]
    fn tokenizer_built_from_loaded_gguf() {
        let _serial = TestSerialGuard::new();
        ensure_mm_initialized();
        reset_for_tests();
        let bytes = build_qwen_gguf_with_vocab(64);
        let idx = load_slm(b"qwen-tok", &bytes).expect("load");
        let (have_tk, vocab) = with_loaded_slm(idx, |slm| {
            let tk = slm.tokenizer();
            (tk.is_some(), tk.map(|t| t.vocab_size()).unwrap_or(0))
        })
        .expect("with_loaded_slm");
        assert!(have_tk);
        assert!(vocab > 0);
        unload_slm(idx).expect("unload");
    }

    #[test]
    fn unload_frees_weight_block_repeatedly() {
        let _serial = TestSerialGuard::new();
        ensure_mm_initialized();
        reset_for_tests();
        let bytes = build_qwen_gguf_with_vocab(16);
        // Two cycles: a weight-pool leak would fail the second
        // load with AllocFailed once the pool is exhausted (the
        // host stub allocates real memory on every alloc; the pool
        // tracks it as a fixed slot count).
        for _ in 0..2 {
            let idx = load_slm(b"x", &bytes).expect("load");
            unload_slm(idx).expect("unload");
        }
        assert_eq!(count(), 0);
    }

    #[test]
    fn unload_already_empty_slot_is_invalid_format() {
        let _serial = TestSerialGuard::new();
        ensure_mm_initialized();
        reset_for_tests();
        let bytes = build_qwen_gguf_with_vocab(16);
        let idx = load_slm(b"x", &bytes).expect("load");
        // First unload succeeds.
        assert!(matches!(unload_slm(idx), Ok(())));
        // Second unload on the now-empty slot must NOT double-free
        // the weight block; the slot table's `take()` returns None
        // and the registry surfaces InvalidFormat.
        assert!(matches!(
            unload_slm(idx),
            Err(LoadError::InvalidFormat)
        ));
        // And an out-of-range index is rejected the same way.
        assert!(matches!(
            unload_slm(SLM_MAX_SLOTS),
            Err(LoadError::InvalidFormat)
        ));
    }

    #[test]
    fn load_rejects_oversized_buffer() {
        let _serial = TestSerialGuard::new();
        ensure_mm_initialized();
        reset_for_tests();
        // Pretend the source buffer is bigger than the cap. Building
        // a real 2 GB Vec in tests is wasteful — we cheat by passing
        // a slice whose `len()` exceeds the limit even though the
        // backing storage is tiny. `from_raw_parts` builds such a
        // slice from a placeholder pointer; the early-return in
        // `load_slm` rejects the request before any read.
        let cap = MAX_PLAUSIBLE_GGUF_BYTES + 1;
        // SAFETY: we never deref the slice — `load_slm`'s cap check
        // returns before any pointer access. The borrow lasts only
        // for the duration of `load_slm`.
        let big_slice: &[u8] = unsafe {
            core::slice::from_raw_parts(core::ptr::NonNull::<u8>::dangling().as_ptr(), cap)
        };
        match load_slm(b"oversized", big_slice) {
            Err(LoadError::ModelTooLarge) => {}
            other => panic!("expected ModelTooLarge, got {other:?}"),
        }
    }
}
