//! Component Manifest Parser
//!
//! Parses simple key-value manifest format.
//! Full YAML support deferred to future phase.

use super::state::{ComponentInfo, ComponentType, Priority};

/// Parse error types.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ParseError {
    /// Empty or null input
    EmptyInput,
    /// Invalid key-value format
    InvalidFormat,
    /// Missing required field
    MissingField(&'static str),
    /// Invalid field value
    InvalidValue(&'static str),
}

/// Parse a simple key-value manifest.
///
/// Expected format (newline-separated):
/// ```text
/// name: my-component
/// version: 1.0.0
/// type: service
/// priority: normal
/// description: Optional description
/// ```
///
/// Returns ComponentInfo on success, ParseError on failure.
pub fn parse_manifest(data: &[u8]) -> Result<ComponentInfo, ParseError> {
    if data.is_empty() {
        return Err(ParseError::EmptyInput);
    }

    let mut info = ComponentInfo::new();
    let mut has_name = false;
    let mut has_version = false;
    let mut has_type = false;

    // Parse line by line
    let mut start = 0;
    for i in 0..=data.len() {
        let is_newline = i == data.len() || data[i] == b'\n' || data[i] == b'\r';

        if is_newline {
            let line = &data[start..i];
            if !line.is_empty() && line[0] != b'#' {
                // Parse key: value
                if let Some(colon_pos) = line.iter().position(|&b| b == b':') {
                    let key = trim_bytes(&line[..colon_pos]);
                    let value = trim_bytes(&line[colon_pos + 1..]);

                    if !key.is_empty() && !value.is_empty() {
                        match key {
                            b"name" => {
                                info.set_name(value);
                                has_name = true;
                            }
                            b"version" => {
                                info.set_version(value);
                                has_version = true;
                            }
                            b"type" => {
                                info.component_type = parse_type(value)?;
                                has_type = true;
                            }
                            b"priority" => {
                                info.priority = parse_priority(value);
                            }
                            b"description" => {
                                info.set_description(value);
                            }
                            _ => {
                                // Unknown field - ignore for forward compatibility
                            }
                        }
                    }
                }
            }

            // Move to next line
            start = i + 1;
            // Skip \r\n pairs
            if i + 1 < data.len() && data[i] == b'\r' && data[i + 1] == b'\n' {
                start = i + 2;
            }
        }
    }

    // Validate required fields
    if !has_name {
        return Err(ParseError::MissingField("name"));
    }
    if !has_version {
        return Err(ParseError::MissingField("version"));
    }
    if !has_type {
        return Err(ParseError::MissingField("type"));
    }

    Ok(info)
}

/// Trim leading and trailing whitespace from bytes.
fn trim_bytes(data: &[u8]) -> &[u8] {
    let start = data.iter().position(|&b| b != b' ' && b != b'\t').unwrap_or(data.len());
    let end = data.iter().rposition(|&b| b != b' ' && b != b'\t').map(|i| i + 1).unwrap_or(0);

    if start >= end {
        &[]
    } else {
        &data[start..end]
    }
}

/// Parse component type from string.
fn parse_type(value: &[u8]) -> Result<ComponentType, ParseError> {
    match value {
        b"service" => Ok(ComponentType::Service),
        b"driver" => Ok(ComponentType::Driver),
        b"application" => Ok(ComponentType::Application),
        _ => Err(ParseError::InvalidValue("type")),
    }
}

/// Parse priority from string.
fn parse_priority(value: &[u8]) -> Priority {
    match value {
        b"idle" => Priority::Idle,
        b"low" => Priority::Low,
        b"normal" => Priority::Normal,
        b"high" => Priority::High,
        b"critical" => Priority::Critical,
        _ => Priority::Normal, // Default
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_simple_manifest() {
        let manifest = b"name: test-component\nversion: 1.0.0\ntype: service\n";
        let result = parse_manifest(manifest);
        assert!(result.is_ok());

        let info = result.unwrap();
        assert_eq!(info.name_str(), "test-component");
        assert_eq!(info.version_str(), "1.0.0");
        assert_eq!(info.component_type, ComponentType::Service);
        assert_eq!(info.priority, Priority::Normal);
    }

    #[test]
    fn test_parse_with_priority() {
        let manifest = b"name: high-prio\nversion: 2.0.0\ntype: driver\npriority: high\n";
        let result = parse_manifest(manifest);
        assert!(result.is_ok());

        let info = result.unwrap();
        assert_eq!(info.priority, Priority::High);
    }

    #[test]
    fn test_missing_name() {
        let manifest = b"version: 1.0.0\ntype: service\n";
        let result = parse_manifest(manifest);
        assert_eq!(result, Err(ParseError::MissingField("name")));
    }
}
