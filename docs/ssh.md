# SSH (#199, Phase 3)

The SLM-OS SSH daemon: design, code layout, lifecycle, and operator
surface. Threat model and crypto-audit detail live in
[`docs/security.md`](security.md); operator-side flow lives in
[`docs/networking.md`](networking.md) §SSH; the at-a-glance status
matrix is [`docs/fact-sheets/networking.md`](fact-sheets/networking.md).

**Status:** shipped (May 2026). Software-side and hardware-side
validation complete on `jetson-nano-1` and `pi-5-2`.

## Why

Telnet (port 2323) had been the only multi-session network shell on
SLM-OS. It carries the same REPL the UART console drives but with
no authentication and no encryption, so it could only be used on
trusted networks. SSH adds:

- **Encrypted transport** (AES-256-GCM), preventing on-path read of
  shell I/O.
- **Authenticated shell** (password against a scrypt-hashed
  `/mnt/files/etc/passwd`), refusing unrecognised logins.
- **Server-identity pinning** (Ed25519 host key, persisted across
  reboots; SHA-256 fingerprint shown via the `sshd fingerprint`
  shell verb).
- **Bootstrap gate**: a fresh boot with autostart=ON refuses every
  login attempt until the operator runs `adduser` on the console at
  least once. There is no exploit window between boot and the first
  user provisioning.

## Code layout

| File | Responsibility |
|---|---|
| `kernel/include/rng.h` + `kernel/src/rng.c` | Crypto-quality RNG (per-arch TRNG + SHA-256-mixed jitter pool) feeding wolfssl HASHDRBG |
| `kernel/arch/{arm64,x86_64}/rng.c` | RNDR / RDRAND probe + read |
| `kernel/lib/wolfcrypt/`, `kernel/lib/wolfssh/` | Vendored wolfSSL 5.7.4 + wolfSSH 1.4.18 (with two SLM-OS-local patches documented in `kernel/lib/wolfssh/VENDOR.md`) |
| `kernel/lib/wolfcrypt/user_settings.h` | SLM-OS-side wolfssl/wolfssh config — algorithm gating, custom RNG, custom heap |
| `kernel/net/ssh/wolf_os.{c,h}` | SLM-OS-side glue: `XMALLOC/XFREE/XREALLOC`, RNG seed, WLOG callback, freestanding `strncasecmp/strtok_r` |
| `kernel/net/ssh/wolf_heap.{c,h}` | Dedicated 48 MB PMM-backed allocator behind XMALLOC — scrypt's 32 MB working buffer wouldn't fit in `lua_stubs.c`'s 1 MB pool |
| `kernel/net/ssh/host_key.{c,h}` | Ed25519 keypair gen / persist / fingerprint. PHC-style file layout under `/mnt/files/etc/ssh/`. Atomic `.tmp + rename` |
| `kernel/net/ssh/passwd.{c,h}` + `passwd_shell.c` | `/mnt/files/etc/passwd` (scrypt N=2^15 hashes, PHC line format). `adduser` / `passwd` / `deluser` / `whoami` shell verbs |
| `kernel/net/ssh/sshd.{c,h}` | Listener + per-connection accept loop + session task that drives `wolfSSH_accept` to the shell-channel bind |
| `kernel/net/ssh/shell_io_ssh.{c,h}` | `shell_io` vtable wrapping `wolfSSH_stream_read/send` — drops the post-KEX session into the existing REPL |
| `kernel/net/ssh/sshd_shell.c` | `sshd start/stop/status/fingerprint/regenerate-host-key` shell verbs |
| `kernel/net/ssh/sshd_autostart.{c,h}` | Boot-time entry that reads `/etc/sshd.conf` (`enabled=`, `port=`) and brings the daemon up |

## Connection lifecycle

```
                 +-------------------+
TCP SYN on 2222  | net_pump (lwIP)   |
   --------->   |  on_accept         |   tcp_accept callback
                +---------+---------+
                          |
                          | alloc_conn_locked (g_conns[4])
                          | wolfSSH_new(g_ctx)
                          | task_create("sshd-N") → CPU 0, TASK_PRIORITY_IDLE
                          v
                +---------+---------+
RX bytes via    | on_tcp_recv       |  fills per-conn 8 KB ring
tcp_recv cb     |                   |  (single writer = net_pump)
                +---------+---------+
                          |
                          v
                +---------+---------+
                | sshd_session_task |  yield-loops on wolfSSH_accept
                |                   |    → KEX (curve25519-sha256)
                |                   |    → user-auth (scrypt-against-passwd)
                |                   |    → shell channel
                +---------+---------+
                          |
                  KEX OK + channel
                          |
                          v
                +---------+---------+
                | shell_session_alloc + shell_io_ssh_create     |
                | shell_session_bind(this_task, sess)           |
                | shell_run() — same REPL as UART + telnet     |
                +-----------+-----------------------------------+
                            |
                  EOF / disconnect / `exit`
                            |
                            v
                +---------+---------+
                | unbind + destroy + conn_close_locked + task_exit |
                +---------------------+
```

Concurrency knobs:

- `g_mod_lock` (module spinlock) — protects `g_conns[]` slot allocation, `g_active` / `g_kex_completed` / `g_kex_failed` / `g_last_kex_err` / `g_last_kex_cpu` counter mutates, and `conn_close_locked`. Idempotent on double-close.
- per-conn `c->lock` — protects the RX ring (`rx_head` / `rx_tail` / `rx[]` / `peer_closed`).
- shell-task pinning to CPU 0 — keeps UART output ordering consistent with the existing console.

## Bootstrap gate

```
                                             +---+
                       wolfSSH_accept ----→  | / |  passwd_any_users() ?
                                             +---+
                                              /   \
                                          no /     \ yes
                                            /       \
                                  USERAUTH_FAILURE   passwd_verify()
                                  (every attempt)        ↓
                                                    OK / WRONG / NOT_FOUND
```

The gate's truth table:

| `passwd_any_users()` | `passwd_verify` result | wolfSSH callback returns |
|---|---|---|
| false | (not called) | `USERAUTH_FAILURE` |
| true  | `PASSWD_OK`   | `USERAUTH_SUCCESS` |
| true  | anything else | `USERAUTH_FAILURE` |

A boot with `NET_SSHD_AUTOSTART=ON` and an empty `/mnt/files/etc/passwd`
exposes only a listening port that rejects every login. The window
between boot and the first `adduser` is safe.

## Build flags

| Flag | Default | What it does |
|---|---|---|
| `NET_SSHD` | `OFF` | Master switch. Builds wolfssl + wolfssh static libs, links the SSH glue into `slmos.elf`. |
| `NET_SSHD_AUTOSTART` | `ON` on `RASPI5` / `JETSON_ORIN_NANO`, `OFF` elsewhere | Calls `sshd_autostart()` from `shell_init` at boot. Reads `/mnt/files/etc/sshd.conf` for runtime override (`enabled=` / `port=`). |
| `NET_SSHD_DEMO_ALLOW_ALL` | `OFF` (was `ON` during the #199c → #199d window) | Replaces `sshd_userauth_passwd` with a stub that accepts every login. For debugging only. `sshd_autostart` prints a loud WARNING banner on every boot when this is wired. |

## Test surface

| Test | Suite size | Scope |
|---|---|---|
| `kernel/tests/test_rng.c` | 9 cases | RNG init / source dispatch / jitter pool / injection / chi-squared self-test |
| `kernel/tests/test_sshd.c` | 7 cases | Listener + ring buffer round-trip via the test hooks in `sshd_test.h` |
| `kernel/tests/test_host_key.c` | 6 cases | Generate / persist / load / fingerprint round-trip via the actual VFS mount |
| `kernel/tests/test_passwd.c` | 10 cases | scrypt round-trip + length-bound regressions + bootstrap-gate behaviour |
| `kernel/tests/test_wolf_heap.c` | 11 cases | wolfssl heap alloc / free / realloc paths + magic-stamp violation + double-free regression |
| `kernel/tests/test_sshd_autostart.c` | 15 cases | `/etc/sshd.conf` parser internals (decimal / bool / line tokeniser) via `sshd_autostart_test.h` |

Run via `make test NET_SSHD=ON`. All 58 SSH-related tests pass on QEMU.

## Hardware validation

Verified end-to-end on `jetson-nano-1` (2026-04 to 2026-05) and `pi-5-2`
(2026-05):

- `adduser root <pw>` → ok
- `sshd status` → listener up on 2222
- `ssh -p 2222 root@<board-ip>` from a workstation OpenSSH 9.x → KEX
  completes, password auth succeeds, the SLM-OS Debug Shell banner
  prints, `help` runs over the encrypted channel
- `sshd fingerprint` matches OpenSSH's host-key prompt on first
  connection (and the prompt no longer fires after pinning)
- Host key persists across `reboot` cycles (Jetson kexec + Pi 5
  tryboot), proven by `sshd fingerprint` returning the same value

## Out of scope (documented non-goals)

- Public-key authentication — password is the only auth method. A
  follow-up ticket can add the pubkey callback when a deployment
  with key-management infrastructure materialises.
- HSM / TPM-backed host keys — the seed lives on the littlefs mount.
- Rate limiting / per-IP backoff — surface is unprotected against a
  password-guessing client. scrypt at N=2^15 makes online guessing
  expensive but not infeasible; rate-limiting is a future hardening
  ticket.
- Per-session identity plumbing — `whoami` currently reports
  `ssh` rather than the authenticated user. A follow-up ticket
  threads the SSH-auth username into `shell_session` so
  per-session attribution works.
- TLS — wolfSSL is vendored (it backs wolfSSH) but the TLS-side
  surface is gated off in `user_settings.h`.

## See also

- [`docs/security.md`](security.md) — threat model + crypto / entropy audit checklist
- [`docs/networking.md`](networking.md) §SSH — operator workflow
- [`docs/fact-sheets/networking.md`](fact-sheets/networking.md) — at-a-glance per-platform matrix
- [`kernel/lib/wolfssh/VENDOR.md`](../kernel/lib/wolfssh/VENDOR.md) — vendored versions + local patches
- [`README.md`](../README.md) §"SSH into your bare-metal OS" — quick-start

---

*Last updated: 2026-05-25.*
