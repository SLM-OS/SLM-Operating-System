# SLM GPU program: design and evidence

The decision and its reasons are in [ADR 0003](../adr/0003-slm-gpu-graph.md).
This page holds how the program is built, how it is checked, and the
measurements behind the ADR. Shell verbs are listed in
[the UEFI-direct guide](../deploy/jetson-uefi-direct.md).

## How a program is built

- `slm launch` builds the plan in Rust (`runtime/src/slm/gpu_program.rs`).
  `kernel/gpu/slm_prog.c` encodes each op for its kernel, and
  `kernel/gpu/nvidia/ga10b_prog.c` writes the pushbuffer, QMDs and cbufs.
- Weights are the GGUF tensor bytes mapped into the cold channel's VM on
  64 KB PTEs, without a copy. Weights and activations are cacheable; QMDs,
  cbufs, the pushbuffer, semaphores and the step page are volatile. One L2
  evict per step makes the logits visible to the CPU.
- The session arena holds activations, the KV cache, QMDs, cbufs and
  pushbuffers: 19 MB at `--ctx 512`.
- The program has 423 launches and 310 barriers. Barriers are
  WAIT_FOR_IDLE, with the QMD invalidating shader data. A program ends with
  WAIT_FOR_IDLE and a flush before its semaphore. The semaphore release
  alone fired about 1 us after launching a kernel that was still running,
  which returned half-computed logits.
- The embedding lookup runs on the GPU. KV append needs no op: the K and V
  projections write the position's cache slot. Kernels read positions and
  token ids from the step page, so a decode step patches 84 cbufs.
- The kernel ABI comes from `scripts/cuda/slm/MANIFEST.json`.
  `scripts/cuda/gen-slm-kernel-table.py` checks the parameter offsets and
  generates the table the encoder uses.

### Kernel selection

| Op | Decode (1 row) | Prefill, up to 8 rows | Prefill, 9 to 64 rows |
|---|---|---|---|
| Q4_K / Q6_K matmul, N above 256 | `_b1` | `_b8` | `_bs` (x in shared memory, grid ceil(N/120), block 32x8) |
| Q4_K / Q6_K matmul, N up to 256 | `_b1` | `_b8` | `_b8` split per x tile of 8 rows (grid y = ceil(B/8)) |
| RMSNorm, attention | `_w8` (block 32x8) | `_w8` | `_w8` |

The `_bs` kernels use 104 (Q4_K) and 118 (Q6_K) registers and 16 KB of
shared memory. x must be 16-byte aligned. Shaders come from
`make oplib-shaders` with host CUDA 12.0.

### Concurrency and failure

- A dispatcher that finds a program in flight releases the dispatch lock
  until it ends; sched-MLP's try-lock takes its CPU fallback. Any CPU can
  retire the program from its end semaphore, so a waiter with IRQs masked
  cannot deadlock (#1093 class).
- A step times out after 5 s. The timeout dumps GR and MMU fault state, the
  channel is recovered in place (#1171, ADR 0006), and the session builds its
  program again and replays its steps on it, the same rows in the same order,
  so the KV cache and the output are what they would have been. Only if the
  recovery or the rebuild fails does it replay its tokens on the CPU.
- After kexec there is no cold channel: `--gpu on` is refused and
  `--gpu auto` keeps the per-op path.

## Measurements on SLM-OS

jetson-nano-2, cold UEFI-direct boot, model from `/boot/models`, GPC
1020 MHz, EMC 3199 MHz (read by `slm gpu` after the runs). C3K kernels at
78105b51, built with CUDA 12.0. Each run: `slm reset 0`, then
`slm prompt 0 --raw --max 256`, then `slm gpu profile 0`.

| Run | Decode (tok/s) | First token (ms) | Prefill, 64 rows, GPU (ms) |
|---|---|---|---|
| p1 r1 | 49.528 | 75 | 72.67 |
| p1 r2 | 49.517 | 75 | 72.51 |
| p1 r3 | 49.513 | 75 | 72.55 |
| p2 r1 | 49.526 | 75 | 72.49 |
| p2 r2 | 49.528 | 75 | 72.60 |
| p2 r3 | 49.531 | 75 | 72.54 |
| p3 r1 | 49.518 | 75 | 72.53 |
| p3 r2 | 49.523 | 75 | 72.44 |
| p3 r3 | 49.519 | 75 | 72.52 |

The bar from ADR 0001 is 43.0 tok/s decode and 74 ms for a 63-token
prefill. Both are met. A decode step takes 19.82 ms: 19.32 ms on the GPU,
52 us of L2 evict. The previous build (C3K kernels at 97d69f43, split
`_b64` for the small-N matmuls) gave 49.42 to 49.44 tok/s, 88.5 ms of
prefill and 91 to 92 ms to the first token.

Output is coherent and identical across the three runs of each prompt. It
departs from llama.cpp's output at p1 "page tables. The" (hardware against
page), p2 "She had" (always against never) and p3 "repeatedly" (attempt
against try), the same points as with the previous kernels. On the earlier
kernels at p1, SLM-OS's CPU path printed the same 64 tokens as the program.

### Correctness checks

- `slm gpu opcheck 0`: every kernel at 1, 2, 9, 63 and 64 rows on the real
  model's tensors against an exact FP64 dot product over dequantized
  weights. 0 failed; the largest error is 7 milli (`mm_down`, 63 and 64
  rows). FP16 subnormals are honoured. The `vs q8k` lines compare against
  llama.cpp-style Q8_K activations and are informational.
- `slm gpu golden` on the tiny model: same top token on 10 of 10 steps, at
  most 1.423 from llama.cpp (limit 1.5) and 0.021 from the exact CPU
  reference (limit 0.1).
- Launch overhead, measured with the earlier kernels: 4.1 us per launch and
  barrier, against CUDA's 1 to 3 us.

### Failure and race tests

- `slm gpu inject hang` and `fault` each time out once at 5 s, print GR
  state (the fault names its VA), replay on the CPU with the same tokens and
  leave the shell working. Consumers on that channel then time out too until
  a reboot.
- `nvgpu cold race 2000` during p1 gave 4000 correct results with no errors
  and unchanged text. Decode dropped to 48.92 tok/s; the slowest consumer
  pair took 87.6 ms (it waited out the prefill).

## Channel recovery on hardware

ADR 0006 records the decision; this is what jetson-nano-2 showed (2026-11-08).

- Level 2 always ran for the injected hang and fault: the TSG preempt cannot stop a compute semaphore acquire or a faulted SM, so level 1 gives up after its 100 ms wait, and the GR engine reset takes about 7 ms more. A level-1 recovery by hand on an idle channel takes about 0.5 ms.
- A forced context reload left pending at the GR reset kept the TSG marked as on the engine (CHRAM 0xb8a, an MMU fault on an unbound instance block): the first hang recovered, the second left the channel DEAD. An empty runlist before the resubmit, or clearing the latched state first, did not help. The recovery's channel reset therefore enables without a reload, and the v7 dispatch path forces one only on the kexec-inherited channel (#844).

Raw logs for what follows are in `~/slmos-ref/derivatives/475-followups/1171/` (MANIFEST.md lists them).

- One boot, Qwen2.5-1.5B session with `--gpu on`: 20 injected hangs and 5 injected faults, each in a fresh session. 25 of 25 recovered at level 2 (level 1: the TSG preempt timed out). Failure to working channel: 1.594 s the first time (with the full fault dump), then 124.7 to 125.0 ms. Each session rebuilt its program and replayed its 64 steps on the GPU in 134 ms, and every reply was byte-identical to the run without an injection.
- `nvgpu cold race` (sched MLP, eviction Q-net, MNIST, the operator library's RMSNorm) ran through all 25 recoveries: 160830 rounds, 0 wrong results. First correct result after each timeout line: sched 158 ms, eviction 169 to 170 ms, MNIST and RMSNorm 181 ms (1627 to 1650 ms after the first). Calls that arrived while a recovery was running were turned away to the CPU, 448 per consumer. On this image the race counted 198 of the RMSNorm refusals as errors, because the operator library returned -1 for a refusal; it now returns `SLM_GPU_ERR_CHANNEL_DOWN`.
- After the 25 recoveries, p1 (256 tokens, greedy) decoded at 49.507 tok/s with TTFT 75 ms. The text was the same as before the injections. `nvgpu cold check` passed. A level-1 recovery by hand on the idle channel took 570 us.
- Gate boot of image 1c392ce5: nine greedy runs of p1 to p3 at 49.416 to 49.528 tok/s, TTFT 75 to 76 ms, prefill 72.4 to 73.1 ms, text byte-identical to main; 10 of 10 cold boots. Then 5 injected hangs with the race running: 5 of 5 recovered, every consumer correct again 157 to 181 ms after each timeout line, and the refusals counted as down. One RMSNorm result out of 18375 rounds was wrong, cause not known yet (#1186); the race now records the first wrong result of each consumer.
- Wrong-result rate of the race's operator-library RMSNorm, per round, two-sided 95% bounds: main without the recovery code 0 in 105,368 [0, 3.5e-5]; the recovery images with recoveries 1 in 305,173 over 51 recoveries (3.3e-6 [8e-8, 1.8e-5]); without recoveries 0 in 81,735. The two cannot be told apart at these counts; the one wrong result is tracked in #1186, and `nvgpu cold race status` records the next one's detail (scratch slots after, a re-run, its epoch).

## Kernels under JetPack (C3K)

Native CUDA 12.6 harness (`scripts/cuda/harness/`), MAXN_SUPER, 1020 MHz,
479 ops in one CUDA graph, real weights, random 63-token prompt.

| | Native harness | Bar |
|---|---|---|
| Decode at position 63 | 22.47 ms, 44.5 tok/s (44.2 with logit readback) | 43.0 tok/s |
| Prefill, 63 tokens | 71.4 ms | 74 ms |

- 147 checks pass on the real model. At B=63 the logits stay within 0.018
  of an exact CPU reference (tolerance 0.1) and the top token matches.
  Every kernel is within 1e-3 of output RMS or 2 FP16 ULP.
- Against llama.cpp on the tiny model the largest logit error is 1.42, with
  the same top token at 10 of 10 steps. CPU variants put this down to
  llama.cpp's Q8_K activations: F32 plus Q8_K gives 0.54, F32 plus an exact
  dot gives 1.41. The remaining 0.5 is not explained. Q8_K differs from an
  exact dot by up to 3.4 percent of RMS on real weights; the GPU kernels
  differ by under 3e-5.

How each step was found:

- Decode matmuls are issue-bound. The stall counters show about one
  instruction per active scheduler cycle, while reads stay under the
  72 GB/s that the tile shape sustains. Weights go into HMMA as FP16
  subnormals, which the tensor core honours, at 30 instructions per mma
  (52 at first).
- Prefill was limited by x missing the L1 and by registers. x sits in shared
  memory across 8 warps, and register use dropped from 132 to 104 so that
  more warps fit.
- Splitting K into ranges kept x in the L1 but was slower overall.
- llama.cpp's mmq uses 26 to 28 instructions per mma with int8 activations.
  These kernels stay FP16; no IMMA is used.

### Tensor-core utilization

- `ncu` counter `sm__pipe_tensor_cycles_active`, percent of peak elapsed:
  decode matmuls 2 to 10 percent, prefill 13 to 23 percent. llama.cpp under
  the same tool: decode 0.2 to 0.9 percent, prefill 1.1 to 14.6 percent.
  Neither reaches the 60 percent in the DoD.
- Mma issue cycles over matmul wall time, from known mma counts and the
  measured 3.44 G mma/s: 7.7 percent.
- MAC share on tensor cores: 99.47 percent. GQA is the only SIMT
  matmul-class op.
- On bare metal these counters are programmed through `tegra-soc-hwpm`, not
  nvgpu. Its `hwpm_dbg_regops` log lists 323 register addresses for one
  `ncu` read but no values, so SLM-OS does not read them yet.

## Greedy text against llama.cpp

Greedy text against llama.cpp CUDA (`run-teacher.sh`, p1 to p3, 256 tokens, llama.cpp's tokens fed back):

| Matmul input | Top-1 agreement | Largest llama.cpp gap where we differ | First free-run divergence |
|---|---|---|---|
| FP16 (as shipped) | 98.8, 98.8, 99.6 percent | 0.091 logits | 50, 83, 154 |
| q8_1, as llama.cpp CUDA | 99.6, 98.4, 98.4 percent | 0.126 logits | 50, 83, 20 |

Every disagreement is a near tie. The FP16 divergences are the ones seen on SLM-OS: "page tables. The" hardware/page, "She had" always/never, "repeatedly" attempt/try. Emulating q8_1 does not track llama.cpp longer, so it is not adopted. On p2, the FP16 harness output does not loop (no sentence more than twice). The q8_1 run loops on "It was a creature that was unlike any she had ever seen before." (8 times), the sentence SLM-OS repeats. So the loop follows from which near-tie branch is taken; it is not a systematic error.

## Concurrency and failure handling

- One lock in `slm_prog.c` covers the global program tables, every write to the cold channel's GPU VM, and each step from submit to the end of its wait. A waiter yields instead of spinning with IRQs masked, and gets `SLM_PROG_ERR_BUSY` after 9 s (the 5 s step timeout, a 3 s recovery budget and 1 s of margin). A close that cannot take the lock is finished by the holder on release, whether the holder is `slm_prog` or another VM writer.
- The GPU VM lock (#1173). `ga10b_vm_reserve` is a bump allocator and `ga10b_vm_map` allocates a table and then writes the entry that points at it, so two writers at once could get the same VA, or one could overwrite an entry the other had just written and lose its mappings (`nvgpu oplib wpool` during `slm launch --gpu on`). The program lock is the VM lock: writers outside `slm_prog` take it through `slm_prog_vm_lock` / `slm_prog_vm_unlock` (`slm_prog.h`), and `slm_prog_vm_lock_stats` counts how often each side found the other holding it.

  | Writer | Where | Lock |
  |---|---|---|
  | `slm launch` / `slm close`: weights, arena and control maps, unmaps, VA reuse | `slm_prog.c`: `open_locked`, `wmap_get`, `map_weights_fresh`, `va_get`, `session_free`, `wmap_put` | program lock (`slm_prog_open` / `_close`) |
  | Operator library cold address space: `nvgpu oplib wpool` (`oplib_wpool_build`), `nvgpu oplib stage` and attach (SASS pool, cbuf, QMD pool) | `oplib_gpu_as.c`: `cold_reserve`, `cold_map_at`, `cold_map` | VM lock around each call; `map` reserves and maps in one hold |
  | Sched-MLP and eviction Q-net pipelines, re-run by `nvgpu cold` after a failed policies step | `ga10b_cold_pipelines_embed.c`: `ga10b_cold_pipeline_build` around `ga10b_cp_build` | VM lock, released before the publish (dispatch lock) |
  | MNIST pipeline | `ga10b_mnist_embed.c`: `ga10b_mnist_cold_install` around `ga10b_mnist_pipeline_build` | VM lock |
  | Software walk (reader) | `oplib_cold.c`: `oplib_cold_lookup` | VM lock, so it never follows a half-written entry |
  | Race driver, test only | `oplib_cold.c`: `oplib_cold_vmrace_round` (`nvgpu oplib vmrace`) | VM lock (through the cold address space, then for the walk and unmap) |
  | Channel VM creation and first maps (GPFIFO, pushbuffer, semaphore, GR context buffers) | `ga10b_chan.c`: `ga10b_chan_create`, `ga10b_grctx.c`: `ctx_prepare` | none: boot step `channel`, before `oplib_cold_attach` (step `policies`) makes any other writer possible; `ga10b_cold_run` runs one bring-up at a time and never repeats a step that succeeded |
  | PMU VM, golden-context VM | `ga10b_cold.c`: `step_pmu`; `ga10b_grctx.c`: golden create | none: separate VMs, boot only |

  Lock order: the program (VM) lock, then the GPU dispatch lock (`submit`, `l2_evict` and the TLB invalidates take it inside). No VM writer holds the dispatch lock, and a taker with IRQs masked, as every dispatch-lock holder is, makes one try and gets BUSY, so there is no ABBA. VM writers outside `slm_prog` never call back into it while they hold the lock (it is not recursive). The fake-backend tests in `kernel/tests/test_slm_prog.c` (`test_vm_*`) run one side inside the other's page-table write, where the lost write happened, and fail with the operator library's lock removed.
- The in-flight flag (`g_chan` in `slm_ffi.c`, `kernel/gpu/gpu_chan_state.c`) has no owner field. One session cannot clear another's because `run_step` holds the `slm_prog` lock from submit to `slm_gpu_prog_finished` or the recovery, so only one program is ever in flight. A program past its deadline turns the other dispatchers away to their CPU path instead of letting them queue behind it (#1171).
- Channel recovery (#1171, ADR 0006). `run_step` calls the backend's `recover` after a timeout or a failed L2 evict, still holding the program lock. `slm_gpu_channel_recover` sets the channel RECOVERING under the dispatch lock (every dispatcher then takes its CPU path), runs `ga10b_cold_recover` with IRQs on and no spinlock held, and sets it UP (new epoch, breaker reset, every kind's warmup armed again) or DEAD. Level 1 preempts the TSG and resets the channel; level 2 (after a failed level 1) preempts the runlist and resets the GR engine as `nvgpu_gr_reset` does. Both reload the channel's GR context from the golden image, put RAMFC, USERD and the GPFIFO cursor back to 0, evict L2, resubmit the runlist, enable the channel without a forced context reload, and probe with a host and a compute semaphore. A forced reload pending at a GR reset keeps the TSG marked as on the engine, so neither the recovery nor the v7 dispatch path on the cold channel forces one (ADR 0006). Recovery writes no VM entry, so holding the program (VM) lock is safe. `nvgpu cold recover` runs it by hand under the same lock. Cache clean and invalidate take no lock, so they never skip. `slm launch` reads and pins the model under the registry lock and opens the program after releasing it.
- After a step timeout or a failed L2 evict that the channel recovery could not undo, closing the session removes the PTEs but deliberately leaks the arena, control pages and VA (one log line), because dirty GPU lines could otherwise land on reused pages. After a successful recovery (`SLM_PROG_ERR_RECOVERED`) nothing of the program can run any more and L2 was evicted, so its memory is freed.
- A GPU program pins its model: `slm unload` returns BUSY while one is mapped.
- A deferred close keeps the model too (#1159). When another holder keeps the program lock past a close's 9 s wait (a step timeout's dump, which is 5 s plus about 270 UART lines, or an operator-library VM writer), `slm_prog_close` marks the session and returns; the holder's unlock removes its PTEs later. The program's Drop has already dropped the registry pin by then, so the pin alone would let `slm unload` free owned weight pages (a VFS copy or `slm xload`) that the GPU VM still maps. Each session records its weights range, and `slm_prog_weights_mapped` (`slm_prog.h`, no lock) reports 1 for any session that maps the range until `session_free` has removed its PTEs and invalidated the TLB (a release store of `open`). The registry's `unload_slm` asks it under the registry lock after the pin check; no new mapping can start then, because a program maps weights only while it holds a pin and pins are taken under that lock. Boot models (`/boot/models`) are borrowed and never freed, but unload refuses for them in the same way. Tests: `test_deferred_close_keeps_weights_mapped` and `test_e2e_unload_waits_for_a_deferred_close` (`kernel/tests/test_slm_prog.c`), `unload_refused_until_a_deferred_close_finishes` (`runtime/src/slm/gpu_program.rs`).
- At build, each tensor's dims, type and byte length are checked against the plan; a mismatch keeps the session on the CPU and says why.
- The q8_1 activation emulation (ADR 0004) is compiled into the matmul kernels only with `-DSLM_MM_XQ_EMULATE` (harness use). Having it in the production kernels cost about 0.7 ms per decode step (47.8 instead of 49.4 tok/s).
- `slm gpu golden` exists only in a build with `SLM_GPU_GOLDEN=ON`.
