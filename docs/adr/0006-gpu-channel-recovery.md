# ADR 0006: recover the cold GPU channel after a step timeout

## Context

After a UEFI-direct boot every GPU consumer shares one SLM-OS-built channel: the SLM GPU program, the scheduler MLP, the eviction Q-net, the MNIST pipeline and the operator library's per-op path (#1088, #1092, ADR 0003). Before #1171, a program step that timed out (5 s) left that channel wedged until reboot. Once the program's deadline passed, the next dispatcher was let onto the channel. Its work queued behind the hung entry and timed out too, each dispatch polling for 2 s with IRQs masked, until the dispatch breaker tripped. Every consumer then stayed on the CPU, and the session replayed its whole history on the CPU at about 0.4 tok/s. `slm gpu inject hang` (a compute-class semaphore acquire that never completes) and `slm gpu inject fault` (a kernel write to an unmapped address) reproduce both cases on purpose.

## Decision

Recover the channel in place, after nvgpu's `gv11b_fifo_recover`, then carry on:

1. **Gate.** A dispatcher may use the channel only while it is up and no GPU program owns it (`kernel/gpu/gpu_chan_state.c`). A program past its deadline, a recovery in progress, or a recovery that failed turns dispatchers away to their CPU path (`SLM_GPU_ERR_CHANNEL_DOWN`), without counting toward the breaker.
2. **Recovery** runs in the task that saw the timeout, with IRQs on and no spinlock held, and every wait is bounded (`ga10b_cold_recover`):
   - Level 1, channel reset. Disable the channel and the runlist scheduler, then preempt the TSG. Skipped when an error is latched (an MMU fault, ENG_FAULTED, a GR exception).
   - Level 2, GR engine reset, when level 1 fails or is skipped. Runlist preempt until the channel is off the PBDMA, then the boot steps that make up `nvgpu_gr_reset`: GR reset cycle, non-context loads, the LS PMU's FECS/GPCCS bootstrap, the GR init table. Then clear the latched fault and interrupt state.
   - Both levels then put the channel back as it was built: reload its GR context from the golden image, RAMFC as at SETUP_BIND (GP_GET 0, no pending method or acquire), USERD and the GPFIFO cursor at 0, an L2 evict, the runlist resubmitted and the channel enabled with a forced context reload. The instance block, VM and every mapping stay, so the handoffs of all consumers stay valid.
   - A probe (a host semaphore, then a compute one after a GR drain) decides between up and dead.
3. **Session.** A recovered step returns `SLM_PROG_ERR_RECOVERED`. The dead program's memory is freed, since nothing of it can run any more. The decoder builds the program again and replays the session's steps on it, the same rows in the same order, so the new KV cache is identical. The CPU replay remains the fallback.

## Alternatives considered

- Release the hung semaphore from the CPU. Rejected: it only clears the injected acquire. A faulted SM or a kernel that never ends stays stuck.
- Build a new channel for each recovery. Rejected: the stored handoffs of every consumer, the operator library's pools and the GR context buffers are tied to the channel's instance block and VM. Resetting the channel in place keeps all of them.
- Always reset the GR engine. Rejected as the first step: it reloads FECS and GPCCS through the PMU and is the slower, riskier path. Level 1 is tried first when nothing is latched.
- Keep the old behaviour (consumers stay on the CPU until reboot). Rejected: one bad step disabled every GPU consumer for the rest of the boot.

## Consequences

A step timeout costs the 5 s timeout plus the recovery, after which every consumer is back on the GPU. A recovery that fails leaves the channel DEAD: consumers stay on the CPU without touching it, so nothing wedges, and `nvgpu cold recover` can retry. `slm gpu inject` arms one launch only. Recovery exists only for the cold channel; after kexec the inherited channel keeps the old behaviour (#1169). A GPU consumer's own v7 dispatch timeout does not start a recovery; `nvgpu cold recover` does it by hand.

## Evidence

TBD after the hardware runs (#1171).
