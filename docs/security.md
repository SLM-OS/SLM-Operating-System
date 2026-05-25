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

The SSH daemon (#199, Phase 3) is built on a vendored wolfSSH
1.4.18-stable + wolfCrypt (wolfSSL 5.7.4-stable) under
`kernel/lib/{wolfssh,wolfcrypt}/`. CMake gates the entire surface
on `NET_SSHD` (default OFF outside lab targets; default ON on Pi 5
+ Jetson because the bootstrap gate makes that safe).

### Threat model

In-scope:
- Confidentiality + integrity of the SSH session against a passive
  network observer.
- Resistance to first-connection MITM via host-key pinning (operator
  records the SLM-OS-side fingerprint via `sshd fingerprint`).
- Refusal to permit shell access without a credential.

Out of scope (documented non-goals per #199):
- Public-key authentication (deferred follow-up).
- HSM / TPM-backed host keys.
- SFTP / SCP / X11 forwarding / agent forwarding (stripped from
  the wolfSSH build).
- DoS resistance against an unbounded attacker (see Hardening).

### Cryptographic surface

| Primitive | Choice | Justification |
|---|---|---|
| Key exchange | curve25519-sha256 | RFC 8731; no NIST curves in the build |
| Host key | Ed25519 (RFC 8032) | curve25519 family, fast, no NIST primitives |
| AEAD cipher | AES-256-GCM | RFC 5647; wolfSSH 1.4.18's chosen cipher menu doesn't include chacha20-poly1305@openssh.com — original ticket spec called for ChaCha but the library only ships AES. AES-GCM is OpenSSH's first-choice cipher today |
| MAC | (implicit, AEAD) | GCM tag is the MAC |
| Password KDF | scrypt N=2^15, r=8, p=1 | RFC 7914 floor, OWASP second recommendation. argon2id isn't vendored (wolfssl doesn't ship it) |
| Salt | 16 random bytes per user | `rng_get_bytes` — see RNG section above |

The `user_settings.h` under `kernel/lib/wolfcrypt/` is the single
source of truth for the feature surface. Enabling additional
algorithms requires explicit edits there + matching source-set
additions in `CMakeLists.txt`.

### Constant-time guarantees

Verified by source inspection against the vendored 5.7.4-stable
wolfCrypt; the audit summary lives in
[`docs/security.md#constant-time-audit`](#constant-time-audit) below
and is the responsibility of the maintainer to refresh per upstream
bump.

- **Curve25519 scalar multiplication**: `fe_operations.c`/`ge_operations.c`
  paths used by wolfCrypt with `ECC_TIMING_RESISTANT` enabled
  (set in user_settings.h). The scalar-mul loop processes a fixed
  254 bits regardless of input.
- **Ed25519 signature verification**: `wc_ed25519_verify_msg`
  computes both sides of the comparison and uses
  `ConstantCompare` on the final 32-byte image.
- **ChaCha20-Poly1305 / AES-GCM tag check**: wolfCrypt's
  `ConstantCompare` (in `wolfcrypt/src/misc.c`) is the only call
  site for the AEAD tag comparison. Inspected — XOR-and-OR
  reduction over the full 16 bytes; no early exit.
- **Password hash comparison**: SLM-OS-side
  `passwd.c::constant_time_compare` uses an XOR-OR accumulator
  over the full 32-byte derived hash; no first-mismatch short
  circuit.

### Bootstrap gate

`sshd_userauth_passwd` (`kernel/net/ssh/sshd.c`) returns
`WOLFSSH_USERAUTH_FAILURE` for every authentication attempt unless
`passwd_any_users()` returns true. That predicate is true iff at
least one entry exists in `/mnt/files/etc/passwd`. On a fresh boot
with `NET_SSHD_AUTOSTART=ON`, the listener accepts connections but
every login attempt fails — there's no exploit window.

The operator's first action on a fresh image:

```
slmos> adduser root <password>
```

is what opens the gate. The boot banner advertises the gate state
so the operator can tell at a glance whether the daemon is
accepting logins.

### Host-key persistence

Stored under `/mnt/files/etc/ssh/`:

- `host_ed25519_key` — 32-byte raw seed (SLM-OS-native, not the
  OpenSSH PEM envelope). Only the kernel reads this.
- `host_ed25519_key.pub` — OpenSSH-compatible
  `ssh-ed25519 <base64> SLM-OS\n` line. Drop into
  `~/.ssh/known_hosts` directly.

Atomic write (`.tmp` + rename); power loss mid-write leaves either
the prior key or the new key, never a torn half. The fingerprint
(`sshd fingerprint`) is the SHA-256 of the RFC 4253 ssh-ed25519
blob, base64-encoded without padding, prefixed with `SHA256:` —
matches what OpenSSH 6.8+ prompts for on first connection.

### Hardening

DoS resistance is the current gap. Today every wrong-password
attempt costs ~100 ms of scrypt CPU on Pi 5; there's no per-IP
rate limit, no connection-rate cap, no pre-auth resource ceiling.
Follow-ups:

- [ ] Per-source-IP failure backoff (1 s after 3 fails, 10 s
      after 10, 60 s after 30 — reset on success or 5 min idle).
- [ ] Connection-rate cap per source IP.
- [ ] Pre-auth memory + CPU limit; drop a connection after N
      seconds of stalled auth.
- [ ] Run hydra at 100 conn/s for 60 s; confirm a concurrent
      UART shell session stays responsive.
- [ ] Wire a per-session authenticated-user field on
      `shell_session` so `whoami` from inside an SSH session
      returns the actual login name (currently reports "ssh").

### Entropy audit checklist

Run off-target with a multi-MB capture from `rng read`:

- [ ] `ent` battery (Fourchette / Shannon / chi-squared / mean / pi
      Monte-Carlo / serial correlation). TRNG path expected to pass
      all five; jitter path expected to pass at acceptable margins.
- [ ] NIST SP800-90B IID / non-IID conformance tests. Document
      the chosen entropy estimator and the H_min the source proves.
- [ ] Stuck-state behaviour: cap RDRAND with `cpufreq` lockstep
      to provoke `hw_degrade_events` and confirm the fallback to
      jitter is taken cleanly.

### Constant-time audit

The above section is the *current* claim. Per upstream wolfCrypt
bump, the auditor must re-walk the four primitives and refresh
this section.

### Interop matrix

Refreshed per `make kernel NET_SSHD=ON PLATFORM=<target>` deploy
on the lab boards. Today's known-good set:

| Client | Version | Status |
|---|---|---|
| OpenSSH (Linux) | 9.x | Pending hardware validation |
| OpenSSH (macOS) | bundled | Pending |
| PuTTY | 0.8x | Pending |
| Windows Terminal SSH | bundled | Pending |
| Termius (mobile) | latest | Optional / pending |

Hardware lab time tracked separately; see PR #199e thread.

---

*Last updated: May 2026.*
