# wolfSSH — vendored

Source: github.com/wolfSSL/wolfssh, tag **v1.4.18-stable**.
License: GPL-3.0 (commercial license also available from wolfSSL Inc.).
See `LICENSE` in this directory.

## Scope

Compiled only when `NET_SSHD=ON`. Provides the SSH-2 protocol stack
on top of the wolfCrypt primitives one directory over.

Subsystems intentionally stripped (per parent issue #199 / sub-ticket
#893):

- SFTP (`wolfsftp.c`)
- SCP (`wolfscp.c`)
- Agent forwarding (`agent.c`)
- X11 forwarding
- Channel multiplexing beyond a single shell channel
- Pseudoterminal emulation (we accept `pty-req` politely and copy the
  TERM string into `shell_session`, but do not run a real PTY layer)

Preserved subsystems:

- SSH-2 transport (`internal.c`, `ssh.c`, `io.c`)
- Curve25519-SHA-256 KEX
- Ed25519 host keys
- ChaCha20-Poly1305 AEAD cipher
- Password authentication (#199d wires the auth callback)
- One shell channel per accepted connection (bound to `shell_session`
  in #199c)

## Files vendored

`src/internal.c`, `src/io.c`, `src/log.c`, `src/misc.c`, `src/port.c`,
`src/ssh.c`, `src/keygen.c`. Headers mirror upstream layout under
`include/wolfssh/`.

## SLM-OS integration

Sits behind the SLM-OS-side glue under `kernel/net/ssh/`:
- `sshd.{h,c}` — listener + per-connection state + wolfSSH IO
  callbacks (recv/send bridging to lwIP raw API) + accept task.
- `sshd_shell.c` — `sshd status|start|stop` shell verb.
- `wolf_os.c` — XMALLOC/XFREE/XREALLOC + `slm_wolfcrypt_seed`
  (HASHDRBG seed source via `rng_get_bytes`) + the in-kernel
  `strncasecmp` / `strtok_r` / `wc_KeyPemToDer` stubs the
  freestanding toolchain doesn't supply.

Local configuration goes through `user_settings.h` in the
`wolfcrypt/` directory (wolfSSH inherits the same user_settings
via `WOLFSSL_USER_SETTINGS`).

## Local patches to upstream sources

| File | Change | Rationale |
|---|---|---|
| `src/internal.c` (~line 9920) | Move the `ed` (Ed25519) struct out of the `#ifndef WOLFSSH_NO_ECDSA` block where upstream had it nested | Upstream 1.4.18 makes Ed25519 transitively dependent on ECDSA / NIST P-curves, which we don't want. The two algorithms are independently selectable after the patch. |
| `src/internal.c` (~line 10250) | Move the `case ID_ED25519:` branch in `SendKexGetSigningKey` out of the same `#ifndef WOLFSSH_NO_ECDSA` block | Same defect as above, in a second site. Without this, `SendKexGetSigningKey` returns `WS_INVALID_ALGO_ID` (-1020) on the first ssh-ed25519 KEX attempt. Found via #988 hardware-validation pass. |

## SLM-OS-side workaround

The DER blob passed to `wolfSSH_CTX_UsePrivateKey_buffer` is generated with `wc_Ed25519KeyToDer` (NOT `wc_Ed25519PrivateKeyToDer`) so the PKCS#8 envelope embeds both the 32-byte private seed AND the 32-byte public key. The seed-only DER from the `_Private_` variant is decoded by wolfSSH via `wc_ed25519_import_private_only` which leaves `key->p = 0`. `wc_ed25519_sign_msg` uses `key->p` in the `hram` computation (line ~512 of `wolfcrypt/src/ed25519.c`), so a zero `key->p` produces signatures the client can't verify ("incorrect signature" client-side after a clean KEX). The full priv+pub DER routes through `wc_ed25519_import_private_key` which sets both `key->k` and `key->p` correctly. Found via #988 hardware-validation pass. See `kernel/net/ssh/sshd.c::sshd_start`.

## Cipher pivot — AES-256-GCM instead of ChaCha20-Poly1305

The original #199 / #893 spec called for `chacha20-poly1305@openssh.com`.
wolfSSH 1.4.18's encryption-algo gate at `include/wolfssh/internal.h:266`
accepts only AES-CBC / AES-CTR / AES-GCM — ChaCha20-Poly1305 is not
in the supported cipher menu. SLM-OS pivoted to **AES-256-GCM**
(RFC 5647, OpenSSH's current first-choice cipher) as the practical
workaround. Every modern OpenSSH client supports it.

wolfcrypt's ChaCha20 + Poly1305 sources remain vendored — they're
useful for the #199d password-KDF path and as a future cipher
option if either we patch wolfSSH or it adds the cipher upstream.

## Source-set restriction

`src/misc.c` from wolfSSH is **not** compiled. Its `ForceZero`,
`ConstantCompare`, `min`, `c32toa`, `ato32` symbols collide at link
time with wolfcrypt's `src/misc.c` (which is a superset). wolfSSH
inlines the helpers from wolfcrypt's `misc.o` at link time.
