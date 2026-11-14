# Architecture Decision Records

Each ADR records one decision: the situation that forced it, what was
decided, what else was considered, and what follows from it. ADRs 0001 to
0007 were written when the decision was made (from 2026-11-07 on). ADRs
0009 to 0017 were backfilled in November 2026 from git history, issues,
PRs and documentation. Points the record did not settle were answered
by the user on 2026-11-14 and are cited as "(user, 2026-11-14)"; where
the answer is that something was not recorded, the ADR says so. The system overview
is `docs/architecture.md`.

## Index

| ADR | Decision | Status |
|---|---|---|
| [0001](0001-llama-cpp-reference-bar.md) | llama.cpp CUDA at the matching clock is the #475 reference bar | accepted |
| [0002](0002-uefi-model-delivery.md) | Deliver models through UEFI file protocols at boot | accepted |
| [0003](0003-slm-gpu-graph.md) | Run the whole SLM forward pass as one GPU command stream | accepted |
| [0004](0004-fp16-activations.md) | FP16 activations and exact dot products in the SLM forward pass | accepted |
| [0005](0005-dod6-decode-bandwidth.md) | Replace the 60 percent tensor-core target with decode bandwidth parity | accepted |
| [0006](0006-gpu-channel-recovery.md) | Recover the cold GPU channel after a step timeout | accepted |
| [0007](0007-cross-cpu-coherency.md) | Cross-CPU state is cacheable on Pi 5 and Jetson (implemented in PR #1225) | accepted |
| 0008 | reserved (gauntlet Brief 5) | reserved |
| [0009](0009-el2-vhe.md) | Run the ARM64 hardware kernels at EL2 with VHE | accepted (backfilled) |
| [0010](0010-kexec-vs-uefi-direct.md) | Keep two Jetson boot paths, kexec from Linux and UEFI-direct | accepted (backfilled) |
| [0011](0011-two-language-lock-model.md) | Separate C and Rust spinlocks; Rust locks reached from IRQs-off paths save IRQs or try-lock | accepted (backfilled) |
| [0012](0012-shell-as-integration-surface.md) | The in-kernel shell is the integration surface for every subsystem | accepted (backfilled) |
| [0013](0013-lua-in-the-kernel.md) | Embed a Lua interpreter in the kernel for scripting | accepted (backfilled) |
| [0014](0014-vendored-lwip-wolfssl.md) | Vendor lwIP for TCP/IP and wolfSSH with wolfCrypt for SSH | accepted (backfilled) |
| [0015](0015-gguf-model-format.md) | GGUF v3 is the SLM model format, checked against llama.cpp goldens | accepted (backfilled) |
| [0016](0016-compiled-c-model-weights.md) | Policy model weights are compiled into the image, runtime blobs override (runtime loading preferred, #1253) | accepted (backfilled), regretted |
| [0017](0017-hailo-path.md) | Drive the Pi 5 Hailo NPU (a Hailo 8) from a native bare-metal driver, without HailoRT | deprecated (#1254) |

## Format

Observed in ADRs 0001 to 0007, and followed by the backfilled ones:

- File name `NNNN-short-slug.md`, four-digit number.
- Title `# ADR NNNN: <the decision, stated as a sentence>`.
- Sections in this order: `## Context`, `## Decision`,
  `## Alternatives considered` (each alternative with the reason it was
  rejected), `## Consequences`, `## Evidence` (hardware, board, date,
  measurements and where the raw logs live).
- A status line where the status is not plain "accepted": ADR 0007 has
  `Status:` inside its Decision section; the backfilled ADRs put
  `Status: accepted (backfilled).` under the title.
- Optional sections when the decision is conditional: ADR 0007 adds
  `## What would overturn this` and dated run sections.
- Short and factual, citing issues, PRs, commits and file paths. No
  second person.
- A later change to a decision amends the ADR in place with a dated note
  (ADR 0004, commit `27a9d719`, #1182) or supersedes it with a new ADR
  that links back.

## Numbering rule

**Reserve the ADR number in the tracking issue when work starts.** Write
"ADR NNNN reserved" in the issue before drafting, and take the next
number not already reserved in an open issue or present in this
directory.

Reason: on 2026-11-08 two runs both claimed 0006. The #475 follow-up work
(#1171) added `0006-gpu-channel-recovery.md` (commit `bdd55191`,
2026-11-07 23:45 PT), while the coherency gauntlet (#1193) had been told
to write 0006 and its removal branch pointed its docs at
`docs/adr/0006-cross-cpu-coherency.md` (commit `d1f12754`, 2026-11-08).
The coherency ADR was renumbered to 0007 afterwards (commit `5d70fdbd`,
"Coherency ADR is 0007; point references at it", 2026-11-09). 0008 is
reserved this way for gauntlet Brief 5.
