# Tutorial: Preparing and Loading SLM (GGUF) Models

Guide for fetching, verifying, and staging a Small Language Model
(GGUF format) for SLM-OS, the Jetson Orin Nano demo target.

**Prerequisites:** A running SLM-OS instance on a Jetson Orin Nano
Super Dev Kit. The LittleFS filesystem must be mounted at `/mnt/files`.
For Phase-5 ONNX vision models, see `docs/tutorials/models.md` —
this tutorial covers the *parallel* GGUF path used by the
`slm` shell verb family rather than the existing `model` family.

---

## Overview

SLM-OS includes a Rust GGUF v3 parser, a BBPE tokenizer, INT4 (Q4_K_M)
quantization kernels, and the transformer ops needed to run a
decoder-only Small Language Model end-to-end. The full
demo target is **Qwen2.5-1.5B-Instruct** (Alibaba, Apache-2.0),
quantized to Q4_K_M, with optional fallback to **Llama-3.2-1B-Instruct**
(Meta, Llama-3.2 community license).

See `docs/fact-sheets/slm-integration.md` for the full spec and
`docs/plans/slm-integration-plan.md` for the milestone breakdown.

---

## Supported Model Format

### Requirements

- **Format:** GGUF v3 (the format used by llama.cpp and the wider GGML
  ecosystem).
- **Quantization:** Q4_K_M (4-bit weights with super-block scales and
  mins). Optional support for Q4_0 may be added during bring-up.
- **Architecture:** decoder-only transformer with the layer pieces SLM-OS
  implements — RMSNorm, RoPE, GroupedQueryAttention, SwiGLU, LMHead,
  embedding lookup. Qwen2 and Llama families are explicit goals.
- **Tokenizer:** byte-level BPE shipped inside the GGUF metadata
  (vocab + merges). No external tokenizer files are needed.

### Registered models

`scripts/fetch-slm.sh --list` enumerates the model registry. Today:

| Name | Size | License | Source |
|------|------|---------|--------|
| `qwen2.5-1.5b-instruct-q4_k_m` | ~1.0 GB | Apache-2.0 | Qwen/Qwen2.5-1.5B-Instruct-GGUF |
| `llama-3.2-1b-instruct-q4_k_m` | ~0.8 GB | Llama-3.2 community | bartowski/Llama-3.2-1B-Instruct-GGUF |

Architectures outside the LLaMA-family decoder-only shape (encoder-only,
encoder-decoder, mixture-of-experts, vision-language) are not supported.

---

## Size Constraints

The Rust runtime stores weights in the **weight pool** (Phase-3 model
memory) and intermediate activations in the **workspace pool**. The
M5 milestone will carve a per-session **KV-cache sub-pool** (~230 MB
per Qwen2.5-1.5B session at 4 K context, sized for two concurrent
sessions = ~512 MB) out of the workspace pool — until M5 lands the
KV cache is just a workspace allocation, not a separate pool.

Per-platform pool sizes are defined in `kernel/include/config.h`
(M0.2) as `MODEL_MEM_WEIGHT_MB` / `MODEL_MEM_WORKSPACE_MB` and
passed into `rust_model_mem_init` from the kernel boot path:

| Platform | Weight pool | Workspace pool | KV-cache sub-pool (M5) |
|----------|-------------|----------------|------------------------|
| QEMU ARM64 | 256 MB | 128 MB | n/a (no SLM workload) |
| Pi 5 | 512 MB | 256 MB | (unsized; Pi 5 hosts vision models, not SLMs) |
| **Jetson Orin Nano** | **2 048 MB** | **256 MB** | **~512 MB** (carved from workspace) |
| x86-64 | 256 MB | 128 MB | n/a |

Qwen2.5-1.5B-Q4_K_M needs ~1.0 GB for weights and ~230 MB for a
4 K KV cache, so only the Jetson sizing supports the full demo.
The QEMU / Pi 5 / x86-64 defaults are tuned for the smaller Phase-5
ONNX models. The KV-cache sub-pool overlaps the workspace allocator
arithmetically (≥ 256 MB for KV against a 256 MB workspace pool) —
the spec resolves this in `docs/fact-sheets/slm-integration.md` "Memory
Plan" by carving the KV pool out of an extended workspace allocator
when M5 lands; the `MODEL_MEM_WORKSPACE_MB` constant will grow at
that point if needed.

Pool utilization is visible via the existing `model pools` shell
command after `slm load` succeeds.

---

## How to Stage a GGUF

### Step 1: Fetch and verify on the dev host

`scripts/fetch-slm.sh` is the canonical entry point. It downloads from
Hugging Face, computes the SHA256 of the result, and refuses to declare
success until the hash matches a pinned entry in the registry:

```bash
# default: Qwen2.5-1.5B-Q4_K_M into ./build/slm-models/
scripts/fetch-slm.sh

# explicit registered model
scripts/fetch-slm.sh --model llama-3.2-1b-instruct-q4_k_m

# verify a previously-downloaded copy without re-downloading
scripts/fetch-slm.sh --verify-only

# print the on-disk SHA256 (used to bootstrap a TBD-PIN entry)
scripts/fetch-slm.sh --print-sha
```

If the registry's SHA256 entry is still `TBD-PIN-AFTER-FIRST-DOWNLOAD`,
the script will refuse the verify step and print the actual hash. Pin
that hash into `MODEL_SHA256[<name>]` in the script and commit before
relying on the file for a demo run — this is the gate that prevents a
silent upstream swap from being accepted.

### Step 2: Inspect the file

The host CLI `host-tools/gguf-inspect` parses GGUF v3 and prints the
header, KV metadata, and tensor list. Useful for confirming the file
matches the architecture SLM-OS expects:

```bash
cargo run --manifest-path host-tools/gguf-inspect/Cargo.toml -- \
    build/slm-models/qwen2.5-1.5b-instruct-q4_k_m.gguf
```

For a Qwen2.5-1.5B-Instruct GGUF the output should report
`general.architecture = qwen2`, `qwen2.block_count = 28`,
`qwen2.embedding_length = 1536`, `qwen2.attention.head_count = 12`,
`qwen2.attention.head_count_kv = 2`, vocab size 152 064.

### Step 3: Get the GGUF onto the running SLM-OS

GGUFs are ~1 GB. On Jetson the recommended path is `slm xload`:
stream the GGUF from the dev host straight into a single PMM
buffer over the telnet shell, no SD-card / LittleFS round-trip.
The Jetson's 8 GB system can produce only one order-19 (2 GB)
buddy block at a time, and the LittleFS-based `slm load` path
needs two (one to hold the file, one for the registry copy) — so
SD-card staging isn't viable for Qwen-class models on this
platform.

```bash
# On the dev host, after `scripts/fetch-slm.sh`. Substitute your
# Jetson's IP for 192.168.4.100 (the one labctl reports for
# `jetson-nano-1` in the SLM-OS lab).
python3 scripts/tools/slm-xload.py 192.168.4.100 qwen \
    build/slm-models/qwen2.5-1.5b-instruct-q4_k_m.gguf
```

`slm-xload.py` wraps the `slm xload <name> <total>` shell verb over
the existing telnet binary protocol (same wire format as `xput-bin`).
Peak kernel memory == file size; no second copy.

For platforms where the SD card / LittleFS path fits the GGUF
(smaller models, or a future multi-block PMM allocator), the
`/mnt/files`-staged `slm load <path>` workflow still works and
labctl is still the only sanctioned SD-card interface.

### Step 4: Load from the SLM-OS shell

After `slm xload` completes the model is already registered;
`slm list` shows it and `slm launch <handle>` opens a session:

```
slmos> slm xload qwen <total_bytes>
SLM-XLOAD ready name=qwen total=<total_bytes>
... (1 GB streamed) ...
SLM-XLOAD done received=<total_bytes>
[slm] loaded handle=0  arch=qwen2  blocks=28 hidden=1536
      head=12/2 head_dim=128 vocab=152064 ctx=32768 source=1014 MB
```

For platforms that can fit `slm load`'s alloc+copy footprint, the
file-path variant produces the same result:

```
slmos> slm load /mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf
[slm] parsed gguf v3: 339 tensors, q4_K_M, vocab=152064
[slm] loaded handle=0  qwen2  1.54 B params  weights=1014 MB  load=412 ms
```

For the full launch / prompt flow, see `docs/fact-sheets/slm-integration.md`
§"CLI Surface (Shell)". For the spec memory plan, KV-cache sizing,
and tensor-core utilization targets, see the rest of that document.

---

## Verifying End-to-End

```
slmos> slm xload qwen <total_bytes>           # Jetson — streams over telnet
slmos> slm launch 0 --ctx 4096 --gpu auto
slmos> slm prompt 0 "Explain virtual memory in two sentences."
```

A streamed coherent response on UART, followed by `[slm] ttft=… decode=… tok/s`,
indicates the full pipeline (GGUF parse + tokenizer + transformer ops +
GPU kexec-handoff bridge) is working.

---

## See also

- `docs/fact-sheets/slm-integration.md` — full spec
- `docs/plans/slm-integration-plan.md` — milestone breakdown
- `docs/tutorials/models.md` — Phase-5 ONNX (vision) path
- `docs/model-memory.md` — pool layout
- `docs/setup.md` §"Jetson SD-Card Layout"

*Last updated: 27 April 2026*
