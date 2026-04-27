//! `no_std` zero-copy GGUF v3 parser.
//!
//! Runtime-side counterpart to `host-tools/gguf-inspect/src/gguf.rs`
//! (M0.1). Same GGUF v3 wire format, same DoS gates, but built for the
//! kernel: `#![no_std]`, lifetimes bound to the input buffer so tensor
//! names borrow directly from the mmap'd file, and `Vec`/`String` only
//! where ownership is genuinely needed (metadata KVs survive past the
//! parse call; tensor names do not).
//!
//! The full GGUF v3 wire format is documented at
//! <https://github.com/ggerganov/ggml/blob/master/docs/gguf.md>. Layout:
//!
//! ```text
//!  ┌─────────────────────────────────────────────┐
//!  │ Header (24 B)                                │
//!  │   magic = "GGUF"           u32               │
//!  │   version                  u32  (must be 3)  │
//!  │   tensor_count             u64               │
//!  │   metadata_kv_count        u64               │
//!  ├─────────────────────────────────────────────┤
//!  │ Metadata KV pairs (metadata_kv_count of)    │
//!  │   key:   length-prefixed UTF-8 (u64 + bytes) │
//!  │   value: typed (4 byte type tag + payload)   │
//!  ├─────────────────────────────────────────────┤
//!  │ Tensor info (tensor_count of)               │
//!  │   name:    length-prefixed UTF-8             │
//!  │   n_dims:  u32                               │
//!  │   dims:    [u64; n_dims]                     │
//!  │   ggml_t:  u32                               │
//!  │   offset:  u64 (relative to tensor_data)     │
//!  ├─────────────────────────────────────────────┤
//!  │ Padding to general.alignment (default 32)   │
//!  ├─────────────────────────────────────────────┤
//!  │ Tensor data (raw bytes, types per ggml_t)   │
//!  └─────────────────────────────────────────────┘
//! ```
//!
//! M1.2 also lands the Q4_K block-layout accessors used by the M3
//! dequantization kernels.

#![allow(clippy::module_name_repetitions)]

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt;

// ---------------------------------------------------------------------------
// Wire constants
// ---------------------------------------------------------------------------

/// GGUF magic value `"GGUF"` in little-endian byte order.
pub const GGUF_MAGIC: u32 = 0x4655_4747;

/// Format version this parser understands.
pub const GGUF_VERSION: u32 = 3;

/// Default value of `general.alignment` per the GGUF spec.
pub const DEFAULT_ALIGNMENT: u64 = 32;

// ---------------------------------------------------------------------------
// Plausibility gates (mirrors host-tools/gguf-inspect M0.1 review fixes)
// ---------------------------------------------------------------------------

/// Sanity ceiling for `tensor_count` used when sizing a `Vec`. Real
/// GGUFs ship well under 1024 tensors; capping `with_capacity` at
/// `2^20` keeps a malicious header (e.g. `u64::MAX`) from crashing the
/// kernel via OOM. The actual loop still runs `tensor_count`
/// iterations, but each one reserves only what it needs and EOFs
/// cleanly on truncated input.
const MAX_PLAUSIBLE_TENSORS: u64 = 1 << 20;

/// Same idea for metadata KV pairs.
const MAX_PLAUSIBLE_KVS: u64 = 1 << 20;

/// Minimum on-disk size of a single tensor descriptor: name length
/// prefix (8) + zero-byte name + n_dims (4) + ggml_type (4) + offset
/// (8) = 24. Any real tensor will be larger.
const MIN_TENSOR_RECORD_SIZE: u64 = 24;

/// Minimum on-disk size of a single KV pair: 8 (key length) + 0 (zero
/// byte name) + 4 (type tag) + 1 (smallest payload, e.g. u8/bool).
const MIN_KV_RECORD_SIZE: u64 = 13;

/// Reject any single tensor dimension > `2^40` (~1 trillion elements)
/// — these only happen via corruption.
const MAX_PLAUSIBLE_DIM: u64 = 1 << 40;

/// Reject string fields longer than 16 MB. Real GGUF metadata strings
/// (architecture name, tokenizer model, chat template) sit well below
/// this; tokens are shorter still. Anything past this ceiling is
/// almost certainly a hostile or corrupt file claiming a runaway
/// length.
const MAX_PLAUSIBLE_STRING_LEN: u64 = 1 << 24;

/// Cap on initial `Vec::with_capacity` for metadata arrays. The loop
/// will still push `len` elements (bounded by file size via
/// [`MetaType::min_element_size`]), but the *initial* allocation stays
/// modest so a 1 GB file with a billion 1-byte array can't pre-reserve
/// a billion-entry Vec.
const MAX_PLAUSIBLE_ARRAY_LEN: u64 = 1 << 20;

/// Maximum number of dimensions per tensor. GGML's compile-time
/// `GGML_MAX_DIMS` is 4; we accept up to 8 for forward compatibility
/// but reject anything above that as corruption.
const MAX_PLAUSIBLE_DIMS: u32 = 8;

// ---------------------------------------------------------------------------
// Metadata-value types
// ---------------------------------------------------------------------------

/// GGUF metadata-value type tag (4 bytes on the wire).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum MetaType {
    Uint8 = 0,
    Int8 = 1,
    Uint16 = 2,
    Int16 = 3,
    Uint32 = 4,
    Int32 = 5,
    Float32 = 6,
    Bool = 7,
    String = 8,
    Array = 9,
    Uint64 = 10,
    Int64 = 11,
    Float64 = 12,
}

impl MetaType {
    fn from_u32(v: u32) -> Result<Self, GgufError> {
        Ok(match v {
            0 => Self::Uint8,
            1 => Self::Int8,
            2 => Self::Uint16,
            3 => Self::Int16,
            4 => Self::Uint32,
            5 => Self::Int32,
            6 => Self::Float32,
            7 => Self::Bool,
            8 => Self::String,
            9 => Self::Array,
            10 => Self::Uint64,
            11 => Self::Int64,
            12 => Self::Float64,
            _ => return Err(GgufError::UnknownMetaType),
        })
    }

    /// Wire-format byte for this type.
    pub fn as_u32(self) -> u32 {
        self as u32
    }

    /// Lower bound on the on-disk size of one element of this type, in
    /// bytes. Used to cap array lengths against the file size: an
    /// `Array<String>` with claimed length `N` needs at least
    /// `N * min_element_size(String) = N * 8` bytes (the u64 length
    /// prefix; the string body and any payload follow). For nested
    /// arrays we use 12 (4-byte elem-type tag + 8-byte length prefix).
    fn min_element_size(self) -> u64 {
        match self {
            MetaType::Uint8 | MetaType::Int8 | MetaType::Bool => 1,
            MetaType::Uint16 | MetaType::Int16 => 2,
            MetaType::Uint32 | MetaType::Int32 | MetaType::Float32 => 4,
            MetaType::Uint64 | MetaType::Int64 | MetaType::Float64 => 8,
            // String: 8-byte length prefix, body may be empty.
            MetaType::String => 8,
            // Nested array: 4 (elem_type) + 8 (length prefix).
            MetaType::Array => 12,
        }
    }
}

/// A typed GGUF metadata value. Strings and arrays own their data
/// (the `alloc` crate is available; `linked_list_allocator` provides
/// the global allocator). Fixed-size scalars stay inline.
#[derive(Debug, Clone, PartialEq)]
pub enum MetaValue {
    Uint8(u8),
    Int8(i8),
    Uint16(u16),
    Int16(i16),
    Uint32(u32),
    Int32(i32),
    Uint64(u64),
    Int64(i64),
    Float32(f32),
    Float64(f64),
    Bool(bool),
    String(String),
    Array(MetaArray),
}

impl MetaValue {
    /// Returns the wire type tag for this value.
    pub fn meta_type(&self) -> MetaType {
        match self {
            MetaValue::Uint8(_) => MetaType::Uint8,
            MetaValue::Int8(_) => MetaType::Int8,
            MetaValue::Uint16(_) => MetaType::Uint16,
            MetaValue::Int16(_) => MetaType::Int16,
            MetaValue::Uint32(_) => MetaType::Uint32,
            MetaValue::Int32(_) => MetaType::Int32,
            MetaValue::Uint64(_) => MetaType::Uint64,
            MetaValue::Int64(_) => MetaType::Int64,
            MetaValue::Float32(_) => MetaType::Float32,
            MetaValue::Float64(_) => MetaType::Float64,
            MetaValue::Bool(_) => MetaType::Bool,
            MetaValue::String(_) => MetaType::String,
            MetaValue::Array(_) => MetaType::Array,
        }
    }

    /// Convenience accessor for string values.
    pub fn as_str(&self) -> Option<&str> {
        match self {
            MetaValue::String(s) => Some(s.as_str()),
            _ => None,
        }
    }

    /// Convenience accessor for u32 values (also accepts u64 if it
    /// fits — some tools emit `general.alignment` as either width).
    pub fn as_u32(&self) -> Option<u32> {
        match self {
            MetaValue::Uint32(v) => Some(*v),
            MetaValue::Uint64(v) if *v <= u32::MAX as u64 => Some(*v as u32),
            _ => None,
        }
    }
}

/// A homogeneous array of [`MetaValue`]s. The element-type tag is held
/// separately so empty arrays still round-trip.
#[derive(Debug, Clone, PartialEq)]
pub struct MetaArray {
    pub elem_type: MetaType,
    pub values: Vec<MetaValue>,
}

// ---------------------------------------------------------------------------
// Tensor descriptors
// ---------------------------------------------------------------------------

/// GGML tensor element type. Held as a raw `u32` so unknown / future
/// quantization types don't crash the parser — the dispatch in M3+
/// will branch on the well-known constants below.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GgmlType(pub u32);

impl GgmlType {
    /// `f32` — IEEE-754 single precision.
    pub const F32: GgmlType = GgmlType(0);
    /// `f16` — IEEE-754 half precision.
    pub const F16: GgmlType = GgmlType(1);
    /// `q4_0` — legacy 4-bit quant.
    pub const Q4_0: GgmlType = GgmlType(2);
    /// `q4_K` — 4-bit K-quants. Common for Qwen2/Llama weight tensors.
    pub const Q4_K: GgmlType = GgmlType(12);
    /// `q6_K` — 6-bit K-quants. Used for output projection in many K-quant models.
    pub const Q6_K: GgmlType = GgmlType(14);
    /// `q8_K` — 8-bit K-quants. Used for high-precision activations.
    pub const Q8_K: GgmlType = GgmlType(15);

    /// Human-readable label for the well-known types. Returns `None`
    /// for types this build doesn't have a name for.
    pub fn name(self) -> Option<&'static str> {
        Some(match self.0 {
            0 => "f32",
            1 => "f16",
            2 => "q4_0",
            3 => "q4_1",
            6 => "q5_0",
            7 => "q5_1",
            8 => "q8_0",
            9 => "q8_1",
            10 => "q2_K",
            11 => "q3_K",
            12 => "q4_K",
            13 => "q5_K",
            14 => "q6_K",
            15 => "q8_K",
            16 => "iq2_xxs",
            17 => "iq2_xs",
            18 => "iq3_xxs",
            19 => "iq1_s",
            20 => "iq4_nl",
            21 => "iq3_s",
            22 => "iq2_s",
            23 => "iq4_xs",
            24 => "i8",
            25 => "i16",
            26 => "i32",
            27 => "i64",
            28 => "f64",
            29 => "iq1_m",
            30 => "bf16",
            _ => return None,
        })
    }
}

/// Description of one tensor in the file. Borrows its `name` directly
/// from the input buffer (`'a`) — tensor names are fixed in the GGUF
/// file and never need mutation; keeping them as `&str` saves an
/// allocation per tensor (~340 tensors for Qwen2.5-1.5B).
///
/// `dims` is owned because the count is variable per tensor and a
/// `Vec` is the path of least friction; for any realistic GGUF this
/// is at most 4 `u64`s per tensor.
#[derive(Debug, Clone, PartialEq)]
pub struct TensorInfo<'a> {
    /// Tensor name, borrowed from the GGUF buffer.
    pub name: &'a str,
    /// Per-dimension element counts. GGML supports up to 4 dims
    /// natively; this parser caps at [`MAX_PLAUSIBLE_DIMS`] = 8.
    pub dims: Vec<u64>,
    /// GGML element-type tag.
    pub ggml_type: GgmlType,
    /// Offset (in bytes) of this tensor's data, relative to the start
    /// of the aligned tensor-data section.
    pub offset: u64,
}

// ---------------------------------------------------------------------------
// Top-level parser
// ---------------------------------------------------------------------------

/// A parsed GGUF file. Holds borrowed slices of the underlying buffer
/// (tensor names, raw tensor-data bytes) plus owned metadata.
///
/// Tensor names borrow from `data`; tensor *data* is also borrowed via
/// [`Self::tensor_data`]. The lifetime `'a` is the lifetime of the
/// input buffer, so a `Gguf<'a>` cannot outlive the mmap that produced it.
#[derive(Debug, Clone)]
pub struct Gguf<'a> {
    /// Original input buffer, retained so [`Self::tensor_data`] can
    /// hand out borrowed slices into the tensor-data section.
    data: &'a [u8],
    /// Format version (always 3 if [`Self::parse`] returned `Ok`).
    version: u32,
    /// Owned KV metadata, keyed by metadata key. We store as a flat
    /// vector rather than `BTreeMap` because (a) `BTreeMap` from
    /// `alloc::collections` is heavier than necessary for ~30 keys,
    /// and (b) preserving insertion order is useful for diagnostics.
    /// Lookup is linear, but with ~30 keys that's still nanoseconds.
    metadata: Vec<(String, MetaValue)>,
    /// Tensor descriptors in file order.
    tensors: Vec<TensorInfo<'a>>,
    /// File-absolute offset where the aligned tensor-data section
    /// begins.
    tensor_data_start: usize,
    /// Effective alignment used for the tensor-data section.
    alignment: u64,
}

impl<'a> Gguf<'a> {
    /// Parse a GGUF v3 file from a byte slice. The returned `Gguf`
    /// borrows from `data` for tensor-name slices and tensor-data
    /// regions; metadata KVs are owned.
    pub fn parse(data: &'a [u8]) -> Result<Self, GgufError> {
        let mut r = Reader::new(data);

        // -- Header ---------------------------------------------------
        let magic = r.u32()?;
        if magic != GGUF_MAGIC {
            return Err(GgufError::BadMagic);
        }
        let version = r.u32()?;
        if version != GGUF_VERSION {
            return Err(GgufError::BadVersion);
        }
        let tensor_count = r.u64()?;
        let kv_count = r.u64()?;

        // Bound declared counts against bytes-remaining *before*
        // allocating. A header that lies (e.g. `u64::MAX`) would
        // otherwise cause `Vec::with_capacity` to panic / OOM-abort.
        let remaining = r.remaining_len() as u64;
        if kv_count > MAX_PLAUSIBLE_KVS || kv_count > remaining / MIN_KV_RECORD_SIZE {
            return Err(GgufError::OversizedKvCount);
        }
        if tensor_count > MAX_PLAUSIBLE_TENSORS
            || tensor_count > remaining / MIN_TENSOR_RECORD_SIZE
        {
            return Err(GgufError::OversizedTensorCount);
        }

        // -- Metadata -------------------------------------------------
        // Cap initial Vec capacity at the plausibility ceiling so a
        // truncated-but-large kv_count can't pre-reserve hundreds of MB.
        let kv_cap = (kv_count.min(MAX_PLAUSIBLE_KVS)) as usize;
        let mut metadata: Vec<(String, MetaValue)> = Vec::with_capacity(kv_cap);
        for _ in 0..kv_count {
            let key = r.string_owned()?;
            let value = read_meta_value(&mut r)?;
            metadata.push((key, value));
        }

        // -- Tensor descriptors --------------------------------------
        let tensor_cap = (tensor_count.min(MAX_PLAUSIBLE_TENSORS)) as usize;
        let mut tensors: Vec<TensorInfo<'a>> = Vec::with_capacity(tensor_cap);
        for _ in 0..tensor_count {
            let name = r.string_borrowed()?;
            let n_dims = r.u32()?;
            if n_dims > MAX_PLAUSIBLE_DIMS {
                return Err(GgufError::TooManyDims);
            }
            let mut dims: Vec<u64> = Vec::with_capacity(n_dims as usize);
            for _ in 0..n_dims {
                let d = r.u64()?;
                if d > MAX_PLAUSIBLE_DIM {
                    return Err(GgufError::DimTooLarge);
                }
                dims.push(d);
            }
            let ggml_type = GgmlType(r.u32()?);
            let offset = r.u64()?;
            tensors.push(TensorInfo {
                name,
                dims,
                ggml_type,
                offset,
            });
        }

        // -- Alignment + tensor-data section -------------------------
        let alignment = match Self::find_metadata(&metadata, "general.alignment") {
            Some(MetaValue::Uint32(v)) => *v as u64,
            Some(MetaValue::Uint64(v)) => *v,
            _ => DEFAULT_ALIGNMENT,
        };
        if alignment == 0 || !alignment.is_power_of_two() {
            return Err(GgufError::BadAlignment);
        }

        let header_end = r.pos();
        let pad = align_padding(header_end, alignment as usize)
            .ok_or(GgufError::Truncated)?;
        let tensor_data_start = header_end
            .checked_add(pad)
            .ok_or(GgufError::Truncated)?;
        if tensor_data_start > data.len() {
            return Err(GgufError::Truncated);
        }

        Ok(Self {
            data,
            version,
            metadata,
            tensors,
            tensor_data_start,
            alignment,
        })
    }

    /// Format version (always [`GGUF_VERSION`] for a successfully-parsed file).
    pub fn version(&self) -> u32 {
        self.version
    }

    /// Number of metadata KV pairs.
    pub fn metadata_count(&self) -> usize {
        self.metadata.len()
    }

    /// Number of tensor descriptors.
    pub fn tensor_count(&self) -> usize {
        self.tensors.len()
    }

    /// Tensor-data section alignment (defaults to [`DEFAULT_ALIGNMENT`]).
    pub fn alignment(&self) -> u64 {
        self.alignment
    }

    /// File-absolute offset where the tensor-data section begins.
    pub fn tensor_data_start(&self) -> usize {
        self.tensor_data_start
    }

    /// Look up a metadata value by key (linear scan; cheap for ~30 keys).
    pub fn metadata(&self, key: &str) -> Option<&MetaValue> {
        Self::find_metadata(&self.metadata, key)
    }

    /// Read-only view of all parsed tensor descriptors.
    pub fn tensors(&self) -> &[TensorInfo<'a>] {
        &self.tensors
    }

    /// Look up a tensor descriptor by name (linear scan).
    pub fn tensor(&self, name: &str) -> Option<&TensorInfo<'a>> {
        self.tensors.iter().find(|t| t.name == name)
    }

    /// Borrow this tensor's raw bytes from the input buffer.
    ///
    /// Returns `None` if the tensor's stated `(offset .. offset + size)`
    /// falls outside the tensor-data section. The `size` is recovered
    /// from the *next* tensor's offset (or, for the last tensor, from
    /// the end of the input buffer) — GGUF does not store per-tensor
    /// sizes on the wire, so this is the canonical recovery path used
    /// by `llama.cpp` and `ggml`.
    pub fn tensor_data(&self, info: &TensorInfo<'a>) -> Option<&'a [u8]> {
        // Find the index of `info` by pointer identity on its name —
        // names are slices of `data`, so this is an O(n) scan but
        // each comparison is just a pointer-equality check. For
        // typical use the caller has the index already; this is
        // correct-but-slow.
        let idx = self
            .tensors
            .iter()
            .position(|t| core::ptr::eq(t.name.as_ptr(), info.name.as_ptr()))?;
        self.tensor_data_at(idx)
    }

    /// Borrow tensor data by index. Faster than [`Self::tensor_data`]
    /// when the caller is iterating in order.
    pub fn tensor_data_at(&self, idx: usize) -> Option<&'a [u8]> {
        let info = self.tensors.get(idx)?;
        let abs_start = self.tensor_data_start.checked_add(info.offset as usize)?;
        let end_offset = if idx + 1 < self.tensors.len() {
            self.tensors[idx + 1].offset as usize
        } else {
            // Last tensor: reaches to end-of-buffer.
            self.data.len().checked_sub(self.tensor_data_start)?
        };
        let abs_end = self.tensor_data_start.checked_add(end_offset)?;
        if abs_end > self.data.len() || abs_start > abs_end {
            return None;
        }
        Some(&self.data[abs_start..abs_end])
    }

    fn find_metadata<'m>(
        metadata: &'m [(String, MetaValue)],
        key: &str,
    ) -> Option<&'m MetaValue> {
        metadata
            .iter()
            .find(|(k, _)| k == key)
            .map(|(_, v)| v)
    }
}

/// Compute the padding needed to align `pos` up to the next multiple
/// of `alignment`. Returns `None` if the addition would overflow.
fn align_padding(pos: usize, alignment: usize) -> Option<usize> {
    let rem = pos % alignment;
    if rem == 0 {
        Some(0)
    } else {
        alignment.checked_sub(rem)
    }
}

// ---------------------------------------------------------------------------
// Metadata-value reader
// ---------------------------------------------------------------------------

/// Recursive metadata-value reader. Top-level entry: reads the
/// 4-byte type tag then dispatches.
fn read_meta_value(r: &mut Reader<'_>) -> Result<MetaValue, GgufError> {
    let ty = MetaType::from_u32(r.u32()?)?;
    read_meta_value_with_type(r, ty)
}

/// Read a value of a *known* type. Used both for top-level KV values
/// (after the type tag is consumed by [`read_meta_value`]) and for
/// homogeneous array elements (where the type tag is hoisted out of
/// each element).
fn read_meta_value_with_type(r: &mut Reader<'_>, ty: MetaType) -> Result<MetaValue, GgufError> {
    Ok(match ty {
        MetaType::Uint8 => MetaValue::Uint8(r.u8()?),
        MetaType::Int8 => MetaValue::Int8(r.u8()? as i8),
        MetaType::Uint16 => MetaValue::Uint16(r.u16()?),
        MetaType::Int16 => MetaValue::Int16(r.u16()? as i16),
        MetaType::Uint32 => MetaValue::Uint32(r.u32()?),
        MetaType::Int32 => MetaValue::Int32(r.u32()? as i32),
        MetaType::Uint64 => MetaValue::Uint64(r.u64()?),
        MetaType::Int64 => MetaValue::Int64(r.u64()? as i64),
        MetaType::Float32 => MetaValue::Float32(f32::from_bits(r.u32()?)),
        MetaType::Float64 => MetaValue::Float64(f64::from_bits(r.u64()?)),
        MetaType::Bool => MetaValue::Bool(r.u8()? != 0),
        MetaType::String => MetaValue::String(r.string_owned()?),
        MetaType::Array => {
            let elem_type = MetaType::from_u32(r.u32()?)?;
            MetaValue::Array(read_array_of_type(r, elem_type)?)
        }
    })
}

/// Read a `[u64 length][N elements of type elem_type]` array body.
/// The element-type tag is *not* read here — callers must consume it
/// before calling. Centralizing the length sanity check here means
/// both the top-level and nested-array paths get the same bound.
fn read_array_of_type(r: &mut Reader<'_>, elem_type: MetaType) -> Result<MetaArray, GgufError> {
    let len = r.u64()?;
    let min_elem = elem_type.min_element_size();
    // `min_elem` is at least 1 by construction. Reject arrays whose
    // element bodies couldn't possibly fit in the bytes remaining.
    if len > r.remaining_len() as u64 / min_elem {
        return Err(GgufError::OversizedArrayLen);
    }
    // Cap *initial* capacity at MAX_PLAUSIBLE_ARRAY_LEN; the Vec will
    // grow as needed, but a 1 GB file with a 1-byte-element array of
    // claimed length 100M won't pre-reserve 100 MB on the spot.
    let cap = len.min(MAX_PLAUSIBLE_ARRAY_LEN) as usize;
    let mut values: Vec<MetaValue> = Vec::with_capacity(cap);
    for _ in 0..len {
        values.push(read_meta_value_with_type(r, elem_type)?);
    }
    Ok(MetaArray { elem_type, values })
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

/// Cursor over a byte slice with little-endian primitive readers.
struct Reader<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self { data, pos: 0 }
    }

    fn pos(&self) -> usize {
        self.pos
    }

    fn remaining_len(&self) -> usize {
        // `pos` is only advanced by `take`, which guarantees `pos <= data.len()`.
        self.data.len() - self.pos
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8], GgufError> {
        let end = self.pos.checked_add(n).ok_or(GgufError::Truncated)?;
        if end > self.data.len() {
            return Err(GgufError::Truncated);
        }
        let s = &self.data[self.pos..end];
        self.pos = end;
        Ok(s)
    }

    fn u8(&mut self) -> Result<u8, GgufError> {
        Ok(self.take(1)?[0])
    }

    fn u16(&mut self) -> Result<u16, GgufError> {
        let b = self.take(2)?;
        Ok(u16::from_le_bytes([b[0], b[1]]))
    }

    fn u32(&mut self) -> Result<u32, GgufError> {
        let b = self.take(4)?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    fn u64(&mut self) -> Result<u64, GgufError> {
        let b = self.take(8)?;
        Ok(u64::from_le_bytes([
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        ]))
    }

    /// Read a length-prefixed UTF-8 string and return it as a borrowed
    /// `&str` slice into the original buffer (zero-copy).
    fn string_borrowed(&mut self) -> Result<&'a str, GgufError> {
        let len = self.u64()?;
        if len > MAX_PLAUSIBLE_STRING_LEN {
            return Err(GgufError::OversizedString);
        }
        let len = usize::try_from(len).map_err(|_| GgufError::OversizedString)?;
        let bytes = self.take(len)?;
        core::str::from_utf8(bytes).map_err(|_| GgufError::BadUtf8)
    }

    /// Read a length-prefixed UTF-8 string and return an owned `String`.
    /// Used for metadata keys/values which need to outlive any lifetime
    /// the parser exposes (e.g. `&self.metadata` returns a reference
    /// into a `Vec<(String, MetaValue)>`, not into the input buffer).
    fn string_owned(&mut self) -> Result<String, GgufError> {
        // Reuse the borrowed reader so we get one place that enforces
        // the UTF-8 + length checks.
        Ok(String::from(self.string_borrowed()?))
    }
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/// Errors produced while parsing a GGUF file. Variants are unitless so
/// the type stays cheap to log under `no_std` (no formatting required
/// for a `Display`-free, `Debug`-only environment).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GgufError {
    /// Magic value at byte 0 didn't match `"GGUF"`.
    BadMagic,
    /// File version is not v3.
    BadVersion,
    /// File ended before we could read all expected bytes.
    Truncated,
    /// `general.alignment` was zero or not a power of two.
    BadAlignment,
    /// `tensor_count` is implausibly large or exceeds bytes remaining.
    OversizedTensorCount,
    /// `metadata_kv_count` is implausibly large or exceeds bytes remaining.
    OversizedKvCount,
    /// A length-prefixed string exceeded the 16 MB sanity ceiling.
    OversizedString,
    /// A metadata array length exceeded what could fit in the file.
    OversizedArrayLen,
    /// A tensor dimension exceeds the plausibility cap (`2^40`).
    DimTooLarge,
    /// A tensor declared more than [`MAX_PLAUSIBLE_DIMS`] dimensions.
    TooManyDims,
    /// Encountered a metadata-type tag this parser doesn't recognize.
    UnknownMetaType,
    /// A string field contained invalid UTF-8.
    BadUtf8,
}

impl fmt::Display for GgufError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let s = match self {
            GgufError::BadMagic => "GGUF: bad magic (expected 'GGUF')",
            GgufError::BadVersion => "GGUF: unsupported version (expected 3)",
            GgufError::Truncated => "GGUF: input truncated",
            GgufError::BadAlignment => {
                "GGUF: general.alignment must be a non-zero power of two"
            }
            GgufError::OversizedTensorCount => "GGUF: tensor_count exceeds plausibility cap",
            GgufError::OversizedKvCount => "GGUF: metadata_kv_count exceeds plausibility cap",
            GgufError::OversizedString => "GGUF: string field length exceeds cap",
            GgufError::OversizedArrayLen => "GGUF: array length exceeds remaining bytes",
            GgufError::DimTooLarge => "GGUF: tensor dimension exceeds 2^40",
            GgufError::TooManyDims => "GGUF: tensor declares too many dimensions",
            GgufError::UnknownMetaType => "GGUF: unknown metadata type tag",
            GgufError::BadUtf8 => "GGUF: string field is not valid UTF-8",
        };
        f.write_str(s)
    }
}

// ---------------------------------------------------------------------------
// M1.2 — Q4_K block layout
// ---------------------------------------------------------------------------
//
// Q4_K is the dominant quant for Qwen2.5-1.5B. Each super-block is
// 256 elements packed into a fixed 144-byte struct:
//
//   struct Q4_K_block {
//       f16    d;            // super-block scale     (offset   0..  2)
//       f16    dmin;         // super-block min       (offset   2..  4)
//       u8     scales[12];   // 8×6-bit scales + 8×6-bit mins
//                            //                       (offset   4.. 16)
//       u8     qs[128];      // 4-bit quantized values (256/2)
//                            //                       (offset  16..144)
//   }                                                                 144 B
//
// 5.625 bits/element including d/dmin overhead. Numeric dequantization
// (`dequantize_row_q4_K`, vec_dot, etc.) lands in M3 — for M1.2 we
// only expose the layout and a read-only block view so M3 can drop
// into place without reshuffling structs.

/// Bytes per Q4_K super-block (fixed by the GGML format).
pub const Q4_K_BLOCK_SIZE: usize = 144;

/// Elements per Q4_K super-block.
pub const Q4_K_BLOCK_ELEMENTS: usize = 256;

/// Number of Q4_K super-blocks needed to cover `elements` weights,
/// rounding up: GGML pads a partial trailing block out to 256.
pub const fn q4_k_block_count(elements: usize) -> usize {
    // `usize::div_ceil` is stable but not const on all toolchains we
    // target — keep the explicit form.
    elements / Q4_K_BLOCK_ELEMENTS
        + ((elements % Q4_K_BLOCK_ELEMENTS != 0) as usize)
}

/// On-disk byte size of a Q4_K-quantized row of `elements` weights.
/// Returns `None` if the multiplication would overflow `usize`.
pub fn q4_k_byte_size(elements: usize) -> Option<usize> {
    q4_k_block_count(elements).checked_mul(Q4_K_BLOCK_SIZE)
}

/// Same as [`q4_k_byte_size`] but panics on overflow. Provided for
/// callers that have already validated input bounds (e.g. from the
/// parsed tensor descriptor) and treat overflow as a programmer
/// error.
pub fn q4_k_byte_size_unchecked(elements: usize) -> usize {
    q4_k_block_count(elements) * Q4_K_BLOCK_SIZE
}

/// Borrowed view over a single Q4_K super-block. Constructed via
/// [`Q4KBlockView::from_slice`] — zero-copy, returns `None` if the
/// slice isn't exactly [`Q4_K_BLOCK_SIZE`] bytes.
///
/// The accessors are deliberately raw (return f16 bits via the f32
/// converter for `d`/`dmin`, unpacked u8 for scales/mins, packed u8
/// pair for quants). M3 will layer the actual numeric dequant on top.
pub struct Q4KBlockView<'a> {
    bytes: &'a [u8; Q4_K_BLOCK_SIZE],
}

impl<'a> Q4KBlockView<'a> {
    /// Wrap a 144-byte slice as a Q4_K block view.
    pub fn from_slice(bytes: &'a [u8]) -> Option<Self> {
        let arr: &'a [u8; Q4_K_BLOCK_SIZE] = bytes.try_into().ok()?;
        Some(Self { bytes: arr })
    }

    /// Super-block scale `d`, decoded from its f16 representation.
    pub fn d(&self) -> f32 {
        let bits = u16::from_le_bytes([self.bytes[0], self.bytes[1]]);
        f16_to_f32(bits)
    }

    /// Super-block min `dmin`, decoded from its f16 representation.
    pub fn dmin(&self) -> f32 {
        let bits = u16::from_le_bytes([self.bytes[2], self.bytes[3]]);
        f16_to_f32(bits)
    }

    /// 6-bit scale for sub-block `i` (0..8). The packed encoding lives
    /// in `scales[0..12]`; this accessor returns the raw 6-bit value.
    ///
    /// Layout (little-endian by sub-block index, matches GGML's
    /// `get_scale_min_k4`):
    /// - `i=0..4`: low 6 bits of `scales[i]`
    /// - `i=4..8`: low 4 bits of `scales[i+4]` from `scales[8..12]`
    ///   combined with the high 2 bits of `scales[i-4]` shifted into
    ///   bits 4..6.
    ///
    /// For M1.2 we just expose the 6 bits — M3 will assemble these
    /// into f32 scales using `d * scale`.
    pub fn scale(&self, i: usize) -> u8 {
        debug_assert!(i < 8);
        let s = &self.bytes[4..16];
        if i < 4 {
            s[i] & 0x3F
        } else {
            (s[i + 4] & 0x0F) | ((s[i - 4] >> 6) << 4)
        }
    }

    /// 6-bit min for sub-block `i` (0..8). Symmetric to [`Self::scale`]
    /// but reads the upper-nibble half of `scales[8..12]`.
    pub fn min(&self, i: usize) -> u8 {
        debug_assert!(i < 8);
        let s = &self.bytes[4..16];
        if i < 4 {
            s[i + 4] & 0x3F
        } else {
            (s[i + 4] >> 4) | ((s[i] >> 6) << 4)
        }
    }

    /// Packed quant byte at `idx` (0..128). Each byte contains two
    /// 4-bit values: `low = byte & 0x0F`, `high = byte >> 4`.
    /// M3 will provide the per-element accessor; this is the raw byte.
    pub fn quant(&self, idx: usize) -> u8 {
        debug_assert!(idx < 128);
        self.bytes[16 + idx]
    }

    /// Raw underlying bytes (full 144-byte super-block).
    pub fn as_bytes(&self) -> &'a [u8; Q4_K_BLOCK_SIZE] {
        self.bytes
    }
}

/// Convert an IEEE-754 binary16 bit pattern to `f32`. Pure software
/// implementation — `f16` arithmetic on bare-metal aarch64 has known
/// LLVM-legalizer issues (#141), so we do the conversion by hand.
///
/// The handful of corner cases (denormals, infinities, NaNs) are all
/// handled, but with denormal flushing rounded to the nearest float
/// — fine for GGUF metadata where we just need the f16 scale value
/// to round-trip with bit equality.
fn f16_to_f32(bits: u16) -> f32 {
    let sign = (bits >> 15) & 0x1;
    let exp = (bits >> 10) & 0x1F;
    let mant = bits & 0x3FF;

    let f32_bits = if exp == 0 {
        if mant == 0 {
            // Signed zero.
            (sign as u32) << 31
        } else {
            // Subnormal: normalize by shifting until the implicit 1
            // bit lands at position 10, then bake the exponent.
            let mut m = mant as u32;
            let mut e: i32 = 1;
            while (m & 0x400) == 0 {
                m <<= 1;
                e -= 1;
            }
            m &= 0x3FF;
            let f32_exp = (127 - 15 + e) as u32;
            ((sign as u32) << 31) | (f32_exp << 23) | (m << 13)
        }
    } else if exp == 0x1F {
        // Inf / NaN — set exponent to all-ones, preserve mantissa
        // (shifted into f32 mantissa width so NaN payloads survive).
        ((sign as u32) << 31) | (0xFF << 23) | ((mant as u32) << 13)
    } else {
        let f32_exp = (exp as u32) + (127 - 15);
        ((sign as u32) << 31) | (f32_exp << 23) | ((mant as u32) << 13)
    };
    f32::from_bits(f32_bits)
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
//
// These run on the host (`cargo test --target x86_64-unknown-linux-gnu`).
// They exercise the parser against a tiny synthetic GGUF built in-memory
// — we don't depend on the host-tools crate at runtime, so a stripped
// builder lives here under `#[cfg(test)]`.

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;
    use alloc::vec;

    // -- Synthetic GGUF builder (test-only) --------------------------

    struct TestBuilder {
        kvs: Vec<(String, MetaValue)>,
        tensors: Vec<TestTensor>,
        magic: u32,
        version: u32,
        alignment: u64,
    }

    struct TestTensor {
        name: String,
        dims: Vec<u64>,
        ggml_type: GgmlType,
        data: Vec<u8>,
    }

    impl TestBuilder {
        fn new() -> Self {
            Self {
                kvs: Vec::new(),
                tensors: Vec::new(),
                magic: GGUF_MAGIC,
                version: GGUF_VERSION,
                alignment: DEFAULT_ALIGNMENT,
            }
        }

        fn magic(mut self, m: u32) -> Self {
            self.magic = m;
            self
        }

        fn version(mut self, v: u32) -> Self {
            self.version = v;
            self
        }

        fn alignment(mut self, a: u64) -> Self {
            self.alignment = a;
            self.kvs.push((
                "general.alignment".to_string(),
                MetaValue::Uint32(a as u32),
            ));
            self
        }

        fn kv(mut self, k: &str, v: MetaValue) -> Self {
            self.kvs.push((k.to_string(), v));
            self
        }

        fn tensor(mut self, name: &str, dims: Vec<u64>, ty: GgmlType, data: Vec<u8>) -> Self {
            self.tensors.push(TestTensor {
                name: name.to_string(),
                dims,
                ggml_type: ty,
                data,
            });
            self
        }

        fn build(self) -> Vec<u8> {
            let mut out: Vec<u8> = Vec::new();
            out.extend_from_slice(&self.magic.to_le_bytes());
            out.extend_from_slice(&self.version.to_le_bytes());
            out.extend_from_slice(&(self.tensors.len() as u64).to_le_bytes());
            out.extend_from_slice(&(self.kvs.len() as u64).to_le_bytes());

            for (k, v) in &self.kvs {
                write_string(&mut out, k);
                write_meta_value(&mut out, v);
            }

            // Pack tensors back-to-back with no padding (matches
            // llama.cpp behaviour for unaligned types).
            let mut offset: u64 = 0;
            let mut tensor_offsets: Vec<u64> = Vec::with_capacity(self.tensors.len());
            for t in &self.tensors {
                tensor_offsets.push(offset);
                offset += t.data.len() as u64;
            }

            for (t, off) in self.tensors.iter().zip(tensor_offsets.iter()) {
                write_string(&mut out, &t.name);
                out.extend_from_slice(&(t.dims.len() as u32).to_le_bytes());
                for d in &t.dims {
                    out.extend_from_slice(&d.to_le_bytes());
                }
                out.extend_from_slice(&t.ggml_type.0.to_le_bytes());
                out.extend_from_slice(&off.to_le_bytes());
            }

            // Pad to alignment.
            let pad = (self.alignment - (out.len() as u64 % self.alignment)) % self.alignment;
            for _ in 0..pad {
                out.push(0);
            }

            for t in &self.tensors {
                out.extend_from_slice(&t.data);
            }

            out
        }
    }

    fn write_string(out: &mut Vec<u8>, s: &str) {
        out.extend_from_slice(&(s.len() as u64).to_le_bytes());
        out.extend_from_slice(s.as_bytes());
    }

    fn write_meta_value(out: &mut Vec<u8>, v: &MetaValue) {
        out.extend_from_slice(&v.meta_type().as_u32().to_le_bytes());
        write_meta_payload(out, v);
    }

    fn write_meta_payload(out: &mut Vec<u8>, v: &MetaValue) {
        match v {
            MetaValue::Uint8(x) => out.push(*x),
            MetaValue::Int8(x) => out.push(*x as u8),
            MetaValue::Uint16(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::Int16(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::Uint32(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::Int32(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::Uint64(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::Int64(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::Float32(x) => out.extend_from_slice(&x.to_bits().to_le_bytes()),
            MetaValue::Float64(x) => out.extend_from_slice(&x.to_bits().to_le_bytes()),
            MetaValue::Bool(x) => out.push(if *x { 1 } else { 0 }),
            MetaValue::String(s) => write_string(out, s),
            MetaValue::Array(arr) => {
                out.extend_from_slice(&arr.elem_type.as_u32().to_le_bytes());
                out.extend_from_slice(&(arr.values.len() as u64).to_le_bytes());
                for elem in &arr.values {
                    write_meta_payload(out, elem);
                }
            }
        }
    }

    // -- Header / round-trip -----------------------------------------

    #[test]
    fn parse_minimal_file() {
        let bytes = TestBuilder::new()
            .kv("general.architecture", MetaValue::String("qwen2".into()))
            .tensor("token_embd.weight", vec![4u64, 2u64], GgmlType::F32, vec![0u8; 32])
            .build();

        let g = Gguf::parse(&bytes).expect("parse");
        assert_eq!(g.version(), GGUF_VERSION);
        assert_eq!(g.metadata_count(), 1);
        assert_eq!(g.tensor_count(), 1);
        assert_eq!(
            g.metadata("general.architecture").and_then(MetaValue::as_str),
            Some("qwen2")
        );

        let t = g.tensor("token_embd.weight").expect("by name");
        assert_eq!(t.name, "token_embd.weight");
        assert_eq!(t.dims, vec![4, 2]);
        assert_eq!(t.ggml_type, GgmlType::F32);
        assert_eq!(t.offset, 0);

        let data = g.tensor_data(t).expect("tensor data");
        assert_eq!(data.len(), 32);
    }

    #[test]
    fn metadata_roundtrips_all_scalar_types() {
        let bytes = TestBuilder::new()
            .kv("u8", MetaValue::Uint8(0xAB))
            .kv("i8", MetaValue::Int8(-7))
            .kv("u16", MetaValue::Uint16(0xBEEF))
            .kv("i16", MetaValue::Int16(-300))
            .kv("u32", MetaValue::Uint32(0xDEADBEEF))
            .kv("i32", MetaValue::Int32(-1_000_000))
            .kv("u64", MetaValue::Uint64(u64::MAX / 2))
            .kv("i64", MetaValue::Int64(-1_000_000_000_000))
            .kv("f32", MetaValue::Float32(core::f32::consts::PI))
            .kv("f64", MetaValue::Float64(core::f64::consts::E))
            .kv("bool_t", MetaValue::Bool(true))
            .kv("bool_f", MetaValue::Bool(false))
            .kv("str", MetaValue::String("hello, world".into()))
            .build();

        let g = Gguf::parse(&bytes).expect("parse");
        assert!(matches!(g.metadata("u8"), Some(MetaValue::Uint8(0xAB))));
        assert!(matches!(g.metadata("i8"), Some(MetaValue::Int8(-7))));
        assert!(matches!(g.metadata("u16"), Some(MetaValue::Uint16(0xBEEF))));
        assert!(matches!(g.metadata("i16"), Some(MetaValue::Int16(-300))));
        assert!(matches!(g.metadata("u32"), Some(MetaValue::Uint32(0xDEADBEEF))));
        assert!(matches!(g.metadata("i32"), Some(MetaValue::Int32(-1_000_000))));
        assert!(matches!(g.metadata("u64"), Some(MetaValue::Uint64(_))));
        assert!(matches!(g.metadata("i64"), Some(MetaValue::Int64(-1_000_000_000_000))));
        assert!(matches!(g.metadata("bool_t"), Some(MetaValue::Bool(true))));
        assert!(matches!(g.metadata("bool_f"), Some(MetaValue::Bool(false))));
        match g.metadata("f32") {
            Some(MetaValue::Float32(v)) => assert_eq!(*v, core::f32::consts::PI),
            _ => panic!("f32 missing"),
        }
        match g.metadata("f64") {
            Some(MetaValue::Float64(v)) => assert_eq!(*v, core::f64::consts::E),
            _ => panic!("f64 missing"),
        }
        assert_eq!(g.metadata("str").and_then(MetaValue::as_str), Some("hello, world"));
    }

    #[test]
    fn metadata_array_of_strings() {
        let arr = MetaArray {
            elem_type: MetaType::String,
            values: vec![
                MetaValue::String("<|im_start|>".into()),
                MetaValue::String("<|im_end|>".into()),
            ],
        };
        let bytes = TestBuilder::new()
            .kv("tokenizer.special_tokens", MetaValue::Array(arr))
            .build();

        let g = Gguf::parse(&bytes).expect("parse");
        match g.metadata("tokenizer.special_tokens") {
            Some(MetaValue::Array(a)) => {
                assert_eq!(a.elem_type, MetaType::String);
                assert_eq!(a.values.len(), 2);
                assert_eq!(a.values[0].as_str(), Some("<|im_start|>"));
                assert_eq!(a.values[1].as_str(), Some("<|im_end|>"));
            }
            _ => panic!("expected array"),
        }
    }

    #[test]
    fn nested_array_roundtrips() {
        let inner = MetaArray {
            elem_type: MetaType::Uint32,
            values: vec![MetaValue::Uint32(1), MetaValue::Uint32(2)],
        };
        let outer = MetaArray {
            elem_type: MetaType::Array,
            values: vec![MetaValue::Array(inner)],
        };
        let bytes = TestBuilder::new().kv("nested", MetaValue::Array(outer)).build();
        let g = Gguf::parse(&bytes).expect("parse");
        match g.metadata("nested") {
            Some(MetaValue::Array(a)) => {
                assert_eq!(a.elem_type, MetaType::Array);
                assert_eq!(a.values.len(), 1);
                match &a.values[0] {
                    MetaValue::Array(inner) => {
                        assert_eq!(inner.elem_type, MetaType::Uint32);
                        assert_eq!(inner.values.len(), 2);
                    }
                    _ => panic!("inner not array"),
                }
            }
            _ => panic!("outer not array"),
        }
    }

    #[test]
    fn multiple_tensors_pack_back_to_back() {
        let bytes = TestBuilder::new()
            .tensor("a", vec![4u64], GgmlType::F32, vec![0xA1; 16])
            .tensor("b", vec![2u64], GgmlType::F32, vec![0xB2; 8])
            .tensor("c", vec![1u64], GgmlType::F32, vec![0xC3; 4])
            .build();

        let g = Gguf::parse(&bytes).expect("parse");
        assert_eq!(g.tensor_count(), 3);

        let a = g.tensor_data_at(0).expect("a");
        let b = g.tensor_data_at(1).expect("b");
        let c = g.tensor_data_at(2).expect("c");
        assert_eq!(a.len(), 16);
        assert_eq!(b.len(), 8);
        assert_eq!(c.len(), 4);
        assert!(a.iter().all(|&x| x == 0xA1));
        assert!(b.iter().all(|&x| x == 0xB2));
        assert!(c.iter().all(|&x| x == 0xC3));
    }

    #[test]
    fn custom_alignment_recoverable() {
        let bytes = TestBuilder::new()
            .alignment(64)
            .tensor("t", vec![4u64], GgmlType::F32, vec![0u8; 16])
            .build();
        let g = Gguf::parse(&bytes).expect("parse");
        assert_eq!(g.alignment(), 64);
        // tensor_data_start must be a multiple of 64.
        assert_eq!(g.tensor_data_start() % 64, 0);
    }

    // -- DoS gates ---------------------------------------------------

    #[test]
    fn rejects_bad_magic() {
        let bytes = TestBuilder::new().magic(0x12345678).build();
        assert!(matches!(Gguf::parse(&bytes), Err(GgufError::BadMagic)));
    }

    #[test]
    fn rejects_bad_version() {
        let bytes = TestBuilder::new().version(2).build();
        assert!(matches!(Gguf::parse(&bytes), Err(GgufError::BadVersion)));
    }

    #[test]
    fn rejects_truncated_header() {
        // 16 bytes is enough for magic+version+tensor_count but cuts
        // off mid-kv_count.
        let bytes = vec![b'G', b'G', b'U', b'F', 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
        assert!(matches!(Gguf::parse(&bytes), Err(GgufError::Truncated)));
    }

    #[test]
    fn rejects_truncated_string() {
        // Build a real header but lie about a metadata key length.
        let mut out: Vec<u8> = Vec::new();
        out.extend_from_slice(&GGUF_MAGIC.to_le_bytes());
        out.extend_from_slice(&GGUF_VERSION.to_le_bytes());
        out.extend_from_slice(&0u64.to_le_bytes());      // tensor_count
        out.extend_from_slice(&1u64.to_le_bytes());      // kv_count = 1
        // Key with claimed length 999 999 but only 4 bytes follow.
        out.extend_from_slice(&999_999u64.to_le_bytes());
        out.extend_from_slice(b"abcd");
        assert!(matches!(Gguf::parse(&out), Err(GgufError::Truncated)));
    }

    #[test]
    fn rejects_oversized_tensor_count() {
        let mut out: Vec<u8> = Vec::new();
        out.extend_from_slice(&GGUF_MAGIC.to_le_bytes());
        out.extend_from_slice(&GGUF_VERSION.to_le_bytes());
        // tensor_count = u64::MAX
        out.extend_from_slice(&u64::MAX.to_le_bytes());
        out.extend_from_slice(&0u64.to_le_bytes());
        assert!(matches!(
            Gguf::parse(&out),
            Err(GgufError::OversizedTensorCount)
        ));
    }

    #[test]
    fn rejects_oversized_kv_count() {
        let mut out: Vec<u8> = Vec::new();
        out.extend_from_slice(&GGUF_MAGIC.to_le_bytes());
        out.extend_from_slice(&GGUF_VERSION.to_le_bytes());
        out.extend_from_slice(&0u64.to_le_bytes());
        out.extend_from_slice(&u64::MAX.to_le_bytes());
        assert!(matches!(
            Gguf::parse(&out),
            Err(GgufError::OversizedKvCount)
        ));
    }

    #[test]
    fn rejects_non_power_of_two_alignment() {
        let bytes = TestBuilder::new()
            .kv("general.alignment", MetaValue::Uint32(48)) // not power of two
            .build();
        assert!(matches!(Gguf::parse(&bytes), Err(GgufError::BadAlignment)));
    }

    #[test]
    fn rejects_zero_alignment() {
        let bytes = TestBuilder::new()
            .kv("general.alignment", MetaValue::Uint32(0))
            .build();
        assert!(matches!(Gguf::parse(&bytes), Err(GgufError::BadAlignment)));
    }

    #[test]
    fn rejects_too_many_dims() {
        // Hand-craft: header says 1 tensor, dims = 99.
        let mut out: Vec<u8> = Vec::new();
        out.extend_from_slice(&GGUF_MAGIC.to_le_bytes());
        out.extend_from_slice(&GGUF_VERSION.to_le_bytes());
        out.extend_from_slice(&1u64.to_le_bytes()); // tensor_count
        out.extend_from_slice(&0u64.to_le_bytes()); // kv_count
        // Tensor: empty name + n_dims = 99
        out.extend_from_slice(&0u64.to_le_bytes()); // name length 0
        out.extend_from_slice(&99u32.to_le_bytes()); // n_dims
        // Pad enough trailing bytes so the count check at the top
        // doesn't trip first.
        for _ in 0..(99 * 8 + 32) {
            out.push(0);
        }
        assert!(matches!(Gguf::parse(&out), Err(GgufError::TooManyDims)));
    }

    #[test]
    fn rejects_unknown_meta_type() {
        // 1 KV with type tag 99.
        let mut out: Vec<u8> = Vec::new();
        out.extend_from_slice(&GGUF_MAGIC.to_le_bytes());
        out.extend_from_slice(&GGUF_VERSION.to_le_bytes());
        out.extend_from_slice(&0u64.to_le_bytes()); // tensor_count
        out.extend_from_slice(&1u64.to_le_bytes()); // kv_count
        out.extend_from_slice(&1u64.to_le_bytes()); // key length 1
        out.push(b'k');
        out.extend_from_slice(&99u32.to_le_bytes()); // unknown type tag
        // Trailing bytes so the count check doesn't trip.
        for _ in 0..32 {
            out.push(0);
        }
        assert!(matches!(Gguf::parse(&out), Err(GgufError::UnknownMetaType)));
    }

    #[test]
    fn rejects_bad_utf8_string() {
        let mut out: Vec<u8> = Vec::new();
        out.extend_from_slice(&GGUF_MAGIC.to_le_bytes());
        out.extend_from_slice(&GGUF_VERSION.to_le_bytes());
        out.extend_from_slice(&0u64.to_le_bytes());
        out.extend_from_slice(&1u64.to_le_bytes());
        // Key "k"
        out.extend_from_slice(&1u64.to_le_bytes());
        out.push(b'k');
        // Value: String type, length 2, bad UTF-8 sequence.
        out.extend_from_slice(&(MetaType::String as u32).to_le_bytes());
        out.extend_from_slice(&2u64.to_le_bytes());
        out.push(0xFF);
        out.push(0xFE);
        // Padding so EOF doesn't trip first.
        for _ in 0..32 {
            out.push(0);
        }
        assert!(matches!(Gguf::parse(&out), Err(GgufError::BadUtf8)));
    }

    // -- Q4_K layout -------------------------------------------------

    #[test]
    fn q4_k_block_size_constants() {
        assert_eq!(Q4_K_BLOCK_SIZE, 144);
        assert_eq!(Q4_K_BLOCK_ELEMENTS, 256);
    }

    #[test]
    fn q4_k_byte_size_math() {
        assert_eq!(q4_k_byte_size(0), Some(0));
        assert_eq!(q4_k_byte_size(256), Some(144));
        assert_eq!(q4_k_byte_size(512), Some(288));
        assert_eq!(q4_k_byte_size(255), Some(144)); // rounds up to 1 block
        assert_eq!(q4_k_byte_size(257), Some(288)); // rounds up to 2 blocks
        // 1536 (Qwen2.5 hidden size) → 6 blocks.
        assert_eq!(q4_k_byte_size(1536), Some(864));
        assert_eq!(q4_k_block_count(1536), 6);
    }

    #[test]
    fn q4_k_block_view_reads_d_dmin_scales() {
        // Build a synthetic 144-byte block.
        let mut block = [0u8; Q4_K_BLOCK_SIZE];
        // d = 1.0 in f16  -> 0x3C00
        block[0] = 0x00;
        block[1] = 0x3C;
        // dmin = -0.5 in f16 -> 0xB800
        block[2] = 0x00;
        block[3] = 0xB8;
        // scales[0..12]: deterministic pattern.
        for i in 0..12 {
            block[4 + i] = (i as u8) | ((i as u8) << 4);
        }
        // qs[0..128]: ascending bytes mod 256.
        for i in 0..128 {
            block[16 + i] = i as u8;
        }

        let view = Q4KBlockView::from_slice(&block).expect("view");
        assert_eq!(view.d(), 1.0);
        assert_eq!(view.dmin(), -0.5);

        // For i=0..3 the scale is the low 6 bits of scales[i].
        // scales[0] = 0x00 → scale(0) = 0.
        // scales[1] = 0x11 → scale(1) = 0x11 & 0x3F = 0x11.
        assert_eq!(view.scale(0), 0x00);
        assert_eq!(view.scale(1), 0x11);
        assert_eq!(view.scale(2), 0x22);
        assert_eq!(view.scale(3), 0x33);

        // For i=0..3, min = low 6 bits of scales[i+4].
        // scales[4] = 0x44 → min(0) = 0x04 (low 6 bits of 0x44).
        assert_eq!(view.min(0), 0x44 & 0x3F);
        assert_eq!(view.min(1), 0x55 & 0x3F);

        // quant(idx) returns raw byte at qs[idx].
        assert_eq!(view.quant(0), 0);
        assert_eq!(view.quant(127), 127);

        // as_bytes round-trips.
        assert_eq!(view.as_bytes()[0], 0x00);
        assert_eq!(view.as_bytes()[143], 127); // last qs byte
    }

    #[test]
    fn q4_k_block_view_rejects_wrong_size() {
        assert!(Q4KBlockView::from_slice(&[0u8; 143]).is_none());
        assert!(Q4KBlockView::from_slice(&[0u8; 145]).is_none());
        assert!(Q4KBlockView::from_slice(&[0u8; 144]).is_some());
    }

    #[test]
    fn f16_to_f32_well_known_values() {
        assert_eq!(f16_to_f32(0x0000), 0.0);
        assert_eq!(f16_to_f32(0x8000), -0.0);
        assert_eq!(f16_to_f32(0x3C00), 1.0);
        assert_eq!(f16_to_f32(0xBC00), -1.0);
        assert_eq!(f16_to_f32(0x4000), 2.0);
        assert_eq!(f16_to_f32(0x3800), 0.5);
        // Inf / -Inf
        assert!(f16_to_f32(0x7C00).is_infinite() && f16_to_f32(0x7C00).is_sign_positive());
        assert!(f16_to_f32(0xFC00).is_infinite() && f16_to_f32(0xFC00).is_sign_negative());
        // NaN
        assert!(f16_to_f32(0x7E00).is_nan());
    }
}
