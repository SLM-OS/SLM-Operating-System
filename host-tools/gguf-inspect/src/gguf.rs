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
}

/// GGML tensor element type (a subset of the 30+ types `ggml` defines —
/// we keep them as raw `u32` so unknown quants don't crash the host
/// tool).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GgmlType(pub u32);

impl GgmlType {
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

        // Metadata KV pairs.
        let mut metadata: BTreeMap<String, MetaValue> = BTreeMap::new();
        for _ in 0..kv_count {
            let key = r.string()?;
            let value = read_meta_value(&mut r)?;
            metadata.insert(key, value);
        }

        // Tensor descriptors.
        let mut tensors = Vec::with_capacity(tensor_count as usize);
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
                dims.push(r.u64()?);
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
            let len = r.u64()?;
            // Plausibility check — refuse arrays bigger than the file.
            if len > r.remaining_len() as u64 + 1 {
                return Err(GgufError::ArrayTooLong(len));
            }
            let mut values = Vec::with_capacity(len.min(1 << 20) as usize);
            for _ in 0..len {
                values.push(read_meta_value_with_type(r, elem_type)?);
            }
            MetaValue::Array(MetaArray { elem_type, values })
        }
    })
}

/// Read a value of a *known* type (used for array elements where the
/// type tag is hoisted out of each element).
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
            // Nested array — element type tag is still at the start.
            let elem_type = MetaType::from_u32(r.u32()?)?;
            let len = r.u64()?;
            let mut values = Vec::with_capacity(len.min(1 << 20) as usize);
            for _ in 0..len {
                values.push(read_meta_value_with_type(r, elem_type)?);
            }
            MetaValue::Array(MetaArray { elem_type, values })
        }
    })
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
