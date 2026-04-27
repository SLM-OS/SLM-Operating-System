//! `gguf-inspect` CLI.
//!
//! Pretty-prints a GGUF v3 file's header, metadata, and tensor list.
//! See `--help` for usage. Hand-rolled arg parsing follows the
//! convention from `host-tools/gsp-harness` — no `clap` dep.

use std::fs;
use std::path::PathBuf;
use std::process::ExitCode;

use gguf_inspect::{Gguf, GgufError, MetaArray, MetaType, MetaValue, TensorInfo};

const USAGE: &str = "\
gguf-inspect — inspect a GGUF v3 model file.

USAGE:
    gguf-inspect <PATH> [FLAGS]

FLAGS:
    --list-tensors      Print only the tensor list (skip metadata).
    --list-metadata     Print only the metadata KV pairs (skip tensors).
    -h, --help          Show this help message.
    -V, --version       Print the gguf-inspect version.

By default, both metadata and tensors are printed (after the header
summary). When neither --list-tensors nor --list-metadata is given,
both sections are shown.
";

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("error: {e}");
            ExitCode::FAILURE
        }
    }
}

#[derive(Default)]
struct Args {
    path: Option<PathBuf>,
    list_tensors: bool,
    list_metadata: bool,
}

fn parse_args() -> Result<Args, String> {
    let mut a = Args::default();
    for arg in std::env::args().skip(1) {
        match arg.as_str() {
            "-h" | "--help" => {
                print!("{USAGE}");
                std::process::exit(0);
            }
            "-V" | "--version" => {
                println!("gguf-inspect {}", env!("CARGO_PKG_VERSION"));
                std::process::exit(0);
            }
            "--list-tensors" => a.list_tensors = true,
            "--list-metadata" => a.list_metadata = true,
            other if other.starts_with('-') => {
                return Err(format!("unknown flag: {other}\n\n{USAGE}"));
            }
            other => {
                if a.path.is_some() {
                    return Err(format!("unexpected positional argument: {other}"));
                }
                a.path = Some(PathBuf::from(other));
            }
        }
    }
    if a.path.is_none() {
        return Err(format!("missing <PATH>\n\n{USAGE}"));
    }
    Ok(a)
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let args = parse_args().map_err(|e| -> Box<dyn std::error::Error> { e.into() })?;
    let path = args.path.expect("validated above");

    let bytes = fs::read(&path).map_err(|e| format!("reading {}: {e}", path.display()))?;
    let gguf =
        Gguf::parse(&bytes).map_err(|e: GgufError| format!("parsing {}: {e}", path.display()))?;

    print_header(&gguf);

    let show_meta = !args.list_tensors;
    let show_tensors = !args.list_metadata;

    if show_meta {
        println!();
        print_metadata(&gguf);
    }
    if show_tensors {
        println!();
        print_tensors(&gguf);
    }
    Ok(())
}

fn print_header(g: &Gguf) {
    println!("gguf v{}", g.version);
    if let Some(arch) = g.architecture() {
        println!("  arch: {arch}");
    }
    println!("  tensors: {}", g.tensors().len());
    println!("  metadata: {} entries", g.metadata().len());
    println!(
        "  alignment: {} (tensor data starts at file offset 0x{:x})",
        g.alignment, g.tensor_data_start
    );
}

fn print_metadata(g: &Gguf) {
    println!("[metadata]");
    let key_width = g.metadata().keys().map(|k| k.len()).max().unwrap_or(0);
    for (key, value) in g.metadata() {
        println!(
            "  {:width$} = {}",
            key,
            format_value(value),
            width = key_width
        );
    }
}

fn print_tensors(g: &Gguf) {
    println!("[tensors]");
    let name_width = g.tensors().iter().map(|t| t.name.len()).max().unwrap_or(0);
    let dims_width = g
        .tensors()
        .iter()
        .map(|t| format_dims(&t.dims).len())
        .max()
        .unwrap_or(0);
    let type_width = g
        .tensors()
        .iter()
        .map(|t| t.ggml_type.to_string().len())
        .max()
        .unwrap_or(0);

    for t in g.tensors() {
        print_tensor(t, name_width, dims_width, type_width);
    }
}

fn print_tensor(t: &TensorInfo, name_width: usize, dims_width: usize, type_width: usize) {
    let dims = format_dims(&t.dims);
    println!(
        "  {:nw$}  {:dw$}  {:tw$}  offset=0x{:x}",
        t.name,
        dims,
        t.ggml_type,
        t.offset,
        nw = name_width,
        dw = dims_width,
        tw = type_width,
    );
}

fn format_dims(dims: &[u64]) -> String {
    let mut s = String::from("[");
    for (i, d) in dims.iter().enumerate() {
        if i > 0 {
            s.push_str(", ");
        }
        s.push_str(&d.to_string());
    }
    s.push(']');
    s
}

fn format_value(v: &MetaValue) -> String {
    match v {
        MetaValue::Uint8(x) => x.to_string(),
        MetaValue::Int8(x) => x.to_string(),
        MetaValue::Uint16(x) => x.to_string(),
        MetaValue::Int16(x) => x.to_string(),
        MetaValue::Uint32(x) => x.to_string(),
        MetaValue::Int32(x) => x.to_string(),
        MetaValue::Uint64(x) => x.to_string(),
        MetaValue::Int64(x) => x.to_string(),
        MetaValue::Float32(x) => format!("{x}"),
        MetaValue::Float64(x) => format!("{x}"),
        MetaValue::Bool(x) => x.to_string(),
        MetaValue::String(s) => format!("{s:?}"),
        MetaValue::Array(arr) => format_array(arr),
    }
}

/// Compact array formatting — prints up to 8 elements then an ellipsis
/// with the total length. Long string arrays (e.g. tokenizer vocabs)
/// would otherwise dump 150k+ entries.
fn format_array(arr: &MetaArray) -> String {
    const MAX_INLINE: usize = 8;
    let n = arr.values.len();
    let mut s = format!("<{}> [", arr_elem_label(arr.elem_type));
    let take = n.min(MAX_INLINE);
    for (i, v) in arr.values.iter().take(take).enumerate() {
        if i > 0 {
            s.push_str(", ");
        }
        s.push_str(&format_value(v));
    }
    if n > take {
        s.push_str(&format!(", ... ({} total)", n));
    }
    s.push(']');
    s
}

fn arr_elem_label(t: MetaType) -> &'static str {
    match t {
        MetaType::Uint8 => "u8",
        MetaType::Int8 => "i8",
        MetaType::Uint16 => "u16",
        MetaType::Int16 => "i16",
        MetaType::Uint32 => "u32",
        MetaType::Int32 => "i32",
        MetaType::Uint64 => "u64",
        MetaType::Int64 => "i64",
        MetaType::Float32 => "f32",
        MetaType::Float64 => "f64",
        MetaType::Bool => "bool",
        MetaType::String => "string",
        MetaType::Array => "array",
    }
}
