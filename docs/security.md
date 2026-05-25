# Security

The security posture of SLM-OS: what's protected, what isn't, and the
primitives the kernel relies on.

This document is the running record across the Phase 3 SSH work
(#199 and sub-tickets #893 / #892 / #891 / #896 / #895). Sections
are added as the corresponding sub-ticket lands.

---

## Crypto-quality randomness

The SLM-OS RNG infrastructure (`kernel/include/rng.h`) is the entropy
source SSH (#199), future host-key generation, password salts, and
session-key derivation build on. It is distinct from the lwIP TCP-ISN
PRNG (`lwip_rand_slm`, `kernel/include/net.h`), which is explicitly
non-cryptographic and used only for TCP initial-sequence numbers and
ephemeral-port selection.

### Source ordering

`rng_init` probes for hardware TRNG support and locks the result in
for the lifetime of the boot:

| Source            | Platforms                                                | Probe                              |
|-------------------|----------------------------------------------------------|------------------------------------|
| `RNDR` (FEAT_RNG) | Cortex-A78AE (Jetson Orin Nano)                          | `ID_AA64ISAR0_EL1[63:60]` non-zero |
| `RDRAND`          | x86-64 (test-pc Skylake and newer)                       | `CPUID.01H:ECX` bit 30             |
| `jitter`          | Cortex-A76 (Pi 5), QEMU TCG, every fallback path         | always available                   |

Cortex-A76 (Pi 5) does not implement FEAT_RNG; the jitter pool is
the only path there. QEMU TCG does not expose RNDR or RDRAND in the
default machine model — the QEMU validation budget for SSH therefore
exercises the jitter path, with the TRNG paths covered by the
per-arch self-tests inside `rng_arch_probe`.

### Jitter pool construction

Pool state lives in `g_pool[SHA256_DIGEST_LEN]` and is updated by
SHA-256 absorption:

```
pool = SHA-256(pool || material)
```

Output bytes come from a SHA-256 squeeze:

```
block = SHA-256(pool || counter); counter += 1
```

The pool itself isn't updated per output block — fresh `CNTPCT_EL0`
delta samples are absorbed every `RNG_JITTER_RESEED_BYTES` (4 KB) to
bound state-compromise impact.

#### Boot-time entropy mix

`rng_init` absorbs, in order:

1. DTB `/chosen/rng-seed` — firmware-supplied, up to 64 B on Pi 5
   and Jetson, 64 B on QEMU.
2. DTB `/chosen/kaslr-seed` — 16 B on Pi 5 / Jetson / QEMU.
3. Eight back-to-back `CNTPCT_EL0` delta samples with a short busy
   wait between reads — picks up the timing-jitter component.
4. An 8-byte boot tag (`"SLMOS/rn"`) so two consecutive identical
   boots still differ in pool state.

The DTB seeds are the strongest entropy source in this mix on
platforms with a firmware that populates `/chosen` (Pi 5's
`bootcode.bin` and Jetson's TF-A both do). The CNTPCT-jitter samples
are the lower-bound source; on QEMU TCG they are quasi-deterministic
but the host scheduler's contribution to the busy-wait loop still
gives meaningful boot-to-boot variation.

### TRNG self-test and degradation

Both architectural sources self-test inside `rng_arch_probe`: four
back-to-back reads must succeed before the source is advertised as
live. RDRAND on Skylake has been observed to return `CF=0` for ten
or more attempts in a row under heavy load — to defend against that,
`rng_get_bytes` retries up to `RNG_TRNG_RETRY_BUDGET` (16) and then
degrades silently to the jitter pool. The `hw_degrade_events`
counter (visible via `rng stats`) records every such degradation so
the operator can correlate during long-running sessions.

### Operator visibility

The `rng` shell command surfaces RNG state:

- `rng status` — source, TRNG state, byte counters
- `rng stats`  — compact one-line counter dump
- `rng selftest` — 4 KB pull + chi-squared sanity check
- `rng read [N]` — hex-dump N bytes (1..1024, default 32)

`rng selftest` is a sanity-bound check (catches "all zero" /
stuck-state failures), not a statistical claim. The full
NIST SP800-90B entropy audit lands with #895 (#199e), run off-target
against several MB of captured output.

### Threat model

The RNG is trusted to:

- Provide uniformly distributed output indistinguishable from random
  to a passive observer, given the assumptions of the chosen source
  (vendor TRNG correctness for RNDR / RDRAND; SHA-256 PRF assumption
  + non-trivial CNTPCT jitter for the fallback).
- Resist forward-prediction across a `RNG_JITTER_RESEED_BYTES`
  boundary by absorbing fresh `CNTPCT_EL0` deltas at every reseed.

It is NOT trusted to:

- Resist a state-compromise attacker that reads `g_pool` directly
  (kernel-mode memory disclosure invalidates everything; the RNG is
  inside the same trust boundary as the rest of the kernel).
- Provide a cryptographically secure floor without a hardware TRNG
  on a perfectly cycle-deterministic emulator. The CNTPCT-jitter
  source is best-effort under such conditions. #895 will quantify
  the residual entropy honestly.

---

## SSH

The SSH server (#199) lands across five sub-tickets. This section
fills out as those land.

- **#893** — wolfSSH / wolfCrypt vendoring + lwIP IO + RNG (this PR).
- **#892** — Ed25519 host-key generation and persistence.
- **#891** — channel routing + sshd autostart.
- **#896** — password authentication, user management, bootstrap gate.
- **#895** — hardening, interop matrix, entropy audit, default-on flip.

Until #896 lands the bootstrap gate, the daemon is opt-in (no
default-on auto-start). Until #895 audits the cipher / KDF surface,
the deployment is "trust the lab network."

---

*Last updated: May 2026.*
