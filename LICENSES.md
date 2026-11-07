# Third-Party Licenses

This document lists third-party code vendored into SLM-OS along with its
license. Each entry identifies the source file(s) the upstream code lives
in and provides the relevant license text in full.

---

## llama.cpp / ggml — MIT

Portions of `runtime/src/inference/quant.rs` (specifically the Q4_K_M
super-block decoding and Q8_K quantization arithmetic — `dequantize_row_q4_K`,
`quantize_row_q8_K_ref`, and `ggml_vec_dot_q4_K_q8_K_generic`) are derived
from the llama.cpp project, licensed under the MIT License. The algorithms
have been translated from C to `no_std` Rust by SLM-OS contributors.

- Original source: <https://github.com/ggml-org/llama.cpp>
  - `ggml/src/ggml-quants.c` (`dequantize_row_q4_K`, `quantize_row_q8_K_ref`)
  - `ggml/src/ggml-cpu/quants.c` (`ggml_vec_dot_q4_K_q8_K_generic`)
  - `ggml/src/ggml-common.h` (`block_q4_K`, `block_q8_K` struct layouts)
- License: <https://github.com/ggml-org/llama.cpp/blob/master/LICENSE>

```
MIT License

Copyright (c) 2023-2024 The ggml authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## Qwen2.5-1.5B-Instruct tokenizer — Apache-2.0

`scripts/fixtures/slm/qwen_vocab.gguf` holds the tokenizer metadata (tokens,
token types, merges) of Qwen2.5-1.5B-Instruct and no model weights. The golden
tokenizer test reads it, and it is not linked into any kernel. See
`scripts/fixtures/slm/PROVENANCE.md`.

- Original source: <https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct>
- License: Apache License 2.0, <https://www.apache.org/licenses/LICENSE-2.0>

The other fixtures in that directory are outputs of llama.cpp / ggml (MIT, see
above) on inputs SLM-OS generated.
