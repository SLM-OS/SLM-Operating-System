#!/usr/bin/env python3
"""Write the small GGUF that `make test-qemu-uefi` puts on the virtual disk.

Mirrors the fixture runtime/src/slm/registry.rs builds for its own tests
(build_qwen_test_fixture): the metadata validate_for_inference reads, a
tokenizer, two tiny tensors. It parses and registers, reporting
Qwen2.5-1.5B's shape (28 blocks); it has no real weights, so it can't
produce text.

Also the reference for boot_model_hash (kernel/include/boot_models.h):
`gguf-boot-fixture.py --hash FILE` prints a file's hash as hex.

Usage: gguf-boot-fixture.py OUT [--vocab N]
"""
import struct
import sys

ALIGN = 32
GGUF_MAGIC = 0x46554747
GGUF_VERSION = 3
T_U32, T_F32, T_STR, T_ARR = 4, 6, 8, 9
GGML_F32, GGML_Q4_K = 0, 12
Q4_K_BLOCK = 144

M = (1 << 64) - 1
P1 = 0x9E3779B185EBCA87
P2 = 0xC2B2AE3D27D4EB4F


def _rotl(v, r):
    return ((v << r) | (v >> (64 - r))) & M


def boot_model_hash(data):
    lanes = [(P1 + P2) & M, P2, 0, (-P1) & M]
    i = 0
    while len(data) - i >= 32:
        for k, w in enumerate(struct.unpack_from("<4Q", data, i)):
            lanes[k] = (_rotl((lanes[k] + w * P2) & M, 31) * P1) & M
        i += 32
    h = (_rotl(lanes[0], 1) + _rotl(lanes[1], 7) + _rotl(lanes[2], 12) +
         _rotl(lanes[3], 18) + len(data)) & M
    for c in data[i:]:
        h = (_rotl(h ^ ((c * P1) & M), 11) * P2) & M
    h ^= h >> 33
    h = (h * P2) & M
    h ^= h >> 29
    return h


def _s(text):
    raw = text.encode()
    return struct.pack("<Q", len(raw)) + raw


def _kv(key, tag, payload):
    return _s(key) + struct.pack("<I", tag) + payload


def build(vocab):
    tokens = [chr(i) if vocab >= 128 and i < 128 else "t%d" % i
              for i in range(vocab)]
    kvs = [
        _kv("general.alignment", T_U32, struct.pack("<I", ALIGN)),
        _kv("general.architecture", T_STR, _s("qwen2")),
        _kv("qwen2.block_count", T_U32, struct.pack("<I", 28)),
        _kv("qwen2.embedding_length", T_U32, struct.pack("<I", 1536)),
        _kv("qwen2.attention.head_count", T_U32, struct.pack("<I", 12)),
        _kv("qwen2.attention.head_count_kv", T_U32, struct.pack("<I", 2)),
        _kv("qwen2.feed_forward_length", T_U32, struct.pack("<I", 8960)),
        _kv("qwen2.context_length", T_U32, struct.pack("<I", 32768)),
        _kv("qwen2.rope.freq_base", T_F32, struct.pack("<f", 1000000.0)),
        _kv("tokenizer.ggml.tokens", T_ARR,
            struct.pack("<IQ", T_STR, len(tokens)) +
            b"".join(_s(t) for t in tokens)),
        _kv("tokenizer.ggml.merges", T_ARR, struct.pack("<IQ", T_STR, 0)),
    ]
    tensors = [
        ("output_norm.weight", GGML_F32, [4],
         struct.pack("<4f", 1.0, 2.0, 3.0, 4.0)),
        ("token_embd.weight", GGML_Q4_K, [32, 4], bytes(Q4_K_BLOCK)),
    ]
    out = struct.pack("<IIQQ", GGUF_MAGIC, GGUF_VERSION, len(tensors), len(kvs))
    out += b"".join(kvs)
    offset = 0
    for name, ggml_type, dims, data in tensors:
        out += _s(name) + struct.pack("<I", len(dims))
        out += b"".join(struct.pack("<Q", d) for d in dims)
        out += struct.pack("<IQ", ggml_type, offset)
        offset += len(data)
    out += bytes(-len(out) % ALIGN)
    out += b"".join(t[3] for t in tensors)
    return out


def main(argv):
    if len(argv) == 3 and argv[1] == "--hash":
        with open(argv[2], "rb") as f:
            print("0x%016x" % boot_model_hash(f.read()))
        return 0
    if len(argv) not in (2, 4) or (len(argv) == 4 and argv[2] != "--vocab"):
        print(__doc__, file=sys.stderr)
        return 2
    vocab = int(argv[3]) if len(argv) == 4 else 256
    with open(argv[1], "wb") as f:
        f.write(build(vocab))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
