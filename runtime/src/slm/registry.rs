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
//! For M1.4 the registry stores **only metadata** — enough for the
//! `slm info` shell command to report architecture, layer count,
//! hidden size, vocab size, etc. M5 will extend `LoadedSlm` to own
//! the weight-pool block that holds the GGUF bytes; until then a
//! load is "metadata-only" and the caller is expected to keep the
//! source bytes alive externally if it wants to reparse later.

use core::sync::atomic::{AtomicBool, Ordering};

use alloc::string::String;
use alloc::vec::Vec;

use crate::kernel_ffi;
use crate::mm::model_loader::LoadError;
use crate::slm::gguf::{ArchInfo, ArchKind, Gguf, GgufError, MetaValue};

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

/// Per-slot record in the SLM registry.
#[derive(Debug, Clone)]
struct LoadedSlm {
    name: String,
    info: ArchInfo,
    vocab_size: u32,
    /// On-disk size of the GGUF bytes used to load this model (for
    /// telemetry; not the resident memory size).
    source_bytes: u32,
    /// Total number of tensor descriptors in the GGUF — proxy for
    /// "complexity" the shell can surface to the user.
    tensor_count: u32,
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
/// `name` is a UTF-8 byte slice (not null-terminated) used for
/// display in `slm list` / `slm info`. It's clamped to
/// [`SLM_NAME_LEN`] - 1 bytes; truncation is silent and recorded as
/// a UART warning.
pub fn load_slm(name: &[u8], data: &[u8]) -> Result<usize, LoadError> {
    let gguf = Gguf::parse(data).map_err(map_gguf_err)?;
    let info = gguf.validate_for_inference().map_err(map_gguf_err)?;
    let vocab_size = vocab_size_of(&gguf)?;
    let tensor_count = u32::try_from(gguf.tensor_count())
        .map_err(|_| LoadError::CorruptedData)?;
    let source_bytes = u32::try_from(data.len()).unwrap_or(u32::MAX);
    let entry = LoadedSlm {
        name: clamp_name(name),
        info,
        vocab_size,
        source_bytes,
        tensor_count,
    };
    insert_entry(entry)
}

/// Free the slot at `index`. Returns `LoadError::InvalidFormat` if
/// the slot is empty or the index is out of range.
pub fn unload_slm(index: usize) -> Result<(), LoadError> {
    if index >= SLM_MAX_SLOTS {
        return Err(LoadError::InvalidFormat);
    }
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to SLOTS.
    let slot = unsafe { &mut *core::ptr::addr_of_mut!(SLOTS) };
    if slot[index].is_none() {
        return Err(LoadError::InvalidFormat);
    }
    slot[index] = None;
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

/// Number of currently-occupied slots.
pub fn count() -> usize {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held.
    let slot = unsafe { &*core::ptr::addr_of!(SLOTS) };
    slot.iter().filter(|s| s.is_some()).count()
}

#[cfg(test)]
pub(crate) fn reset_for_tests() {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held; only used in cfg(test).
    let slot = unsafe { &mut *core::ptr::addr_of_mut!(SLOTS) };
    for s in slot.iter_mut() {
        *s = None;
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
                if b.is_ascii() && (*b as char).is_ascii_graphic() || *b == b' ' {
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

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::slm::gguf::{
        DEFAULT_ALIGNMENT, GGUF_MAGIC, GGUF_VERSION, GgmlType, MetaArray, MetaType,
    };
    use alloc::string::ToString;
    use alloc::vec;

    /// Test-only helper duplicating a small piece of `TestBuilder` to
    /// keep the registry tests self-contained — the parser tests
    /// already exercise the full builder; here we just need a valid
    /// Qwen-shaped GGUF with a tokens array.
    fn build_qwen_gguf_with_vocab(vocab_size: usize) -> Vec<u8> {
        // Re-use the parser's writer through the public type system:
        // construct a builder via the parser-side API surface. The
        // test module of gguf.rs has the writer; we'll mirror its
        // wire-format calls inline.
        let mut tokens = Vec::with_capacity(vocab_size);
        for i in 0..vocab_size {
            tokens.push(MetaValue::String(format!("tok{i}")));
        }
        let kvs: Vec<(String, MetaValue)> = vec![
            ("general.alignment".to_string(), MetaValue::Uint32(DEFAULT_ALIGNMENT as u32)),
            ("general.architecture".to_string(), MetaValue::String("qwen2".into())),
            ("qwen2.block_count".to_string(), MetaValue::Uint32(28)),
            ("qwen2.embedding_length".to_string(), MetaValue::Uint32(1536)),
            ("qwen2.attention.head_count".to_string(), MetaValue::Uint32(12)),
            ("qwen2.attention.head_count_kv".to_string(), MetaValue::Uint32(2)),
            ("qwen2.feed_forward_length".to_string(), MetaValue::Uint32(8960)),
            ("qwen2.context_length".to_string(), MetaValue::Uint32(32768)),
            ("qwen2.rope.freq_base".to_string(), MetaValue::Float32(1_000_000.0)),
            (
                "tokenizer.ggml.tokens".to_string(),
                MetaValue::Array(MetaArray {
                    elem_type: MetaType::String,
                    values: tokens,
                }),
            ),
        ];

        // Manually write the GGUF wire format. This duplicates the
        // parser-side test writer minimally (no tensors needed).
        let mut out: Vec<u8> = Vec::new();
        out.extend_from_slice(&GGUF_MAGIC.to_le_bytes());
        out.extend_from_slice(&GGUF_VERSION.to_le_bytes());
        out.extend_from_slice(&0u64.to_le_bytes()); // tensor_count = 0
        out.extend_from_slice(&(kvs.len() as u64).to_le_bytes());

        for (k, v) in &kvs {
            write_string(&mut out, k);
            write_meta_value(&mut out, v);
        }
        // Pad to alignment, no tensor data.
        let pad = (DEFAULT_ALIGNMENT - (out.len() as u64 % DEFAULT_ALIGNMENT))
            % DEFAULT_ALIGNMENT;
        out.extend(core::iter::repeat_n(0u8, pad as usize));
        out
    }

    fn write_string(out: &mut Vec<u8>, s: &str) {
        out.extend_from_slice(&(s.len() as u64).to_le_bytes());
        out.extend_from_slice(s.as_bytes());
    }

    fn write_meta_value(out: &mut Vec<u8>, v: &MetaValue) {
        out.extend_from_slice(&v.meta_type().as_u32().to_le_bytes());
        match v {
            MetaValue::Uint32(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::Float32(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::String(s) => write_string(out, s),
            MetaValue::Array(arr) => {
                out.extend_from_slice(&arr.elem_type.as_u32().to_le_bytes());
                out.extend_from_slice(&(arr.values.len() as u64).to_le_bytes());
                for elem in &arr.values {
                    // Element-type tag is implicit in the array; only
                    // payload bytes go on the wire (no per-element
                    // type prefix). For tokens we have all strings.
                    if let MetaValue::String(s) = elem {
                        write_string(out, s);
                    } else {
                        panic!("test: only String tokens supported");
                    }
                }
            }
            _ => panic!("test: unsupported MetaValue write"),
        }
    }

    // -- Tests --

    #[test]
    fn load_qwen_records_arch_info_and_returns_handle() {
        reset_for_tests();
        let bytes = build_qwen_gguf_with_vocab(152_064);
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
        assert_eq!(info.vocab_size, 152_064);
        assert_eq!(info.rope_freq_base, 1_000_000.0);
        assert_eq!(info.tensor_count, 0);
        assert_eq!(info.source_bytes, bytes.len() as u32);
    }

    #[test]
    fn unload_frees_slot_for_reuse() {
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
}
