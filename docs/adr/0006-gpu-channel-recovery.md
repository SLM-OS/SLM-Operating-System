# ADR 0006: recover the cold GPU channel after a step timeout

## Context

After a UEFI-direct boot every GPU consumer shares one SLM-OS-built channel: the SLM GPU program, the scheduler MLP, the eviction Q-net, the MNIST pipeline and the operator library's per-op path (#1088, #1092, ADR 0003). Before #1171, a program step that timed out (5 s) left that channel wedged until reboot. Once the program's deadline passed, the next dispatcher was let onto the channel. Its work queued behind the hung entry and timed out too, each dispatch polling for 2 s with IRQs masked, until the dispatch breaker tripped. Every consumer then stayed on the CPU, and the session replayed its whole history on the CPU at about 0.4 tok/s. `slm gpu inject hang` (a compute-class semaphore acquire that never completes) and `slm gpu inject fault` (a kernel write to an unmapped address) reproduce both cases on purpose.

## Decision

Recover the channel in place, after nvgpu's `gv11b_fifo_recover`, then carry on:

1. **Gate.** A dispatcher may use the channel only while it is up and no GPU program owns it (`kernel/gpu/gpu_chan_state.c`). A program past its deadline, a recovery in progress, or a recovery that failed turns dispatchers away to their CPU path (`SLM_GPU_ERR_CHANNEL_DOWN`), without counting toward the breaker.
2. **Recovery** runs in the task that saw the timeout, with IRQs on and no spinlock held, and every wait is bounded (`ga10b_cold_recover`):
   - Level 1, channel reset. Disable the channel and the runlist scheduler, then preempt the TSG (100 ms at most). Always tried first. A latched error is recorded but does not skip it: `nvgpu cold check` leaves an MMU fault latched on a healthy channel.
   - Level 2, GR engine reset, when level 1 fails. Runlist preempt until the channel is off the PBDMA, then the boot steps that make up `nvgpu_gr_reset`: GR reset cycle, non-context loads, the LS PMU's FECS/GPCCS bootstrap, the GR init table. Then clear the latched fault and interrupt state.
   - Both levels then put the channel back as it was built: reload its GR context from the golden image, RAMFC as at SETUP_BIND (GP_GET 0, no pending method or acquire), USERD and the GPFIFO cursor at 0, an L2 evict, the runlist resubmitted and the channel enabled. The instance block, VM and every mapping stay, so the handoffs of all consumers stay valid.
   - No forced context reload, from the recovery or from any dispatch on the cold channel. A reload that is still pending when the GR engine is reset leaves the TSG marked as on the engine, and the channel never runs again. The v7 dispatch path forced one on every dispatch (the #844 workaround for the channel inherited after kexec); it now does so only on the inherited channel.
   - A probe (a host semaphore, then a compute one after a GR drain) decides between up and dead.
3. **Session.** A recovered step returns `SLM_PROG_ERR_RECOVERED`. The dead program's memory is freed, since nothing of it can run any more. The decoder builds the program again and replays the session's steps on it, the same rows in the same order, so the new KV cache is identical. The CPU replay remains the fallback.

## Alternatives considered

- Release the hung semaphore from the CPU. Rejected: it only clears the injected acquire. A faulted SM or a kernel that never ends stays stuck.
- Build a new channel for each recovery. Rejected: the stored handoffs of every consumer, the operator library's pools and the GR context buffers are tied to the channel's instance block and VM. Resetting the channel in place keeps all of them.
- Always reset the GR engine. Rejected as the first step: it reloads FECS and GPCCS through the PMU and is the riskier path. Level 1 is tried first. A hang the TSG preempt cannot stop pays the 100 ms preempt wait before level 2, and the measured recovery stays at about 107 ms, well inside the 5 s budget.
- Skip level 1 when an error is latched. Rejected: an MMU fault stays latched after `nvgpu cold check` on a healthy channel, so the flag does not say the channel is broken.
- Re-enable the channel with a forced context reload, as the channel allocation does. Rejected on hardware: with the reload, the first injected hang recovered and the second left the channel DEAD (CHRAM 0xb8a, the TSG still marked as on the engine, an MMU fault on an unbound instance block). An empty runlist before the resubmit, or clearing the latched state first, did not change that (both remain as `nvgpu cold recover opts <n>` bits for experiments). Without any forced reload every recovery succeeded.
- Keep the old behaviour (consumers stay on the CPU until reboot). Rejected: one bad step disabled every GPU consumer for the rest of the boot.

## Consequences

A step timeout costs the 5 s timeout plus about 0.13 s of recovery and replay, after which every consumer is back on the GPU. The first timeout of each boot also prints the full fault state, which costs about 1.5 s more. A recovery that fails leaves the channel DEAD: consumers stay on the CPU without touching it, so nothing wedges, and `nvgpu cold recover` can retry. `slm gpu inject` arms one launch only. Recovery exists only for the cold channel; after kexec the inherited channel keeps the old behaviour (#1169). A GPU consumer's own v7 dispatch timeout does not start a recovery; `nvgpu cold recover` does it by hand.

## Evidence

jetson-nano-2, UEFI-direct boot, 2026-11-08; raw logs in `~/slmos-ref/derivatives/475-followups/1171/` (`h1-round*-serial.txt`, `h2-campaign-serial.txt`).

- One boot, Qwen2.5-1.5B session with `--gpu on`: 20 injected hangs and 5 injected faults, each in a fresh session. 25 of 25 recovered at level 2 (level 1: the TSG preempt timed out). Failure to working channel: 1.594 s the first time (with the full fault dump), then 124.7 to 125.0 ms. Each session rebuilt its program and replayed its 64 steps on the GPU in 134 ms, and every reply was byte-identical to the run without an injection.
- `nvgpu cold race` (sched MLP, eviction Q-net, MNIST, the operator library's RMSNorm) ran through all 25 recoveries: 160830 rounds, 0 wrong results. First correct result after each timeout line: sched 158 ms, eviction 169 to 170 ms, MNIST and RMSNorm 181 ms (1627 to 1650 ms after the first). Calls that arrived while a recovery was running were turned away to the CPU, 448 per consumer. On this image the race counted 198 of the RMSNorm refusals as errors, because the operator library returned -1 for a refusal; it now returns `SLM_GPU_ERR_CHANNEL_DOWN`.
- After the 25 recoveries, p1 (256 tokens, greedy) decoded at 49.507 tok/s with TTFT 75 ms. The text was the same as before the injections. `nvgpu cold check` passed. A level-1 recovery by hand on the idle channel took 570 us.
