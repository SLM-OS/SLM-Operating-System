//! Minimal protobuf wire format parser for ONNX model loading.
//!
//! Implements just enough of the protobuf wire format to parse ONNX files:
//! varint decoding, length-delimited fields, and nested message iteration.
//!
//! Zero-allocation: operates entirely on borrowed `&[u8]` slices.

/// Protobuf wire types (only the ones needed for ONNX).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum WireType {
    Varint = 0,
    Fixed64 = 1,
    LengthDelimited = 2,
    Fixed32 = 5,
}

impl WireType {
    fn from_u8(v: u8) -> Result<Self, ParseError> {
        match v {
            0 => Ok(WireType::Varint),
            1 => Ok(WireType::Fixed64),
            2 => Ok(WireType::LengthDelimited),
            5 => Ok(WireType::Fixed32),
            // Wire types 3, 4 are deprecated (start/end group)
            other => Err(ParseError::InvalidWireType(other)),
        }
    }
}

/// Data payload of a protobuf field.
#[derive(Debug, Clone, Copy)]
pub enum FieldData<'a> {
    /// Varint-encoded integer (wire type 0).
    Varint(u64),
    /// 32-bit fixed value (wire type 5).
    Fixed32(u32),
    /// 64-bit fixed value (wire type 1).
    Fixed64(u64),
    /// Length-delimited bytes (wire type 2).
    /// Could be a string, nested message, or packed repeated field.
    Bytes(&'a [u8]),
}

/// A single protobuf field parsed from the wire.
#[derive(Debug, Clone, Copy)]
pub struct ProtoField<'a> {
    pub field_number: u32,
    pub wire_type: WireType,
    pub data: FieldData<'a>,
}

/// Errors from protobuf parsing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ParseError {
    /// Input ended before a complete value could be read.
    UnexpectedEof,
    /// Varint encoding is invalid (too many bytes).
    InvalidVarint,
    /// Wire type byte is not a recognized type.
    InvalidWireType(u8),
    /// Length-delimited field extends past end of buffer.
    LengthOverflow,
    /// Field number is zero (invalid in protobuf).
    InvalidFieldNumber,
}

/// Decode a varint from a byte slice.
///
/// Returns `(value, bytes_consumed)` on success.
/// Varints use 7 bits per byte with the high bit as continuation flag.
pub fn decode_varint(data: &[u8]) -> Result<(u64, usize), ParseError> {
    let mut value: u64 = 0;
    let mut shift: u32 = 0;

    for (i, &byte) in data.iter().enumerate() {
        if shift >= 64 {
            return Err(ParseError::InvalidVarint);
        }
        value |= ((byte & 0x7F) as u64) << shift;
        shift += 7;
        if byte & 0x80 == 0 {
            return Ok((value, i + 1));
        }
        // Max 10 bytes for a 64-bit varint
        if i >= 9 {
            return Err(ParseError::InvalidVarint);
        }
    }

    Err(ParseError::UnexpectedEof)
}

/// Read a little-endian u32 from a byte slice.
fn read_u32_le(data: &[u8]) -> Result<u32, ParseError> {
    if data.len() < 4 {
        return Err(ParseError::UnexpectedEof);
    }
    Ok(u32::from_le_bytes([data[0], data[1], data[2], data[3]]))
}

/// Read a little-endian u64 from a byte slice.
fn read_u64_le(data: &[u8]) -> Result<u64, ParseError> {
    if data.len() < 8 {
        return Err(ParseError::UnexpectedEof);
    }
    Ok(u64::from_le_bytes([
        data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
    ]))
}

/// Iterator over protobuf fields in a byte buffer.
///
/// Yields `ProtoField` values containing zero-copy references into the
/// original buffer for length-delimited data.
pub struct ProtoIter<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> ProtoIter<'a> {
    /// Create a new iterator over protobuf fields in `data`.
    pub fn new(data: &'a [u8]) -> Self {
        Self { data, pos: 0 }
    }

    /// Get current position in the buffer.
    pub fn position(&self) -> usize {
        self.pos
    }

    /// Check if all data has been consumed.
    pub fn is_empty(&self) -> bool {
        self.pos >= self.data.len()
    }
}

impl<'a> Iterator for ProtoIter<'a> {
    type Item = Result<ProtoField<'a>, ParseError>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.pos >= self.data.len() {
            return None;
        }

        let remaining = &self.data[self.pos..];

        // Decode the tag (field_number << 3 | wire_type)
        let (tag, tag_len) = match decode_varint(remaining) {
            Ok(v) => v,
            Err(e) => return Some(Err(e)),
        };

        let wire_type_raw = (tag & 0x07) as u8;
        let field_number = (tag >> 3) as u32;

        if field_number == 0 {
            return Some(Err(ParseError::InvalidFieldNumber));
        }

        let wire_type = match WireType::from_u8(wire_type_raw) {
            Ok(wt) => wt,
            Err(e) => return Some(Err(e)),
        };

        let value_start = self.pos + tag_len;
        let value_data = &self.data[value_start..];

        let (data, consumed) = match wire_type {
            WireType::Varint => {
                match decode_varint(value_data) {
                    Ok((val, len)) => (FieldData::Varint(val), len),
                    Err(e) => return Some(Err(e)),
                }
            }
            WireType::Fixed32 => {
                match read_u32_le(value_data) {
                    Ok(val) => (FieldData::Fixed32(val), 4),
                    Err(e) => return Some(Err(e)),
                }
            }
            WireType::Fixed64 => {
                match read_u64_le(value_data) {
                    Ok(val) => (FieldData::Fixed64(val), 8),
                    Err(e) => return Some(Err(e)),
                }
            }
            WireType::LengthDelimited => {
                let (len, len_bytes) = match decode_varint(value_data) {
                    Ok(v) => v,
                    Err(e) => return Some(Err(e)),
                };
                let len = len as usize;
                let data_start = len_bytes;
                if data_start + len > value_data.len() {
                    return Some(Err(ParseError::LengthOverflow));
                }
                let bytes = &value_data[data_start..data_start + len];
                (FieldData::Bytes(bytes), len_bytes + len)
            }
        };

        self.pos = value_start + consumed;

        Some(Ok(ProtoField {
            field_number,
            wire_type,
            data,
        }))
    }
}

// =============================================================================
// Helper functions for extracting typed data from fields
// =============================================================================

/// Extract a varint value from a field, or return 0 if wrong type.
pub fn field_as_u64(field: &ProtoField) -> u64 {
    match field.data {
        FieldData::Varint(v) => v,
        _ => 0,
    }
}

/// Extract a varint value as i64.
pub fn field_as_i64(field: &ProtoField) -> i64 {
    match field.data {
        FieldData::Varint(v) => v as i64,
        _ => 0,
    }
}

/// Extract bytes from a length-delimited field.
pub fn field_as_bytes<'a>(field: &ProtoField<'a>) -> Option<&'a [u8]> {
    match field.data {
        FieldData::Bytes(b) => Some(b),
        _ => None,
    }
}

/// Extract a string from a length-delimited field.
pub fn field_as_str<'a>(field: &ProtoField<'a>) -> Option<&'a [u8]> {
    field_as_bytes(field)
}

/// Iterate over packed varint values in a length-delimited field.
pub struct PackedVarintIter<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> PackedVarintIter<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self { data, pos: 0 }
    }
}

impl<'a> Iterator for PackedVarintIter<'a> {
    type Item = Result<u64, ParseError>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.pos >= self.data.len() {
            return None;
        }
        match decode_varint(&self.data[self.pos..]) {
            Ok((val, len)) => {
                self.pos += len;
                Some(Ok(val))
            }
            Err(e) => Some(Err(e)),
        }
    }
}

/// Iterate over packed i64 values (as signed).
pub fn packed_varint_i64(data: &[u8]) -> impl Iterator<Item = Result<i64, ParseError>> + '_ {
    PackedVarintIter::new(data).map(|r| r.map(|v| v as i64))
}

/// Read a packed repeated float field (little-endian f32 values).
/// Returns the number of floats in the packed data.
pub fn packed_float_count(data: &[u8]) -> usize {
    data.len() / 4
}

/// Read a single f32 from a packed float array at the given index.
pub fn packed_float_at(data: &[u8], index: usize) -> Option<f32> {
    let offset = index * 4;
    if offset + 4 > data.len() {
        return None;
    }
    let bytes = [data[offset], data[offset + 1], data[offset + 2], data[offset + 3]];
    Some(f32::from_le_bytes(bytes))
}
