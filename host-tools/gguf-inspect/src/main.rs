//! `gguf-inspect` CLI.
//!
//! Pretty-prints a GGUF v3 file's header, metadata, and tensor list.
//! See `--help` for usage. Hand-rolled arg parsing follows the
//! convention from `host-tools/gsp-harness` — no `clap` dep.
//!
//! # Subcommands
//!
//! - `inspect <PATH>` (default if the first positional looks like a
//!   path) — pretty-print header, metadata, and tensors.
//! - `dump-vocab <PATH> [--out FILE]` — extract the BBPE vocab +
//!   merges + special-token IDs from a GGUF and emit a self-contained
//!   `*.vocb` v1 binary blob (see [`gguf_inspect::vocab_blob`]).

use std::fs;
use std::io::{self, Write};
use std::path::PathBuf;
use std::process::ExitCode;

use gguf_inspect::{
    write_vocab_blob, Gguf, GgufError, MetaArray, MetaType, MetaValue, SpecialTokenIds, TensorInfo,
};

const USAGE: &str = "\
gguf-inspect — inspect a GGUF v3 model file.

USAGE:
    gguf-inspect <PATH> [FLAGS]
    gguf-inspect inspect <PATH> [FLAGS]
    gguf-inspect dump-vocab <PATH> [--out FILE]

INSPECT FLAGS:
    --list-tensors      Print only the tensor list (skip metadata).
    --list-metadata     Print only the metadata KV pairs (skip tensors).

DUMP-VOCAB FLAGS:
    --out FILE          Write blob to FILE instead of stdout.

GLOBAL FLAGS:
    -h, --help          Show this help message.
    -V, --version       Print the gguf-inspect version.

By default (no subcommand), `inspect` is run with both metadata and
tensors printed after the header summary.
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

enum Command {
    Inspect(InspectArgs),
    DumpVocab(DumpVocabArgs),
}

#[derive(Default)]
struct InspectArgs {
    path: Option<PathBuf>,
    list_tensors: bool,
    list_metadata: bool,
}

#[derive(Default)]
struct DumpVocabArgs {
    path: Option<PathBuf>,
    out: Option<PathBuf>,
}

fn parse_args() -> Result<Command, String> {
    let mut args: Vec<String> = std::env::args().skip(1).collect();

    // Handle --help / --version up front so they work regardless of
    // subcommand position.
    for a in &args {
        match a.as_str() {
            "-h" | "--help" => {
                print!("{USAGE}");
                std::process::exit(0);
            }
            "-V" | "--version" => {
                println!("gguf-inspect {}", env!("CARGO_PKG_VERSION"));
                std::process::exit(0);
            }
            _ => {}
        }
    }

    if args.is_empty() {
        return Err(format!("missing subcommand or <PATH>\n\n{USAGE}"));
    }

    // Detect explicit subcommand. Anything else falls back to the
    // legacy `<PATH> [flags]` form for backward compatibility with
    // M0.1 callers.
    let cmd = match args[0].as_str() {
        "inspect" => {
            args.remove(0);
            Command::Inspect(parse_inspect(args)?)
        }
        "dump-vocab" => {
            args.remove(0);
            Command::DumpVocab(parse_dump_vocab(args)?)
        }
        _ => Command::Inspect(parse_inspect(args)?),
    };
    Ok(cmd)
}

fn parse_inspect(args: Vec<String>) -> Result<InspectArgs, String> {
    let mut a = InspectArgs::default();
    for arg in args {
        match arg.as_str() {
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

fn parse_dump_vocab(args: Vec<String>) -> Result<DumpVocabArgs, String> {
    let mut a = DumpVocabArgs::default();
    let mut iter = args.into_iter();
    while let Some(arg) = iter.next() {
        match arg.as_str() {
            "--out" => {
                let v = iter
                    .next()
                    .ok_or_else(|| format!("--out requires a value\n\n{USAGE}"))?;
                a.out = Some(PathBuf::from(v));
            }
            other if other.starts_with("--out=") => {
                a.out = Some(PathBuf::from(&other["--out=".len()..]));
            }
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
        return Err(format!("dump-vocab: missing <PATH>\n\n{USAGE}"));
    }
    Ok(a)
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let cmd = parse_args().map_err(|e| -> Box<dyn std::error::Error> { e.into() })?;
    match cmd {
        Command::Inspect(args) => run_inspect(args),
        Command::DumpVocab(args) => run_dump_vocab(args),
    }
}

fn run_inspect(args: InspectArgs) -> Result<(), Box<dyn std::error::Error>> {
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

fn run_dump_vocab(args: DumpVocabArgs) -> Result<(), Box<dyn std::error::Error>> {
    let path = args.path.expect("validated above");
    let bytes = fs::read(&path).map_err(|e| format!("reading {}: {e}", path.display()))?;
    let gguf =
        Gguf::parse(&bytes).map_err(|e: GgufError| format!("parsing {}: {e}", path.display()))?;

    let vocab = extract_byte_array(&gguf, "tokenizer.ggml.tokens")
        .ok_or("tokenizer.ggml.tokens missing or wrong type")?;
    let merges = extract_byte_array(&gguf, "tokenizer.ggml.merges").unwrap_or_default();
    let specials = SpecialTokenIds {
        bos: extract_token_id(&gguf, "tokenizer.ggml.bos_token_id"),
        eos: extract_token_id(&gguf, "tokenizer.ggml.eos_token_id"),
        pad: extract_token_id(&gguf, "tokenizer.ggml.padding_token_id"),
        unk: extract_token_id(&gguf, "tokenizer.ggml.unknown_token_id"),
        sep: extract_token_id(&gguf, "tokenizer.ggml.separator_token_id"),
    };

    match args.out {
        Some(out_path) => {
            let mut f = fs::File::create(&out_path)
                .map_err(|e| format!("creating {}: {e}", out_path.display()))?;
            write_vocab_blob(&mut f, &vocab, &merges, specials)
                .map_err(|e| format!("writing {}: {e}", out_path.display()))?;
            f.flush()
                .map_err(|e| format!("flushing {}: {e}", out_path.display()))?;
            eprintln!(
                "wrote {} ({} vocab, {} merges)",
                out_path.display(),
                vocab.len(),
                merges.len()
            );
        }
        None => {
            let stdout = io::stdout();
            let mut handle = stdout.lock();
            write_vocab_blob(&mut handle, &vocab, &merges, specials)
                .map_err(|e| format!("writing stdout: {e}"))?;
            handle
                .flush()
                .map_err(|e| format!("flushing stdout: {e}"))?;
        }
    }
    Ok(())
}

/// Pull a `tokenizer.ggml.*_token_id` from metadata, returning `-1` if
/// the key is absent or not an unsigned integer. The wire type is u32
/// in real Qwen2 GGUFs; we accept the other unsigned widths defensively.
fn extract_token_id(g: &Gguf, key: &str) -> i32 {
    match g.metadata().get(key) {
        Some(MetaValue::Uint32(v)) => i32::try_from(*v).unwrap_or(-1),
        Some(MetaValue::Uint64(v)) => i32::try_from(*v).unwrap_or(-1),
        Some(MetaValue::Int32(v)) => *v,
        Some(MetaValue::Int64(v)) => i32::try_from(*v).unwrap_or(-1),
        _ => -1,
    }
}

/// Pull a `tokenizer.ggml.*` array of strings from metadata as raw byte
/// vectors. Returns `None` if the key is absent or the value is not an
/// `Array<String>`.
fn extract_byte_array(g: &Gguf, key: &str) -> Option<Vec<Vec<u8>>> {
    match g.metadata().get(key)? {
        MetaValue::Array(a) if a.elem_type == MetaType::String => {
            let mut out = Vec::with_capacity(a.values.len());
            for v in &a.values {
                if let MetaValue::String(s) = v {
                    out.push(s.as_bytes().to_vec());
                } else {
                    // Heterogeneous array — bail.
                    return None;
                }
            }
            Some(out)
        }
        _ => None,
    }
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
