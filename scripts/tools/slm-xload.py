#!/usr/bin/env python3
"""
Stream a local GGUF directly to a running SLM-OS instance via the
`slm xload` shell verb.

`slm xload` bypasses the LittleFS round-trip used by `slm load`, so peak
kernel memory is the file size — no second buffer for the registry copy.
This is the supported way to load a Qwen-class GGUF on Jetson, where the
8 GB system can produce only one order-19 (2 GB) PMM buddy at a time
(see `docs/setup.md` §"Jetson SD-Card Layout" for the full rationale).

Wire protocol (kernel side: `slm_xload` in `kernel/src/slm_shell.c`):

  client:  slm xload <name> <total>\\n
  kernel:  SLM-XLOAD ready name=<name> total=<N>\\r\\n
  client:  <total> raw bytes (with 0xFF doubled per RFC 854 IAC stuffing)
  kernel:  SLM-XLOAD done received=<total>\\r\\n
  kernel:  [slm] loaded handle=<idx> ...
  kernel:  slmos>

Usage:

  scripts/tools/slm-xload.py <host> <name> <path>

Example (Jetson, after kexec'ing into SLM-OS):

  scripts/tools/slm-xload.py 192.168.4.100 qwen \\
      ~/models/qwen2.5-1.5b-instruct-q4_k_m.gguf
"""

from __future__ import annotations

import argparse
import os
import select
import socket
import sys
import time

# Match the kernel's `SLM_XLOAD_CHUNK_BYTES` (kernel/src/slm_shell.c)
# so each TCP write maps to one kernel read_raw call. 128 KB.
CHUNK_BYTES = 131072

# Default port for SLM-OS's TCPSH/TELNETD shell — see `[TCPSH]
# Listening on 0.0.0.0:2323` in the kernel boot log.
DEFAULT_PORT = 2323


def recv_until(sock: socket.socket, needle: bytes, timeout: float) -> bytes:
    """Read from `sock` until `needle` appears in the buffer or `timeout`
    seconds elapse. Returns the full buffer received so far on success;
    raises `TimeoutError` if the deadline passes without a match, or
    `EOFError` if the peer closed first."""
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        readable, _, _ = select.select([sock], [], [], 1.0)
        if not readable:
            continue
        data = sock.recv(65536)
        if not data:
            raise EOFError(
                f"connection closed waiting for {needle!r}; got {bytes(buf)!r}"
            )
        buf.extend(data)
        if needle in buf:
            return bytes(buf)
    raise TimeoutError(
        f"timeout waiting for {needle!r}; got {bytes(buf)!r}"
    )


def stream_payload(sock: socket.socket, path: str, size: int) -> None:
    """Send `size` bytes of `path` over `sock`, doubling any 0xFF bytes
    (RFC 854 IAC stuffing — the kernel's telnet RX layer un-stuffs
    transparently). Prints a progress line every 5 seconds."""
    sent = 0  # original (pre-stuffing) bytes the kernel will count
    t0 = time.time()
    last_print = t0
    with open(path, "rb") as f:
        while sent < size:
            chunk = f.read(CHUNK_BYTES)
            if not chunk:
                break
            # Only allocate a doubled-IAC copy when the chunk actually
            # contains 0xFF — the common case (random binary data has
            # 1-in-256 bytes as 0xFF) is rare enough that the branch
            # is worth it.
            wire = (
                chunk.replace(b"\xff", b"\xff\xff") if 0xFF in chunk else chunk
            )
            sock.sendall(wire)
            sent += len(chunk)

            now = time.time()
            if now - last_print >= 5.0:
                elapsed = now - t0
                rate_mb = sent / elapsed / 1024 / 1024 if elapsed > 0 else 0.0
                pct = 100.0 * sent / size
                eta = (
                    (size - sent) / max(1.0, rate_mb * 1024 * 1024)
                    if rate_mb > 0
                    else 0
                )
                print(
                    f"  {sent}/{size} ({pct:.1f}%) | "
                    f"{rate_mb:.2f} MB/s | ETA {int(eta)}s",
                    flush=True,
                )
                last_print = now


def main() -> int:
    ap = argparse.ArgumentParser(
        description=(
            "Stream a GGUF to a running SLM-OS instance via the "
            "`slm xload` shell verb."
        )
    )
    ap.add_argument("host", help="SLM-OS hostname/IP (telnet shell on :2323)")
    ap.add_argument("name", help="Name to register the model under (max 31 chars)")
    ap.add_argument("path", help="Local GGUF file to upload")
    ap.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help=f"Telnet port (default: {DEFAULT_PORT})",
    )
    ap.add_argument(
        "--connect-timeout",
        type=float,
        default=30.0,
        help="Seconds to wait for the initial socket connection (default: 30)",
    )
    ap.add_argument(
        "--load-timeout",
        type=float,
        default=600.0,
        help=(
            "Seconds to wait for the kernel's `SLM-XLOAD done` reply after "
            "the upload finishes. Generous default because rust_slm_load "
            "parsing + tokenizer build for a Qwen2.5-1.5B GGUF can take "
            "tens of seconds on Jetson (default: 600)"
        ),
    )
    args = ap.parse_args()

    if not os.path.isfile(args.path):
        print(f"[slm-xload] {args.path}: not a regular file", file=sys.stderr)
        return 1
    size = os.path.getsize(args.path)
    if size == 0:
        print(f"[slm-xload] {args.path}: empty file", file=sys.stderr)
        return 1

    print(
        f"[slm-xload] {args.path} -> {args.host}:{args.port} "
        f"as '{args.name}' ({size} bytes)",
        flush=True,
    )

    sock = socket.create_connection((args.host, args.port),
                                    timeout=args.connect_timeout)
    # Disable Nagle so the last partial chunk doesn't sit in the OS
    # send buffer waiting for an ACK that the kernel only sends after
    # it sees the missing bytes — same gotcha as `slm-put.py` documents.
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        # Wait for the shell prompt before sending the command. Skipping
        # this would race with the telnet handshake on a freshly-accepted
        # connection.
        recv_until(sock, b"slmos>", timeout=args.connect_timeout)

        cmd = f"slm xload {args.name} {size}\r\n".encode()
        sock.sendall(cmd)

        ready = recv_until(sock, b"SLM-XLOAD ready",
                           timeout=args.connect_timeout)
        ready_line = ready.decode(errors="replace").strip().splitlines()[-1]
        print(f"[slm-xload] kernel: {ready_line}", flush=True)

        t_xfer_start = time.time()
        stream_payload(sock, args.path, size)
        xfer_elapsed = time.time() - t_xfer_start

        # `SLM-XLOAD done` arrives shortly after the last byte; the
        # subsequent `[slm] loaded handle=...` line then runs through
        # rust_slm_load_take_pages, which can take tens of seconds.
        done = recv_until(sock, b"SLM-XLOAD done", timeout=args.load_timeout)
        done_line = done.decode(errors="replace").strip().splitlines()[-1]
        print(f"[slm-xload] kernel: {done_line}", flush=True)

        # Surface the post-load info lines (handle, arch, dims) before
        # the next prompt. Best-effort: if the connection closes here
        # we still consider the upload succeeded, the model is loaded.
        try:
            tail = recv_until(sock, b"slmos>", timeout=args.load_timeout)
            for line in tail.decode(errors="replace").splitlines():
                line = line.strip()
                if line and "slmos>" not in line and "SLM-XLOAD" not in line:
                    print(f"  {line}", flush=True)
        except (TimeoutError, EOFError):
            pass

        rate_mb = size / xfer_elapsed / 1024 / 1024 if xfer_elapsed > 0 else 0.0
        print(
            f"[slm-xload] streamed {size} bytes in {xfer_elapsed:.1f}s "
            f"({rate_mb:.2f} MB/s)",
            flush=True,
        )
        return 0
    finally:
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
