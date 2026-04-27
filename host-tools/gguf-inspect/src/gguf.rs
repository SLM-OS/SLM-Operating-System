//! GGUF v3 parser.
//!
//! Implements the [GGUF v3 format] used by `llama.cpp`/`ggml`. Returns
//! owned data structures — zero-copy mmap of the tensor-data region is
//! deliberately deferred to the runtime-side parser (M1) where it
//! actually matters. Host tooling values clarity over speed.
//!
//! [GGUF v3 format]: https://github.com/ggerganov/ggml/blob/master/docs/gguf.md
//!
//! # Layout
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
//!  │   value: typed (1 byte type tag + payload)   │
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

use std::collections::BTreeMap;
use std::fmt;

/// GGUF magic value `"GGUF"` in little-endian byte order.
pub const GGUF_MAGIC: u32 = 0x4655_4747;

/// Format version this parser understands.
pub const GGUF_VERSION: u32 = 3;

/// Default value of `general.alignment` per the GGUF spec.
pub const DEFAULT_ALIGNMENT: u64 = 32;

/// Sanity ceiling for `tensor_count` used when sizing a `Vec`. Real
/// GGUFs ship well under 1024 tensors; capping `with_capacity` at
/// `2^20` keeps a malicious header (e.g. `u64::MAX`) from aborting the
/// process via OOM. The actual loop still runs `tensor_count`
/// iterations, but each one only reserves what it needs and will
/// EOF-out cleanly on truncated input.
const MAX_PLAUSIBLE_TENSORS: u64 = 1 << 20;

/// Minimum on-disk size of a single tensor descriptor. Used as a lower
/// bound when checking that `tensor_count` doesn't exceed the bytes
/// remaining in the file: name length prefix (8) + zero-byte name +
/// n_dims (4) + ggml_type (4) + offset (8) = 24. Any real tensor will
/// be larger.
const MIN_TENSOR_RECORD_SIZE: u64 = 24;

/// Minimum on-disk size of a single KV pair: 8 (key length) + 0 (zero
/// byte name) + 4 (type tag) + 1 (smallest payload, e.g. u8/bool).
const MIN_KV_RECORD_SIZE: u64 = 13;

/// Reject any single tensor dimension > `2^40` (~1 trillion elements)
/// — these only happen via corruption.
const MAX_PLAUSIBLE_DIM: u64 = 1 << 40;

/// Cap on initial `Vec::with_capacity` for metadata arrays. The loop
/// will still push `len` elements (bounded by file size via
/// [`MetaType::min_element_size`]), but the *initial* allocation stays
/// modest so a 1 GB file with a billion 1-byte array can't pre-reserve
/// a billion-entry Vec.
const MAX_PLAUSIBLE_ARRAY_LEN: u64 = 1 << 20;

/// GGUF metadata-value type tag (1 byte on the wire).
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
            other => return Err(GgufError::UnknownMetaType(other)),
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

/// GGML tensor element type (a subset of the 30+ types `ggml` defines —
/// we keep them as raw `u32` so unknown quants don't crash the host
/// tool).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GgmlType(pub u32);

impl GgmlType {
    /// `f32` — IEEE-754 single precision.
    pub const F32: GgmlType = GgmlType(0);
    /// `f16` — IEEE-754 half precision.
    pub const F16: GgmlType = GgmlType(1);
    /// `q4_K` — 4-bit K-quants. Common for Qwen2/Llama weight tensors.
    pub const Q4_K: GgmlType = GgmlType(12);
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

impl fmt::Display for GgmlType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.name() {
            Some(n) => f.write_str(n),
            None => write!(f, "type{}", self.0),
        }
    }
}

/// A typed GGUF metadata value.
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
}

/// A homogeneous array of [`MetaValue`]s. The element-type tag is held
/// separately so empty arrays still round-trip.
#[derive(Debug, Clone, PartialEq)]
pub struct MetaArray {
    pub elem_type: MetaType,
    pub values: Vec<MetaValue>,
}

/// Description of one tensor in the file. The actual tensor bytes
/// start at `data_start + offset` in the file, where `data_start` is
/// the file-absolute offset returned by [`Gguf::tensor_data_start`].
#[derive(Debug, Clone, PartialEq)]
pub struct TensorInfo {
    pub name: String,
    pub dims: Vec<u64>,
    pub ggml_type: GgmlType,
    /// Offset relative to the start of the tensor-data section.
    pub offset: u64,
}

/// A parsed GGUF file (header + metadata + tensor descriptors).
#[derive(Debug, Clone)]
pub struct Gguf {
    pub version: u32,
    pub metadata: BTreeMap<String, MetaValue>,
    pub tensors: Vec<TensorInfo>,
    /// File-absolute offset where the aligned tensor-data section
    /// begins. Provided so callers can compute absolute tensor
    /// addresses (`data_start + tensor.offset`).
    pub tensor_data_start: u64,
    /// Effective alignment used for the tensor-data section. Comes
    /// from the `general.alignment` u32 KV if present, else
    /// [`DEFAULT_ALIGNMENT`].
    pub alignment: u64,
}

impl Gguf {
    /// Parse a GGUF v3 file from a byte slice.
    pub fn parse(data: &[u8]) -> Result<Self, GgufError> {
        let mut r = Reader::new(data);

        let magic = r.u32()?;
        if magic != GGUF_MAGIC {
            return Err(GgufError::BadMagic(magic));
        }
        let version = r.u32()?;
        if version != GGUF_VERSION {
            return Err(GgufError::UnsupportedVersion(version));
        }
        let tensor_count = r.u64()?;
        let kv_count = r.u64()?;

        // Cap declared counts against the bytes remaining in the file
        // *before* allocating. A header that lies (e.g. `u64::MAX`)
        // would otherwise OOM-abort the process via `Vec::with_capacity`.
        let remaining = r.remaining_len() as u64;
        if kv_count > remaining / MIN_KV_RECORD_SIZE {
            return Err(GgufError::TooManyKvs(kv_count));
        }
        if tensor_count > remaining / MIN_TENSOR_RECORD_SIZE {
            return Err(GgufError::TooManyTensors(tensor_count));
        }

        // Metadata KV pairs. `BTreeMap` has no `with_capacity` so no
        // explicit clamp is needed here — the loop just iterates the
        // (now bounded) `kv_count` and EOF-aborts cleanly on truncation.
        let mut metadata: BTreeMap<String, MetaValue> = BTreeMap::new();
        for _ in 0..kv_count {
            let key = r.string()?;
            let value = read_meta_value(&mut r)?;
            metadata.insert(key, value);
        }

        // Tensor descriptors.
        let mut tensors = Vec::with_capacity(tensor_count.min(MAX_PLAUSIBLE_TENSORS) as usize);
        for _ in 0..tensor_count {
            let name = r.string()?;
            let n_dims = r.u32()?;
            // Plausibility check — protects against malicious / corrupt
            // files asking us to allocate billions of dims.
            if n_dims > 8 {
                return Err(GgufError::TooManyDims(n_dims));
            }
            let mut dims = Vec::with_capacity(n_dims as usize);
            for _ in 0..n_dims {
                let d = r.u64()?;
                // Reject obviously-corrupt dimension values.
                if d > MAX_PLAUSIBLE_DIM {
                    return Err(GgufError::DimTooLarge(d));
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

        // Determine alignment from `general.alignment` if present.
        let alignment = match metadata.get("general.alignment") {
            Some(MetaValue::Uint32(v)) => *v as u64,
            Some(MetaValue::Uint64(v)) => *v,
            _ => DEFAULT_ALIGNMENT,
        };
        if alignment == 0 || !alignment.is_power_of_two() {
            return Err(GgufError::BadAlignment(alignment));
        }

        let header_end = r.pos() as u64;
        let pad = (alignment - (header_end % alignment)) % alignment;
        let tensor_data_start = header_end + pad;

        Ok(Self {
            version,
            metadata,
            tensors,
            tensor_data_start,
            alignment,
        })
    }

    /// Read-only access to the parsed tensor descriptors.
    pub fn tensors(&self) -> &[TensorInfo] {
        &self.tensors
    }

    /// Read-only access to the parsed metadata.
    pub fn metadata(&self) -> &BTreeMap<String, MetaValue> {
        &self.metadata
    }

    /// File-absolute offset where the tensor-data section begins.
    pub fn tensor_data_start(&self) -> u64 {
        self.tensor_data_start
    }

    /// The model architecture as declared by `general.architecture`,
    /// or `None` if the key is absent or non-string.
    pub fn architecture(&self) -> Option<&str> {
        match self.metadata.get("general.architecture")? {
            MetaValue::String(s) => Some(s.as_str()),
            _ => None,
        }
    }
}

/// Recursive metadata-value reader.
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
        MetaType::String => MetaValue::String(r.string()?),
        MetaType::Array => {
            let elem_type = MetaType::from_u32(r.u32()?)?;
            MetaValue::Array(read_array_of_type(r, elem_type)?)
        }
    })
}

/// Read a `[u64 length][N elements of type elem_type]` array body.
/// The element-type tag is *not* read here — callers must consume it
/// before calling. Centralizing the length sanity check here means
/// both the top-level and nested-array code paths get the same bound.
fn read_array_of_type(r: &mut Reader<'_>, elem_type: MetaType) -> Result<MetaArray, GgufError> {
    let len = r.u64()?;
    let min_elem = elem_type.min_element_size();
    // Reject arrays whose element bodies couldn't possibly fit in the
    // bytes remaining. `min_elem` is at least 1, so this also rejects
    // `len > remaining` for byte-sized elements.
    if min_elem > 0 && len > r.remaining_len() as u64 / min_elem {
        return Err(GgufError::ArrayTooLong(len));
    }
    // After the bound check above, `len * min_elem ≤ remaining_bytes`,
    // so `len` is at most file-size. Still cap the *initial* Vec
    // capacity at `MAX_PLAUSIBLE_ARRAY_LEN` so a 1 GB file doesn't
    // pre-allocate hundreds of MB up front; the Vec will grow if the
    // file genuinely contains more, and truncated input falls out as
    // `UnexpectedEof`.
    let cap = len.min(MAX_PLAUSIBLE_ARRAY_LEN) as usize;
    let mut values = Vec::with_capacity(cap);
    for _ in 0..len {
        values.push(read_meta_value_with_type(r, elem_type)?);
    }
    Ok(MetaArray { elem_type, values })
}

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
        self.data.len() - self.pos
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8], GgufError> {
        let end = self
            .pos
            .checked_add(n)
            .ok_or(GgufError::UnexpectedEof { need: n })?;
        if end > self.data.len() {
            return Err(GgufError::UnexpectedEof { need: n });
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

    fn string(&mut self) -> Result<String, GgufError> {
        let len = self.u64()? as usize;
        if len > self.remaining_len() {
            return Err(GgufError::UnexpectedEof { need: len });
        }
        let bytes = self.take(len)?;
        String::from_utf8(bytes.to_vec()).map_err(|_| GgufError::InvalidUtf8)
    }
}

/// Errors produced while parsing a GGUF file.
#[derive(Debug)]
pub enum GgufError {
    /// Magic value at byte 0 didn't match `"GGUF"`.
    BadMagic(u32),
    /// File version is not v3.
    UnsupportedVersion(u32),
    /// Encountered a metadata-type tag this parser doesn't recognize.
    UnknownMetaType(u32),
    /// File ended before we could read `need` bytes.
    UnexpectedEof { need: usize },
    /// String field contained invalid UTF-8.
    InvalidUtf8,
    /// Tensor descriptor claims more dims than we accept (sanity cap).
    TooManyDims(u32),
    /// A single tensor dimension exceeds the plausibility cap.
    DimTooLarge(u64),
    /// `tensor_count` exceeds the bytes remaining in the file (file is
    /// corrupt or hostile).
    TooManyTensors(u64),
    /// `metadata_kv_count` exceeds the bytes remaining in the file.
    TooManyKvs(u64),
    /// Array length exceeds remaining bytes — file is corrupt or
    /// hostile.
    ArrayTooLong(u64),
    /// `general.alignment` was zero or not a power of two.
    BadAlignment(u64),
}

impl fmt::Display for GgufError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            GgufError::BadMagic(m) => {
                write!(f, "bad GGUF magic: 0x{m:08x} (expected 0x{GGUF_MAGIC:08x})")
            }
            GgufError::UnsupportedVersion(v) => {
                write!(f, "unsupported GGUF version {v} (expected {GGUF_VERSION})")
            }
            GgufError::UnknownMetaType(t) => write!(f, "unknown metadata type tag {t}"),
            GgufError::UnexpectedEof { need } => {
                write!(f, "unexpected EOF: needed {need} more bytes")
            }
            GgufError::InvalidUtf8 => f.write_str("string field is not valid UTF-8"),
            GgufError::TooManyDims(n) => {
                write!(f, "tensor declares {n} dimensions (max 8)")
            }
            GgufError::DimTooLarge(d) => {
                write!(f, "tensor dimension {d} exceeds sanity cap (2^40)")
            }
            GgufError::TooManyTensors(n) => {
                write!(f, "header claims {n} tensors, exceeds remaining file size")
            }
            GgufError::TooManyKvs(n) => {
                write!(
                    f,
                    "header claims {n} metadata KV pairs, exceeds remaining file size"
                )
            }
            GgufError::ArrayTooLong(n) => {
                write!(f, "metadata array claims {n} elements, exceeds file size")
            }
            GgufError::BadAlignment(a) => {
                write!(f, "general.alignment = {a} (must be non-zero power of two)")
            }
        }
    }
}

impl std::error::Error for GgufError {}
