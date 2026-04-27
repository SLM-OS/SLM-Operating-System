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

See `docs/specs/slm-integration.md` for the full spec and
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
memory) and intermediate activations + the KV cache in the **workspace
pool**. Per-platform pool sizes live in `kernel/include/config.h`:

| Platform | Weight pool | Workspace pool |
|----------|-------------|----------------|
| QEMU ARM64 | 256 MB | 128 MB |
| Pi 5 | 512 MB | 256 MB |
| **Jetson Orin Nano** | **2 048 MB** | **256 MB** |
| x86-64 | 256 MB | 128 MB |

Qwen2.5-1.5B-Q4_K_M needs ~1.0 GB for weights and ~230 MB for a 4 K
KV cache, so only the Jetson sizing supports the demo. The QEMU /
Pi 5 / x86-64 defaults are tuned for the smaller Phase-5 ONNX models.

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

### Step 3: Stage onto the Jetson SD card

GGUFs are ~1 GB; they live on the LittleFS partition mounted at
`/mnt/files`, not embedded in the kernel image. Stage via labctl
rather than mounting `/dev/sd*` directly:

```bash
labctl sdwire_to_host --sbc jetson-nano-1
labctl sdwire_cp build/slm-models/qwen2.5-1.5b-instruct-q4_k_m.gguf \
                  /mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf
labctl sdwire_to_dut --sbc jetson-nano-1
labctl power_cycle --sbc jetson-nano-1
```

`docs/lab-operations.md` covers the wider labctl workflow.

### Step 4: Load from the SLM-OS shell

Once the M7 milestone lands, the `slm load` shell verb opens the GGUF
through VFS, parses it, copies weight tensors into the weight pool,
and registers the model:

```
slmos> slm load /mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf
[slm] parsed gguf v3: 339 tensors, q4_K_M, vocab=152064
[slm] loaded handle=0  qwen2  1.54 B params  weights=1014 MB  load=412 ms
```

Until M7 ships, the load path is exercised through the integration
tests in `runtime/src/slm/tests/` and the `host-tools/gguf-inspect` CLI.

For the full launch / prompt flow, see `docs/specs/slm-integration.md`
§"CLI Surface (Shell)". For the spec memory plan, KV-cache sizing,
and tensor-core utilization targets, see the rest of that document.

---

## Verifying End-to-End Once M7 Lands

```
slmos> slm load /mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf
slmos> slm launch 0 --ctx 4096 --gpu auto
slmos> slm prompt 0 "Explain virtual memory in two sentences."
```

A streamed coherent response on UART, followed by `[slm] ttft=… decode=… tok/s`,
indicates the full pipeline (GGUF parse + tokenizer + transformer ops +
GPU kexec-handoff bridge) is working.

---

## See also

- `docs/specs/slm-integration.md` — full spec
- `docs/plans/slm-integration-plan.md` — milestone breakdown
- `docs/tutorials/models.md` — Phase-5 ONNX (vision) path
- `docs/model-memory.md` — pool layout
- `docs/setup.md` §"Jetson SD-Card Layout"

*Last updated: 27 April 2026*
