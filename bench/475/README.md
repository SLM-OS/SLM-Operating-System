# #475 benchmark

Same GGUF (`qwen2.5-1.5b-instruct-q4_k_m.gguf`, sha256 `6a1a2eb6...e9407e`), same pre-rendered
ChatML prompts of exactly 64 tokens, temperature 0, up to 256 generated tokens.

- `prompts/pN.txt`: raw prompt bytes. No trailing newline beyond the template's.
- `run-llama.sh`: reference run under JetPack on jetson-nano-2. Records nvpmodel mode and GPU clock.
