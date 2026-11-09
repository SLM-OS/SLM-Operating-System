# ADR 0006: recover the cold GPU channel after a step timeout

## Context

After a UEFI-direct boot every GPU consumer shares one SLM-OS-built channel: the SLM GPU program, the scheduler MLP, the eviction Q-net, the MNIST pipeline and the operator library (#1088, #1092, ADR 0003). Before #1171 a program step that timed out left the channel wedged until reboot: later dispatches queued behind the hung entry, timed out with IRQs masked and tripped the dispatch breaker, and every consumer and the session fell back to the CPU (about 0.4 tok/s).

## Decision

Recover the channel in place, after nvgpu's `gv11b_fifo_recover`, then carry on:

1. **Gate.** A dispatcher uses the channel only while it is up and no program owns it. A program past its deadline, a recovery in progress or a failed one sends dispatchers to their CPU path at once, without counting toward the breaker.
2. **Recovery**, in the task that saw the timeout, IRQs on, every wait bounded. Level 1 resets the channel after a TSG preempt; level 2, when level 1 fails, also resets the GR engine as `nvgpu_gr_reset` does. Both reload the golden context, reset RAMFC, USERD and the GPFIFO cursor, evict L2, resubmit the runlist, enable the channel with no forced context reload, and probe it. The instance block, VM and mappings stay, so every consumer's handoff stays valid.
3. **Session.** The decoder rebuilds its program and replays its steps on the GPU in the same order, so the KV cache is identical. The CPU replay is the fallback.

Detail, failure modes and measurements: `docs/design/slm-gpu-program.md`, "Concurrency and failure handling" and "Channel recovery on hardware".

## Alternatives considered

- Release the hung semaphore from the CPU. Rejected: it clears only the injected acquire, not a faulted SM or an endless kernel.
- Build a new channel per recovery. Rejected: every consumer's handoff, the operator library's pools and the context buffers are tied to the channel's instance block and VM.
- Always reset the GR engine first. Rejected: level 1 is cheaper and less risky; a hang it cannot stop costs only its 100 ms preempt wait.
- Skip level 1 when an error is latched. Rejected: an MMU fault stays latched on a healthy channel after `nvgpu cold check`.
- Re-enable with a forced context reload, as channel allocation does. Rejected on hardware: a reload pending at the GR reset left the channel DEAD from the second hang on.
- Keep the old behaviour. Rejected: one bad step disabled every GPU consumer until reboot.

## Consequences

A step timeout costs the 5 s timeout plus about 0.13 s of recovery and replay, and about 1.5 s more on the first timeout of a boot, which prints the full fault state. A failed recovery leaves the channel DEAD: consumers stay on the CPU without touching it, and `nvgpu cold recover` can retry. Recovery exists only for the cold channel; the kexec-inherited channel keeps its old behaviour (#1169). A consumer's own dispatch timeout does not start a recovery.

## Evidence

jetson-nano-2, 2026-11-08, raw logs in `~/slmos-ref/derivatives/475-followups/1171/`: every injected hang and fault recovered, each session's replay was byte-identical, every consumer was correct again 157 to 181 ms after each timeout line, and decode ran at 49.5 tok/s afterwards. One wrong operator-library result in a race with recoveries is unexplained (#1186).
