#!/usr/bin/env python3
"""Regenerate the SLM golden fixtures under scripts/fixtures/slm/ (#475).

Nothing here needs to run in CI: the outputs are checked in and the tests
only read them. Regenerate only to move to a newer reference, and read the
diff when doing so, since a changed golden means the reference moved.

Needs a CPU build of upstream llama.cpp (targets llama-tokenize and
llama-quantize) and its gguf-py package, plus the real Qwen2.5-1.5B GGUF for
the tokenizer vocabulary:

    git clone --depth 1 https://github.com/ggml-org/llama.cpp <dir>
    cmake -S <dir> -B <dir>/build -DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF
    cmake --build <dir>/build -j --target llama-tokenize llama-quantize llama
    scripts/tools/slm-gen-goldens.py --llama-cpp <dir> \
        --model <path>/qwen2.5-1.5b-instruct-q4_k_m.gguf

Outputs:
    qwen_vocab.gguf        Tokenizer metadata of the real model, no tensors.
    tokenizer_goldens.tsv  llama-tokenize ids over that vocabulary.
    golden_ops.rec         ggml / numpy reference vectors for single ops.
    golden_qkv_bias.rec    ggml's Q/K/V projections with their bias adds.
    tiny_qwen2.gguf        A small Qwen2 model quantized by llama-quantize.
    tiny_qwen2_greedy.rec  llama.cpp's greedy tokens and logits on that model.
    qwen25_real_golden.rec llama.cpp's greedy tokens on the real model and the
                           exact CPU reference's logits for the same steps, for
                           `slm golden` on hardware (also needs cargo).
    PROVENANCE.md          Reference commit, inputs and how to read the files.

`--only qkv-bias` or `--only real-model` adds that one fixture, at the
llama.cpp commit PROVENANCE.md pins, without touching the others. A llama.cpp
tree without its .git directory (a GitHub source archive) needs
`--llama-commit`.
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "scripts" / "fixtures" / "slm"
DRIVER_SRC = REPO / "scripts" / "tools" / "slm-golden-driver.cpp"

# Dimensions shaped like Qwen2.5-1.5B (12 query heads, 2 KV heads, FFN wider
# than the hidden state), scaled down. Quantized tensors need their first
# dimension to be a multiple of 256, which sets the floor on hidden and FFN.
TINY_BLOCKS = 2
TINY_HIDDEN = 256
TINY_FF = 512
TINY_HEADS = 8
TINY_HEADS_KV = 2
TINY_CTX = 128
TINY_ROPE_BASE = 1_000_000.0
TINY_PROMPT = "hello the"
TINY_GENERATE = 8
# Reference and SLM-OS round differently (F32 vs FP16 hidden state), and Q8_K
# turns that into logit noise of up to about 1 on this model, so a greedy token
# only proves something where the winner leads by clearly more than that.
TINY_MIN_MARGIN = 1.5
# The first-position (pos == 0) check uses this single-token prompt.
TINY_FIRST_PROMPT = "hello"

TOKENIZER_CASES = [
    ("ascii", "Hello, world! This is a test."),
    ("ascii_lower", "the quick brown fox jumps over the lazy dog"),
    ("contractions", "I'll say we've done it, don't you think? It's John's."),
    ("upper_contraction", "IT'S DON'T WE'LL"),
    ("digits", "In 2024, 12345 people paid $3.14 each."),
    ("digit_run", "0123456789"),
    ("punct_run", "wow!!! really??? (yes) [no] {maybe} ...---..."),
    ("punct_newline", "End of line.\nNext line!\n\nAnother?\r\nDone"),
    ("symbols", "a+b=c; x->y && z||w #define <tag/> @home ~/path"),
    ("url", "https://example.com/a/b?q=1&r=2#frag"),
    ("code", "fn main() {\n    let x = vec![1, 2, 3];\n    println!(\"{:?}\", x);\n}\n"),
    ("spaces_inner", "a  b   c    d"),
    ("spaces_lead", "   leading spaces"),
    ("spaces_trail", "trailing spaces   "),
    ("spaces_only", "     "),
    ("single_space", " "),
    ("tabs", "col1\tcol2\t\tcol3\t"),
    ("newlines_only", "\n\n\n"),
    ("single_newline", "\n"),
    ("crlf", "one\r\ntwo\r\n\r\nthree"),
    ("space_newline_mix", "x \n y  \n\n  z \t\n"),
    ("latin_accents", "café naïve Zürich résumé señor"),
    ("cyrillic", "Привет, мир! Как дела?"),
    ("greek", "Καλημέρα κόσμε"),
    ("arabic", "مرحبا بالعالم"),
    ("hebrew", "שלום עולם"),
    ("devanagari", "नमस्ते दुनिया"),
    ("cjk", "你好，世界！今天天气很好。"),
    ("japanese", "こんにちは世界、カタカナ漢字"),
    ("korean", "안녕하세요 세계"),
    ("emoji", "smile 😀 family 👨‍👩‍👧 flag 🇺🇸 end"),
    ("combining", "é ä ñ"),
    ("nbsp_marks", "a b c　d"),
    ("mixed_script", "GPT-4o和Qwen2.5比较，score=0.95!"),
    ("long_word", "pneumonoultramicroscopicsilicovolcanoconiosis"),
    ("long_word2", "supercalifragilisticexpialidocious" * 3),
    ("long_run", "a" * 200),
    ("long_digits", "9" * 64),
    ("chatml_literal", "<|im_start|>user\nHi<|im_end|>\n"),
]

CHAT_CASES = [
    ("chat_short", [("user", "Hi!")]),
    ("chat_system", [("system", "You are a helpful assistant."),
                     ("user", "What is 2 + 2?")]),
    ("chat_multi", [("system", "Be brief."), ("user", "Name a color."),
                    ("assistant", "Blue."), ("user", "Another one, please.")]),
    ("chat_unicode", [("user", "翻译成中文: good morning\n\nThanks 😀")]),
    # Content that starts with whitespace merges with the role's newline when
    # llama.cpp tokenizes the rendered template as one text (#475 W7).
    ("chat_lead_newline", [("user", "\nHello")]),
    ("chat_lead_newlines", [("user", "\n\n\nHello there")]),
    ("chat_lead_space", [("user", " Hi")]),
    ("chat_lead_spaces", [("user", "    indented code")]),
    ("chat_lead_tab", [("user", "\tTabbed")]),
    ("chat_lead_crlf", [("user", "\r\nWindows line")]),
    ("chat_only_space", [("user", "   ")]),
    ("chat_system_lead_newline", [("system", "\nBe brief."), ("user", " \nHi")]),
    ("chat_trailing_space", [("user", "Hi  \n")]),
]


def run(*args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def write_records(path, records):
    with open(path, "wb") as f:
        f.write(b"SGR1")
        f.write(struct.pack("<I", len(records)))
        for name in sorted(records):
            data = records[name]
            raw = name.encode()
            f.write(struct.pack("<I", len(raw)) + raw)
            f.write(struct.pack("<I", len(data)) + data)
            f.write(b"\0" * (((len(data) + 3) & ~3) - len(data)))


def read_records(path):
    b = Path(path).read_bytes()
    assert b[:4] == b"SGR1"
    count = struct.unpack_from("<I", b, 4)[0]
    p = 8
    out = {}
    for _ in range(count):
        nl = struct.unpack_from("<I", b, p)[0]; p += 4
        name = b[p:p + nl].decode(); p += nl
        dl = struct.unpack_from("<I", b, p)[0]; p += 4
        out[name] = b[p:p + dl]
        p += (dl + 3) & ~3
    return out


def f32(a):
    return np.asarray(a, dtype="<f4").tobytes()


def half_exact(a):
    """Round to values FP16 holds exactly. SLM-OS keeps hidden state in FP16
    and the reference runs in F32, so the shared input must be representable
    in both or the comparison would measure the input rounding."""
    return np.asarray(a, dtype=np.float16).astype(np.float32)


def build_driver(llama, build_dir):
    exe = build_dir / "slm-golden-driver"
    bin_dir = llama / "build" / "bin"
    run("g++", "-std=c++17", "-O2", DRIVER_SRC, "-o", exe,
        f"-I{llama}/include", f"-I{llama}/ggml/include",
        f"-L{bin_dir}", "-lllama", "-lggml-cpu", "-lggml-base", "-lggml",
        f"-Wl,-rpath,{bin_dir}")
    return exe


# --- tokenizer ---------------------------------------------------------------

def extract_vocab(gguf, model, out_path):
    r = gguf.GGUFReader(str(model))
    w = gguf.GGUFWriter(str(out_path), "qwen2")
    f = r.fields
    w.add_name("qwen2.5-1.5b-instruct tokenizer only")
    w.add_block_count(int(f["qwen2.block_count"].contents()))
    w.add_context_length(int(f["qwen2.context_length"].contents()))
    w.add_embedding_length(int(f["qwen2.embedding_length"].contents()))
    w.add_feed_forward_length(int(f["qwen2.feed_forward_length"].contents()))
    w.add_head_count(int(f["qwen2.attention.head_count"].contents()))
    w.add_head_count_kv(int(f["qwen2.attention.head_count_kv"].contents()))
    w.add_rope_freq_base(float(f["qwen2.rope.freq_base"].contents()))
    w.add_layer_norm_rms_eps(float(f["qwen2.attention.layer_norm_rms_epsilon"].contents()))
    w.add_tokenizer_model(f["tokenizer.ggml.model"].contents())
    w.add_tokenizer_pre(f["tokenizer.ggml.pre"].contents())
    w.add_token_list(f["tokenizer.ggml.tokens"].contents())
    w.add_token_types(f["tokenizer.ggml.token_type"].contents())
    w.add_token_merges(f["tokenizer.ggml.merges"].contents())
    w.add_eos_token_id(int(f["tokenizer.ggml.eos_token_id"].contents()))
    w.add_pad_token_id(int(f["tokenizer.ggml.padding_token_id"].contents()))
    w.add_bos_token_id(int(f["tokenizer.ggml.bos_token_id"].contents()))
    w.add_add_bos_token(bool(f["tokenizer.ggml.add_bos_token"].contents()))
    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()


def llama_tokenize(llama, model, text, parse_special, tmp):
    p = Path(tmp) / "tok.txt"
    p.write_bytes(text.encode("utf-8"))
    args = [llama / "build/bin/llama-tokenize", "-m", model, "--ids",
            "--log-disable", "--no-bos", "-f", p]
    if not parse_special:
        args.append("--no-parse-special")
    out = run(*args, capture_output=True, text=True).stdout.strip()
    out = out.strip("[]").strip()
    return [int(x) for x in out.split(",")] if out else []


def render_chat(turns):
    s = "".join(f"<|im_start|>{r}\n{c}<|im_end|>\n" for r, c in turns)
    return s + "<|im_start|>assistant\n"


def gen_tokenizer(llama, vocab_path, full_model, tmp):
    lines = []
    for name, text in TOKENIZER_CASES:
        ids = llama_tokenize(llama, vocab_path, text, False, tmp)
        # The extracted vocabulary must behave as the whole model does.
        full = llama_tokenize(llama, full_model, text, False, tmp)
        assert ids == full, f"vocab fixture differs from model on {name}"
        lines.append(f"plain\t{name}\t{text.encode().hex()}\t" + ",".join(map(str, ids)))
    for name, turns in CHAT_CASES:
        text = render_chat(turns)
        ids = llama_tokenize(llama, vocab_path, text, True, tmp)
        full = llama_tokenize(llama, full_model, text, True, tmp)
        assert ids == full, f"vocab fixture differs from model on {name}"
        spec = ";".join(f"{r}={c.encode().hex()}" for r, c in turns)
        lines.append(f"chat\t{name}\t{spec}\t" + ",".join(map(str, ids)))
    (OUT / "tokenizer_goldens.tsv").write_text("\n".join(lines) + "\n")


# --- single ops ----------------------------------------------------------------

def gen_ops(driver, tmp):
    rng = np.random.default_rng(475)
    inp = {}
    inp["rmsnorm.x"] = f32(half_exact(rng.normal(0, 2.0, 256)))
    inp["rmsnorm.gamma"] = f32(half_exact(rng.normal(1.0, 0.25, 256)))

    head_dim, n_head = 32, 2
    positions = [0, 1, 7, 63]
    inp["rope.x"] = f32(half_exact(rng.normal(0, 1.5, head_dim * n_head)))
    inp["rope.params"] = f32([head_dim, n_head, 1_000_000.0])
    inp["rope.positions"] = f32(positions)

    inp["swiglu.gate"] = f32(half_exact(rng.normal(0, 2.0, 512)))
    inp["swiglu.up"] = f32(half_exact(rng.normal(0, 2.0, 512)))

    cols, rows = 512, 8
    inp["quant.x"] = f32(half_exact(rng.normal(0, 1.0, cols)))
    inp["quant.src"] = f32(rng.normal(0, 0.1, cols * rows))
    inp["quant.dims"] = f32([cols, rows])

    in_path = Path(tmp) / "ops_in.rec"
    out_path = Path(tmp) / "ops_out.rec"
    write_records(in_path, inp)
    run(driver, "ops", in_path, out_path)
    out = read_records(out_path)

    # matmul_quant_rows takes an F32 dot over dequantized Q6_K weights, where
    # ggml goes through Q8_K. So also record that dot, with ggml's dequantized
    # weights, in double precision: without it a 1% error stays inside the
    # Q8_K rounding bound.
    x = np.frombuffer(inp["quant.x"], dtype="<f4").astype(np.float64)
    q6 = np.frombuffer(out["q6k.dequant"], dtype="<f4").astype(np.float64).reshape(rows, cols)
    out["q6k.dot_exact"] = f32(q6 @ x)

    # GQA in numpy, from build_attn_mha's definition: Q heads share K/V heads
    # in order (repeat-interleave), kq = K Q^T * 1/sqrt(head_dim), softmax over
    # positions, output = softmax-weighted sum of V.
    n_q, n_kv, gdim, seq = 8, 2, 16, 5
    q = half_exact(rng.normal(0, 1.5, (n_q, gdim)))
    k = half_exact(rng.normal(0, 1.5, (seq, n_kv, gdim)))
    v = half_exact(rng.normal(0, 1.5, (seq, n_kv, gdim)))
    group = n_q // n_kv
    gqa = np.zeros((n_q, gdim), dtype=np.float64)
    for hq in range(n_q):
        hkv = hq // group
        logits = (k[:, hkv].astype(np.float64) @ q[hq].astype(np.float64)) / np.sqrt(gdim)
        w = np.exp(logits - logits.max())
        w /= w.sum()
        gqa[hq] = w @ v[:, hkv].astype(np.float64)
    inp["gqa.q"] = f32(q)
    inp["gqa.k"] = f32(k)
    inp["gqa.v"] = f32(v)
    inp["gqa.dims"] = f32([n_q, n_kv, gdim, seq])
    out["gqa.out"] = f32(gqa)

    merged = {**inp, **out}
    write_records(OUT / "golden_ops.rec", merged)


# --- Q/K/V projections with their biases (#1165) ---------------------------------

# Shaped like one Qwen2 attention block, scaled down: 8 query heads and 2 KV
# heads of 64, so Q has 4 times the rows of K and V. V comes in both types
# Qwen2.5-1.5B Q4_K_M uses for it (14 layers each).
QKV_HIDDEN = 512
QKV_PROJECTIONS = [
    # name, ggml type, rows, bias sigma. The sigmas follow the real model's
    # biases (layer 0: Q std 2.6, K std 69 with entries near +/-300, V 0.23).
    ("q", 12, 512, 2.5),    # Q4_K
    ("k", 12, 128, 60.0),   # Q4_K
    ("v4", 12, 128, 0.25),  # Q4_K
    ("v6", 14, 128, 0.25),  # Q6_K
]


def gen_qkv_bias(driver, tmp):
    """`golden_qkv_bias.rec`: ggml's `mul_mat` plus `add`, per projection.

    A file of its own, so adding it changed no existing golden. Q6_K also gets
    an exact dot over ggml's dequantized weights and each block's sum of
    |w|, because SLM-OS takes an F32 dot there and ggml goes through Q8_K
    (see `check_lm_head` in runtime/src/slm/golden.rs)."""
    rng = np.random.default_rng(1165)
    x = half_exact(rng.normal(0, 1.0, QKV_HIDDEN))
    inp = {"qkv_bias.x": f32(x),
           "qkv_bias.names": ",".join(p[0] for p in QKV_PROJECTIONS).encode()}
    biases = {}
    for name, gtype, rows, sigma in QKV_PROJECTIONS:
        pre = f"qkv_bias.{name}."
        inp[pre + "type"] = f32([gtype])
        inp[pre + "src"] = f32(rng.normal(0, 0.05, rows * QKV_HIDDEN))
        b = rng.normal(0, sigma, rows).astype(np.float32)
        biases[name] = b
        inp[pre + "b"] = f32(b)
    in_path = Path(tmp) / "qkv_in.rec"
    out_path = Path(tmp) / "qkv_out.rec"
    write_records(in_path, inp)
    run(driver, "qkv-bias", in_path, out_path)
    out = read_records(out_path)

    keep = {k: v for k, v in inp.items() if not k.endswith(".src")}
    for name, gtype, rows, _ in QKV_PROJECTIONS:
        pre = f"qkv_bias.{name}."
        keep[pre + "w"] = out[pre + "w"]
        keep[pre + "out"] = out[pre + "out"]
        if gtype == 14:
            w = np.frombuffer(out[pre + "dequant"], dtype="<f4").astype(np.float64)
            w = w.reshape(rows, QKV_HIDDEN)
            keep[pre + "out_exact"] = f32(w @ x.astype(np.float64) + biases[name])
            keep[pre + "wabs"] = f32(np.abs(w).reshape(rows, QKV_HIDDEN // 256, 256).sum(axis=2))
    write_records(OUT / "golden_qkv_bias.rec", keep)


# --- the real model, for the hardware target (#1165) -------------------------------

REAL_MODEL_SHA256 = "6a1a2eb6d15622bf3c96857206351ba97e1af16c30d7a74ee38970e434e9407e"
# Each case: its name, how many greedy tokens to compare, the prompt length it
# needs (None: any), and candidate prompts tried in order, the first whose
# continuation qualifies being kept. A candidate is raw text, or a file under
# the repository holding a rendered chat prompt (special tokens parsed).
# `long` is 64 tokens, the most rows one GPU program step takes, so in batch
# mode its prompt goes through the largest prefill; the benchmark prompts of
# bench/475 are exactly that long.
REAL_CASES = [
    ("short", 8, None, [
        ("text", "The capital of France is Paris. The capital of Germany is"),
        ("text", "Monday, Tuesday, Wednesday, Thursday,"),
        ("text", "One, two, three, four, five,"),
    ]),
    ("long", 4, 64, [
        ("file", "bench/475/prompts/p1.txt"),
        ("file", "bench/475/prompts/p2.txt"),
        ("file", "bench/475/prompts/p3.txt"),
    ]),
]
# Every compared step needs a clear winner: the board must match llama.cpp's
# top token, and llama.cpp's activation quantization moves logits by up to
# about 1 (ADR 0004), so a near tie would test the reference, not SLM-OS.
REAL_MIN_MARGIN = 1.0
# <|endoftext|> and <|im_end|>: a continuation that stops early is no use.
REAL_STOP_IDS = {151643, 151645}
# The layer whose Q, K and V biases `slm golden ... drop-bias` zeroes.
REAL_MUTATE_LAYER = 0


def gen_real(llama, driver, model, tmp):
    """`qwen25_real_golden.rec`: llama.cpp's greedy run on the real model for
    each case, then the exact CPU reference over the same tokens. The
    reference comes from the runtime's host test
    `golden_real_model_write_fixture` (the GPU program on the software GPU,
    which runs the exact reference), so cargo runs here."""
    if sha256(model) != REAL_MODEL_SHA256:
        raise SystemExit(f"{model} is not the model the fixture is for")
    llama_recs = {}
    chosen = []
    for name, steps, length, candidates in REAL_CASES:
        for kind, value in candidates:
            if kind == "text":
                text = value
                ids = llama_tokenize(llama, model, text, False, tmp)
            else:
                text = (REPO / value).read_text()
                ids = llama_tokenize(llama, model, text, True, tmp)
            greedy = Path(tmp) / "real_greedy.rec"
            run(driver, "greedy", model, ",".join(map(str, ids)), steps, greedy)
            rec = read_records(greedy)
            toks = np.frombuffer(rec["greedy.tokens"], dtype="<i4")
            margins = np.frombuffer(rec["greedy.margins"], dtype="<f4")
            print(f"{name}: {kind} {value!r}: {len(ids)} ids, tokens {toks.tolist()} "
                  f"min margin {margins.min():.3f}")
            if ((length is not None and len(ids) != length)
                    or margins.min() < REAL_MIN_MARGIN or REAL_STOP_IDS & set(toks.tolist())):
                continue
            llama_recs.update({
                f"{name}.prompt.text": text.encode(),
                f"{name}.prompt.ids": np.asarray(ids, dtype="<u4").tobytes(),
                f"{name}.tokens": rec["greedy.tokens"],
                f"{name}.margins": rec["greedy.margins"],
            })
            chosen.append((name, value, len(ids), toks.tolist(), float(margins.min())))
            break
        else:
            raise SystemExit(f"no prompt met the criteria for case {name}")
    llama_recs["cases"] = ",".join(c[0] for c in chosen).encode()
    llama_rec = Path(tmp) / "real_llama.rec"
    write_records(llama_rec, llama_recs)
    out = OUT / "qwen25_real_golden.rec"
    env = dict(os.environ, SLM_GOLDEN_MODEL=str(model), SLM_REAL_LLAMA_REC=str(llama_rec),
               SLM_REAL_LAYER=str(REAL_MUTATE_LAYER), SLM_REAL_OUT=str(out))
    r = subprocess.run(
        ["cargo", "test", "--release", "--target", "x86_64-unknown-linux-gnu",
         "--features", "slm", "golden_real_model_write_fixture", "--",
         "--ignored", "--nocapture", "--test-threads=1"],
        cwd=REPO / "runtime", env=env)
    if r.returncode != 0:
        raise SystemExit("the exact reference disagreed with llama.cpp's top token")
    return chosen


# --- tiny model ----------------------------------------------------------------

def gpt2_byte_chars():
    bs = list(range(ord("!"), ord("~") + 1)) + list(range(0xA1, 0xAD)) + list(range(0xAE, 0x100))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256 + n)
            n += 1
    return [chr(c) for c in cs]


def tiny_vocab():
    tokens = gpt2_byte_chars()
    merges = [("h", "e"), ("l", "l"), ("Ġ", "t"), ("he", "ll"), ("hell", "o"),
              ("Ġt", "h"), ("Ġth", "e")]
    tokens += [a + b for a, b in merges]
    types = [1] * len(tokens)
    specials = ["<|endoftext|>", "<|im_start|>", "<|im_end|>"]
    tokens += specials
    types += [3] * len(specials)
    return tokens, types, [f"{a} {b}" for a, b in merges]


def write_tiny_f32(gguf, path, seed):
    rng = np.random.default_rng(seed)
    tokens, types, merges = tiny_vocab()
    n_vocab = len(tokens)
    w = gguf.GGUFWriter(str(path), "qwen2")
    w.add_name("slm-tiny-qwen2-golden")
    w.add_block_count(TINY_BLOCKS)
    w.add_context_length(TINY_CTX)
    w.add_embedding_length(TINY_HIDDEN)
    w.add_feed_forward_length(TINY_FF)
    w.add_head_count(TINY_HEADS)
    w.add_head_count_kv(TINY_HEADS_KV)
    w.add_rope_freq_base(TINY_ROPE_BASE)
    w.add_layer_norm_rms_eps(1e-6)
    w.add_tokenizer_model("gpt2")
    w.add_tokenizer_pre("qwen2")
    w.add_token_list(tokens)
    w.add_token_types(types)
    w.add_token_merges(merges)
    w.add_bos_token_id(tokens.index("<|endoftext|>"))
    w.add_eos_token_id(tokens.index("<|im_end|>"))
    w.add_pad_token_id(tokens.index("<|endoftext|>"))
    w.add_add_bos_token(False)

    def tensor(name, shape, sigma, mean=0.0):
        w.add_tensor(name, rng.normal(mean, sigma, shape).astype(np.float32))

    head_dim = TINY_HIDDEN // TINY_HEADS
    kv_total = TINY_HEADS_KV * head_dim
    # numpy shape is (rows, cols); GGUF records it as [cols, rows].
    tensor("token_embd.weight", (n_vocab, TINY_HIDDEN), 0.5)
    tensor("output.weight", (n_vocab, TINY_HIDDEN), 0.3)
    tensor("output_norm.weight", (TINY_HIDDEN,), 0.1, 1.0)
    for i in range(TINY_BLOCKS):
        p = f"blk.{i}."
        tensor(p + "attn_norm.weight", (TINY_HIDDEN,), 0.1, 1.0)
        tensor(p + "attn_q.weight", (TINY_HIDDEN, TINY_HIDDEN), 0.2)
        tensor(p + "attn_q.bias", (TINY_HIDDEN,), 0.1)
        tensor(p + "attn_k.weight", (kv_total, TINY_HIDDEN), 0.2)
        tensor(p + "attn_k.bias", (kv_total,), 0.1)
        tensor(p + "attn_v.weight", (kv_total, TINY_HIDDEN), 0.2)
        tensor(p + "attn_v.bias", (kv_total,), 0.1)
        tensor(p + "attn_output.weight", (TINY_HIDDEN, TINY_HIDDEN), 0.1)
        tensor(p + "ffn_norm.weight", (TINY_HIDDEN,), 0.1, 1.0)
        tensor(p + "ffn_gate.weight", (TINY_FF, TINY_HIDDEN), 0.1)
        tensor(p + "ffn_up.weight", (TINY_FF, TINY_HIDDEN), 0.1)
        tensor(p + "ffn_down.weight", (TINY_HIDDEN, TINY_FF), 0.1)
    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()


def gen_tiny(gguf, llama, driver, tmp):
    """Quantize with the real tool, so the file has the same K_M layout real
    models have, and take the first seed whose greedy run is a useful test."""
    quant = llama / "build/bin/llama-quantize"
    prompt = first = None
    for seed in range(1, 5000):
        f32_path = Path(tmp) / "tiny_f32.gguf"
        q_path = Path(tmp) / "tiny_q.gguf"
        write_tiny_f32(gguf, f32_path, seed)
        run(quant, f32_path, q_path, "Q4_K_M", "1",
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if prompt is None:
            prompt = llama_tokenize(llama, q_path, TINY_PROMPT, False, tmp)
            first = llama_tokenize(llama, q_path, TINY_FIRST_PROMPT, False, tmp)
        out_path = Path(tmp) / "greedy.rec"
        run(driver, "greedy", q_path, ",".join(map(str, prompt)), TINY_GENERATE, out_path)
        rec = read_records(out_path)
        toks = np.frombuffer(rec["greedy.tokens"], dtype="<i4")
        margins = np.frombuffer(rec["greedy.margins"], dtype="<f4")
        # Token 2 is excluded as a leftover from rust_slm_prompt stopping on it
        # (see PROVENANCE.md), and a run stuck on one token would prove little.
        if 2 in toks or len(set(toks.tolist())) < 5 or margins.min() < TINY_MIN_MARGIN:
            continue
        first_path = Path(tmp) / "first.rec"
        run(driver, "greedy", q_path, ",".join(map(str, first)), 1, first_path)
        first_rec = read_records(first_path)
        first_toks = np.frombuffer(first_rec["greedy.tokens"], dtype="<i4")
        first_margins = np.frombuffer(first_rec["greedy.margins"], dtype="<f4")
        if 2 in first_toks or first_margins.min() < TINY_MIN_MARGIN:
            continue
        (OUT / "tiny_qwen2.gguf").write_bytes(q_path.read_bytes())
        records = {
            "prompt.text": TINY_PROMPT.encode(),
            "prompt.ids": np.asarray(prompt, dtype="<i4").tobytes(),
            "first.text": TINY_FIRST_PROMPT.encode(),
            "first.ids": np.asarray(first, dtype="<i4").tobytes(),
            "first.tokens": first_rec["greedy.tokens"],
            "first.logits": first_rec["greedy.logits"],
            **rec,
        }
        write_records(OUT / "tiny_qwen2_greedy.rec", records)
        return seed, toks.tolist(), float(margins.min()), prompt, first
    raise SystemExit("no seed met the greedy criteria")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def llama_commit(llama, given):
    """The llama.cpp commit: `git rev-parse` in a clone, else `--llama-commit`
    (a GitHub source archive has no .git; its pax header names the commit)."""
    r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=llama,
                       capture_output=True, text=True)
    if r.returncode == 0:
        sha = r.stdout.strip()
        if given and given != sha:
            raise SystemExit(f"--llama-commit {given} but the checkout is at {sha}")
        return sha
    if not given:
        raise SystemExit(f"{llama} is not a git checkout: pass --llama-commit")
    return given


def pinned_commit():
    for line in (OUT / "PROVENANCE.md").read_text().splitlines():
        if line.startswith("- llama.cpp commit:"):
            return line.split("`")[1]
    raise SystemExit("no llama.cpp commit in PROVENANCE.md")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--llama-cpp", required=True, type=Path)
    ap.add_argument("--model", type=Path)
    ap.add_argument("--llama-commit", help="commit of a --llama-cpp tree that is not a git clone")
    ap.add_argument("--only", choices=["qkv-bias", "real-model"],
                    help="add one fixture at the commit PROVENANCE.md pins, leaving the others alone")
    args = ap.parse_args()
    llama = args.llama_cpp.resolve()
    sys.path.insert(0, str(llama / "gguf-py"))
    import gguf

    OUT.mkdir(parents=True, exist_ok=True)
    sha = llama_commit(llama, args.llama_commit)
    if args.only:
        # A new vector must come from the same reference as the rest.
        if sha != pinned_commit():
            raise SystemExit(f"llama.cpp {sha}, but PROVENANCE.md pins {pinned_commit()}")
        with tempfile.TemporaryDirectory() as tmp:
            driver = build_driver(llama, Path(tmp))
            if args.only == "qkv-bias":
                gen_qkv_bias(driver, tmp)
                name = "golden_qkv_bias.rec"
            else:
                if args.model is None:
                    raise SystemExit("--only real-model needs --model")
                for case in gen_real(llama, driver, args.model.resolve(), tmp):
                    print("real model case %s: %r, %d ids, tokens %s, min margin %.3f" % case)
                name = "qwen25_real_golden.rec"
        model_sha, model_name, seed, toks, margin = provenance_facts()
        write_provenance(sha, model_sha, model_name, seed, toks, margin)
        print(f"llama.cpp {sha}; wrote {name} ({(OUT / name).stat().st_size} bytes)")
        return
    if args.model is None:
        raise SystemExit("--model is required for a full regeneration")
    with tempfile.TemporaryDirectory() as tmp:
        driver = build_driver(llama, Path(tmp))
        vocab_path = OUT / "qwen_vocab.gguf"
        extract_vocab(gguf, args.model, vocab_path)
        gen_tokenizer(llama, vocab_path, args.model, tmp)
        gen_ops(driver, tmp)
        gen_qkv_bias(driver, tmp)
        seed, toks, margin, prompt, first = gen_tiny(gguf, llama, driver, tmp)
        gen_real(llama, driver, args.model.resolve(), tmp)
    print(f"llama.cpp {sha}; tiny seed {seed}; prompt {prompt}; first {first}; "
          f"tokens {toks}; min margin {margin:.3f}")
    write_provenance(sha, sha256(args.model), args.model.name, seed, toks, margin)


def provenance_facts():
    """What `write_provenance` needs, read back from the current PROVENANCE.md,
    for an `--only` run that leaves the other fixtures alone."""
    import re
    text = (OUT / "PROVENANCE.md").read_text()
    model = re.search(r"Vocabulary source: `(.+?)`, sha256 `([0-9a-f]{64})`", text)
    seed = re.search(r"default_rng\((\d+)\)` weights", text)
    margin = re.search(r"\(([\d.]+) here\)", text)
    toks = re.search(r"Greedy tokens: `(\[[\d, ]*\])`", text)
    if not (model and seed and margin and toks):
        raise SystemExit("PROVENANCE.md is not in the generator's format")
    return (model.group(2), model.group(1), int(seed.group(1)),
            [int(t) for t in toks.group(1).strip("[]").split(",")], float(margin.group(1)))


def real_provenance():
    path = OUT / "qwen25_real_golden.rec"
    if not path.exists():
        return ""
    r = read_records(path)
    layer = int(np.frombuffer(r["real.mutate.layer"], dtype="<u4")[0])
    lines = []
    for name in r["real.cases"].decode().split(","):
        text = r[f"real.{name}.prompt.text"].decode()
        ids = np.frombuffer(r[f"real.{name}.prompt.ids"], dtype="<u4").tolist()
        toks = np.frombuffer(r[f"real.{name}.tokens"], dtype="<u4").tolist()
        margins = np.frombuffer(r[f"real.{name}.margins"], dtype="<f4")
        source = next((v for n, _, _, cands in REAL_CASES if n == name
                       for k, v in cands if k == "file" and (REPO / v).read_text() == text), None)
        prompt = f"`{source}` (chat template, special tokens parsed)" if source else f"`{text}` (raw)"
        lines.append(f"  - `{name}`: prompt {prompt}, {len(ids)} ids; llama.cpp's tokens "
                     f"`{toks}`, smallest top-1 margin {margins.min():.3f}.")
    cases = "\n".join(lines)
    return f"""- Real model (`qwen25_real_golden.rec`, #1165), the vocabulary source's
  full GGUF. Per case, llama.cpp's greedy run (plain argmax, CPU) with a
  top-1 margin of at least {REAL_MIN_MARGIN} at every step, and for each of those
  steps the full row of logits from the exact CPU reference, the GPU
  program's own reference with exact dot products, run on the host one
  token at a time by `golden_real_model_write_fixture`. Rows are i16 with a
  per-row offset and scale (stored error at most 0.001). `slm golden`
  compares the GPU program with them on a board
  (`docs/deploy/jetson-uefi-direct.md`); its `drop-bias` mutation zeroes
  layer {layer}'s Q, K and V biases.
{cases}
"""


def write_provenance(sha, model_sha, model_name, seed, toks, margin):
    sizes = "\n".join(
        f"| `{p.name}` | {p.stat().st_size} |"
        for p in sorted(OUT.iterdir()) if p.name != "PROVENANCE.md")
    real = real_provenance()
    (OUT / "PROVENANCE.md").write_text(f"""# SLM golden fixtures

Generated by `scripts/tools/slm-gen-goldens.py`; do not edit by hand. The
reference is upstream llama.cpp and ggml, CPU build, never SLM-OS code. The
one exception is the real model's exact CPU reference (below): ADR 0004 sets
SLM-OS's own exact computation as the tight reference, next to llama.cpp's
top token.

- llama.cpp commit: `{sha}`
- Vocabulary source: `{model_name}`, sha256 `{model_sha}`. Qwen2.5 is
  Apache-2.0 (see `LICENSES.md`). Only tokenizer metadata is kept here.
- Tiny model: numpy `default_rng({seed})` weights, quantized by
  `llama-quantize` to Q4_K_M, not a trained model. The seed is the first with
  at least 5 distinct greedy tokens, no token 2 and a top-1 margin of at least
  {TINY_MIN_MARGIN} at every step ({margin:.3f} here). Greedy tokens: `{toks}`.
- Q/K/V bias vector (`golden_qkv_bias.rec`, #1165): numpy `default_rng(1165)`
  inputs shaped like one Qwen2 attention block (hidden {QKV_HIDDEN}; Q 512 rows,
  K and V 128), ggml's `ggml_add(ggml_mul_mat(W, x), b)` per projection, as
  llama.cpp's Qwen2 graph: `q`, `k` and `v4` in Q4_K, `v6` in Q6_K, with an
  exact dot and per-block sums of |w| for Q6_K.
{real}
Token 2 is excluded from the greedy run, a leftover from when `rust_slm_prompt`
hardcoded it as EOS. `golden_stop_ids_are_the_real_models_not_token_2` and
`golden_ffi_runs_past_token_2_and_stops_at_gguf_eos` cover that bug (#1158).

| File | Bytes |
|---|---|
{sizes}

`*.rec` layout (little endian), read by `runtime/src/slm/golden.rs`: `SGR1`,
u32 record count, then per record u32 name length, name, u32 data length,
data, padding to a 4-byte boundary. `tokenizer_goldens.tsv` has one case per
line: mode, name, input (hex UTF-8, or `role=hex;...` for chat), token ids.
""")


if __name__ == "__main__":
    main()
