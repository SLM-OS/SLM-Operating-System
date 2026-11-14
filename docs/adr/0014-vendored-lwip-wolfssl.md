# ADR 0014: vendor lwIP for TCP/IP and wolfSSH with wolfCrypt for SSH

Status: accepted (backfilled).

## Context

SLM-OS needs TCP/IP for the network shell (ADR 0012), DHCP, ping, HTTP fetch of runtime blobs and the telemetry daemon, on QEMU (VirtIO-Net), x86-64, Pi 5 (Cadence MACB/GEM, #202) and Jetson (USB networking). Telnet carries the shell without authentication or encryption, so `docs/ssh.md` ("Why") calls for an SSH server with encrypted transport, password login and a pinned host key (#199). Why third-party stacks were chosen over writing them: unknown, ask the user.

## Decision

**lwIP.** Vendored in `kernel/lib/lwip/` by 22bb84fd "Implement TCP/IP networking with lwIP and VirtIO-Net" (2025-12-27). The commit message says lwIP 2.1.3, but `src/include/lwip/init.h` at that commit and now reads 2.2.0 (`CHANGELOG` STABLE-2.2.0). It runs `NO_SYS=1`, raw API only (`LWIP_SOCKET=0`, `LWIP_NETCONN=0`), configured by `kernel/include/lwipopts.h`, with SLM-OS glue in `kernel/net/lwip_slm.c` and `kernel/net/sys_arch.c`. Built from `LWIP_SOURCES` into `slmos.elf` when `ENABLE_NETWORKING` is on (default ON for QEMU_VIRT, X86_64, RASPI5, JETSON_ORIN_NANO). Upstream changes: only `src/apps/http/http_client.c` and its header (0396250b, 56af2809, 9686c3c2, 2026-04-25, HTTP timeout and DNS-abort handling); they are not recorded in a vendor note.

**wolfCrypt and wolfSSH.** Vendored by 358d6358 "sshd 199a: crypto-quality RNG + vendoring scaffold (#893) (#902)" (2026-05-25), followed the same day by 4989faec (host keys, #892), 904fa48d (shell channel, #891), 3b446bdf (scrypt password auth and bootstrap gate, #896) and 5a67cc7e (hardening, autostart default, #895). Pinned to wolfSSL v5.7.4-stable (`kernel/lib/wolfcrypt/`, a subset of `src/`) and wolfSSH v1.4.18-stable (`kernel/lib/wolfssh/`, 7 source files; SFTP, SCP, agent and X11 forwarding stripped). Algorithm selection is `kernel/lib/wolfcrypt/user_settings.h`; glue in `kernel/net/ssh/` (`sshd.c`, `wolf_os.c`, `wolf_heap.c`, a 48 MB PMM-backed heap for scrypt). Both build as separate static libraries with relaxed warnings (`-Wno-error` and others), ARM64 only, when `NET_SSHD=ON` (default OFF; `NET_SSHD_AUTOSTART` default ON for Pi 5 and Jetson).

Local patches are listed in `kernel/lib/wolfssh/VENDOR.md`: two moves in `src/internal.c` so Ed25519 no longer depends on the ECDSA block (found under #988). The intended cipher, ChaCha20-Poly1305, is not offered by wolfSSH 1.4.18, so SSH uses AES-256-GCM. wolfSSH's `src/misc.c` is not compiled (symbol clash with wolfCrypt's).

## Alternatives considered

unknown, ask the user. The record names no other TCP/IP stack or SSH library; #199 and #893 assume wolfSSH from the start. `docs/security.md` notes argon2id was not used for passwords because wolfSSL does not ship it (scrypt instead).

## Consequences

- lwIP's raw API is not reentrant. After #1047 made tasks preemptible, every task-context lwIP call must hold `lwip_core_lock()`, valid only because all lwIP callers are pinned to CPU 0 (1e8183b1, #1065 / PR #1073; `kernel/CLAUDE.md`). `net_pump` runs under that IRQ-masking lock, which led to the #1093 CPU 0 deadlock.
- Open defect in the lwIP integration: one pbuf leaked per telnet session close (#1107).
- License: the SLM-OS repository is MIT (`LICENSE`); wolfCrypt is GPL-2.0 and wolfSSH GPL-3.0 per their `VENDOR.md` files, lwIP is BSD-style (`kernel/lib/lwip/COPYING`). None of the three is listed in `LICENSES.md`. Whether the GPL terms were reviewed for `NET_SSHD=ON` images: unknown, ask the user.
- The wolfCrypt `VENDOR.md` refresh procedure names `scripts/refresh-wolfcrypt.sh` as a TODO; no such script exists.
- #199 and #893 remain open although `docs/ssh.md` reports SSH shipped and hardware-validated on jetson-nano-1 and pi-5-2.
- `make test NET_SSHD=ON` raises `TEST_TIMEOUT` to 400 s for real scrypt under TCG (`Makefile`).

## Evidence

- Vendor notes: `kernel/lib/wolfcrypt/VENDOR.md`, `kernel/lib/wolfssh/VENDOR.md`; build: `CMakeLists.txt` (`LWIP_SOURCES`, `WOLFCRYPT_SOURCES`, `NET_SSHD*` options).
- Docs: `docs/networking.md` ("lwIP Settings", "lwIP Integration", "SSH"), `docs/ssh.md`, `docs/security.md` ("SSH").
- Tests: 983cb0cc "ssh: comprehensive functional tests + doc coverage refresh (#1000)" (2026-05-25), `kernel/tests/test_wolf_heap.c`, `test_lwip_core_lock_held_by_task_context_paths`.
