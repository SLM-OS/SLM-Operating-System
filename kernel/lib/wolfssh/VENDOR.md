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

Sits behind the SLM-OS-side glue under `kernel/net/ssh/` (sshd entry,
lwIP IO hooks, shell-channel binding). Upstream sources are not
modified — local configuration goes through `user_settings.h` in the
`wolfcrypt/` directory (wolfSSH inherits the same user_settings via
`WOLFSSL_USER_SETTINGS`).
