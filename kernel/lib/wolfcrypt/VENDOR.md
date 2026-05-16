# wolfCrypt — vendored

Source: github.com/wolfSSL/wolfssl, tag **v5.7.4-stable**.
License: GPL-2.0 (commercial license also available from wolfSSL Inc.).
See `LICENSE` in this directory.

## Scope

Compiled only when `NET_SSHD=ON`. Provides the crypto primitives
that wolfSSH (one directory over) consumes:

- Curve25519 key agreement (`curve25519-sha256` KEX)
- Ed25519 signatures (host keys)
- ChaCha20-Poly1305 AEAD (`chacha20-poly1305@openssh.com` cipher)
- SHA-256, HMAC-SHA-256
- HKDF / scrypt (KDF for #199d password hashing)
- Crypto-quality RNG (wolfCrypt RNG, seeded by SLM-OS `rng_get_bytes`)

Everything else — RSA, DH, ECC NIST curves, AES, 3DES, MD5/SHA-1
beyond the wolfSSH-required surface, TLS — is compiled out via
`user_settings.h`.

## Files vendored

Source files in `src/` are the minimum subset wolfSSH 1.4.18 + the
above primitives reach at runtime. `include/wolfssl/` is mirrored as
expected by upstream includes (e.g. `#include <wolfssl/wolfcrypt/sha256.h>`).

Upstream paths follow `wolfssl/wolfssl/wolfcrypt/*.h` and
`wolfssl/wolfcrypt/src/*.c`. The repository preserves that layout so
diffs against upstream stay readable.

## How to refresh

```
# Fetch the new tag into ~/slmos-ref/wolfssl
git -C ~/slmos-ref/wolfssl fetch --tags
git -C ~/slmos-ref/wolfssl checkout v<new-tag>
# Re-run scripts/refresh-wolfcrypt.sh (TODO: written alongside the
# follow-up that lands actual wolfSSH compilation).
```

## SLM-OS local changes

Local modifications, if any, are confined to:

- `user_settings.h` (this directory) — SLM-OS feature config, not
  shipped upstream.
- The integration shims under `kernel/net/ssh/` and
  `kernel/include/rng.h` — pure SLM-OS code, no upstream modifications.

Any future patch against upstream sources must be documented here
with rationale and an upstream-ticket reference.
