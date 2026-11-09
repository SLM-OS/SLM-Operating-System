# Hardware gate procedure (#475 follow-ups, jetson-nano-2, UEFI-direct)

Written by the baseline run on main c69438a2 (2026-11-08). Baseline numbers:
`baseline/main-c69438a2.md`. Same procedure as the design doc's table
(`docs/design/slm-gpu-program.md`, "Measurements on SLM-OS").

## 1. Build (one build at a time per worktree)

```
export PATH=/opt/arm-gnu-toolchain/bin:$HOME/.cargo/bin:$PATH
make kernel-clean && make kernel PLATFORM=JETSON_ORIN_NANO 2>&1 | tee build.log
grep 'oplib-shaders: nvcc /usr/bin/nvcc (CUDA 12.0)' build.log   # required; a stub-shader build is invalid
sha256sum build/kernel/slmos.bin
```

## 2. Cold UEFI-direct boot

1. `power_cycle(jetson-nano-2)`; `serial_capture(timeout=150, until_pattern="login:")`.
2. `scripts/jetson-uefi-deploy.sh 192.168.4.5 build/kernel/slmos.bin` (prints `staged sha256=... BootNext=...`).
3. `serial_capture(timeout=150, until_pattern="slmos>|Unhandled Exception|Powering off|ABORT|PANIC|panic")`.
   The capture must show `[INFO] Boot model qwen2.5-1.5b-instruct-q4_k_m.gguf: 1065 MiB ... kernel verify` and `[INFO] EMC: 204 -> 3199 MHz`.

Every `serial_send` takes a confirm_token: call once, then repeat with the token.
Pass `capture_until="slmos>"` (literal `>`) or a narrower pattern given below.
Echo of typed input is garbled on this console; the commands still parse. Judge from the output lines.

## 3. Session (once per boot)

```
clocks                                                       # emc 3199 MHz, gpc0clk 1020 MHz before the runs
slm load /boot/models/qwen2.5-1.5b-instruct-q4_k_m.gguf      # "[slm] zero-copy from boot memory, ~346 ms"
slm launch 0 --ctx 512 --sampler greedy --gpu on             # backend=GPU program(launches=423 barriers=310 batch=64 arena=19MB weights=64KB)
```

## 4. One run (p1, p2, p3, each x 3, greedy)

Print the prompt line with the helper (prompts contain newlines; the shell and `slm prompt`
each unescape once, see the script's docstring):

```
python3 -I /home/john/slmos-ref/derivatives/475-followups/tools/prompt-cmd.py bench/475/prompts/p1.txt
```

Send ONE `serial_send` whose `data` is three lines joined by a newline:
`slm reset 0`, the printed `slm prompt 0 --raw --max 256 "..."` line, `slm gpu profile 0`;
`capture_timeout=40`, `capture_until="last prefill \d+ rows gpu \d+ us"` (about 5.3 s per run).
Save each capture with `tools/save-capture.py <file>` (stdin: the JSON-quoted result string).

Read:
- `[slm] ttft=75 ms  decode=49.508 tok/s  total=... tokens=256 bytes=... stop=max`:
  **TTFT** = `ttft`, **decode** = `decode` (summary line, `kernel/src/slm_shell.c:947`).
- `[slm] decode N steps: ...; last prefill 64 rows gpu 72536 us`:
  **prefill (64 rows)** = `last prefill` us / 1000 (`runtime/src/slm/gpu_program.rs:1241`).
  `decode N steps` and its per-step averages accumulate over the session; only `last prefill` is per run.
  `rows` must read 64 (proves the 64-token prompt arrived intact).
- Reply text = the bytes between the echoed line ending `assistant\n"` and `\n[slm] ttft=`;
  its length must equal `bytes=`. Compare with `baseline/text-pN.txt` byte for byte (`cmp`).

## 5. After the runs

```
slm stats 0 --gpu      # "clocks   : EMC 3199 MHz, GPC 1020 MHz", prefill/decode/dram bw lines
clocks                 # emc 3199 MHz, gpc0clk 1020 MHz
slm gpu                # "clocks: EMC 3199 MHz, GPC 1020 MHz", session 0: GPU program, cpu_tokens=0
```

## 6. Default sampler (p1 x 3)

`slm close 0`, then `slm launch 0 --ctx 512 --gpu on` (no sampler flags: defaults are
topkp, temp 0.7, top_k 40, top_p 0.9, seed 1, `kernel/src/slm_shell.c:634-641,792-794`;
the banner prints them). Run p1 three times exactly as in section 4. Text differs run to run
(`slm reset` does not reseed); compare decode and TTFT only. Baseline: decode 40.80-40.87 tok/s.

## 7. Finish

`power_cycle`, `serial_capture(until_pattern="login:")`, `actuation_status(jetson-nano-2, recovery_mode)` -> `last_state: open`.

## Known traps

- `slm unload` takes about 57-90 s on this model (shell silent meanwhile, it is not hung); use a
  180 s capture. EMC was found at 204 MHz at launch on one of two boots (#1161); the first
  program open re-requests 3199 (`[slm-prog] EMC was 204 MHz; ... now 3199 MHz`).
