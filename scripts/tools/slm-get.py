#!/usr/bin/env python3
"""
Pull a binary file off a running SLM-OS instance over the shell transport.

Mirror of slm-put.py for the device→host direction. Speaks the `xget-bin`
shell protocol: handshake header gives the file size, then the kernel
streams raw bytes (telnet IAC byte-stuffed) until done.

Usage:
    slm-get.py --host <ip> --port <port> --remote /mnt/files/foo.bin --out foo.bin
    slm-get.py --host <ip> --port <port> --remote /mnt/files/foo.bin --out foo.bin --resume

`--resume` continues a partial local file by sending the existing local
size as the kernel-side `skip` argument.
"""

from __future__ import annotations

import argparse
import os
import socket
import sys
import time

IAC = 255
DO = 253
DONT = 254
WILL = 251
WONT = 252
SB = 250
SE = 240


class TelnetShell:
    """Telnet-over-TCP shell with IAC un-stuffing.

    Mirrors slm-put.py's TelnetShell. ``buf`` holds the un-stuffed
    payload bytes (IAC IAC collapsed back to a single 0xFF, telnet
    option negotiations consumed and answered).
    """

    def __init__(self, host: str, port: int, prompt: bytes, timeout: float):
        self.host = host
        self.port = port
        self.prompt = prompt
        self.timeout = timeout
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(0.25)
        # See slm-put.py — Nagle would otherwise sit on the last partial
        # read window and stall the transfer near the tail.
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = bytearray()
        self.iac_pending = bytearray()

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    # --- IAC handling (identical to slm-put.py) -----------------
    def _handle_iac(self, data: bytes, idx: int) -> int | None:
        if idx + 1 >= len(data):
            return None
        cmd = data[idx + 1]
        if cmd == IAC:
            self.buf.append(IAC)
            return idx + 2
        if cmd in (DO, DONT, WILL, WONT):
            if idx + 2 >= len(data):
                return None
            opt = data[idx + 2]
            reply = bytes([IAC, WONT, opt]) if cmd in (DO, DONT) \
                else bytes([IAC, DONT, opt])
            self.sock.sendall(reply)
            return idx + 3
        if cmd == SB:
            j = idx + 2
            while j + 1 < len(data):
                if data[j] == IAC and data[j + 1] == SE:
                    return j + 2
                j += 1
            return None
        return idx + 2

    def _consume_telnet(self, data: bytes) -> None:
        if self.iac_pending:
            data = bytes(self.iac_pending) + data
            self.iac_pending.clear()
        i = 0
        while i < len(data):
            if data[i] != IAC:
                self.buf.append(data[i])
                i += 1
                continue
            next_i = self._handle_iac(data, i)
            if next_i is None:
                self.iac_pending.extend(data[i:])
                break
            i = next_i

    def _recv_some(self) -> None:
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return
        if not data:
            raise RuntimeError("connection closed by remote host")
        self._consume_telnet(data)

    # --- Line-mode helpers --------------------------------------
    def read_until_prompt(self) -> bytes:
        deadline = time.monotonic() + self.timeout
        while True:
            if self.prompt in self.buf:
                prompt_idx = self.buf.rfind(self.prompt)
                out = bytes(self.buf[:prompt_idx + len(self.prompt)])
                del self.buf[:prompt_idx + len(self.prompt)]
                return out
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    f"timed out waiting for shell prompt {self.prompt!r}"
                )
            self._recv_some()

    def read_until_substring(self, needle: bytes,
                             timeout: float | None = None) -> bytes:
        """Drain bytes into ``buf`` until ``needle`` appears, then return
        everything from buf up to and including the needle. Bytes after
        the needle stay in buf for subsequent reads — important for
        xget-bin since binary payload follows the `XGET-BIN ready` line
        with no separator.
        """
        deadline = time.monotonic() + (timeout or self.timeout)
        while True:
            idx = self.buf.find(needle)
            if idx >= 0:
                end = idx + len(needle)
                out = bytes(self.buf[:end])
                del self.buf[:end]
                return out
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    f"timed out waiting for {needle!r}"
                )
            self._recv_some()

    def read_n_bytes(self, n: int, timeout: float | None = None) -> bytes:
        """Drain exactly ``n`` un-stuffed bytes from buf, reading more
        from the socket as needed. Used for the binary middle of the
        xget-bin response.
        """
        deadline = time.monotonic() + (timeout or self.timeout)
        while len(self.buf) < n:
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    f"timed out reading binary stream "
                    f"({len(self.buf)}/{n} bytes)"
                )
            self._recv_some()
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def send_line(self, line: str) -> None:
        self.sock.sendall(line.encode("ascii") + b"\n")


# ---------------------------------------------------------------
# CLI
# ---------------------------------------------------------------
def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", required=True,
                   help="device hostname or IP")
    p.add_argument("--port", type=int, default=23,
                   help="telnet shell port (default 23)")
    p.add_argument("--remote", required=True,
                   help="remote file path on the device")
    p.add_argument("--out", required=True,
                   help="local output file path")
    p.add_argument("--resume", action="store_true",
                   help="resume by sending current local file size as skip")
    p.add_argument("--prompt", default="slmos> ",
                   help="shell prompt to detect (default 'slmos> ')")
    p.add_argument("--timeout", type=float, default=30.0,
                   help="per-step timeout in seconds (default 30)")
    p.add_argument("--total-timeout", type=float, default=600.0,
                   help="overall timeout in seconds (default 600)")
    p.add_argument("--quiet", action="store_true",
                   help="suppress progress output")
    return p.parse_args()


def format_bytes(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} {unit}"
        n /= 1024
    return f"{n:.1f} TB"


def format_rate(bps: float) -> str:
    return f"{format_bytes(int(bps))}/s"


def main() -> int:
    args = parse_args()
    prompt = args.prompt.encode("ascii")

    skip = 0
    open_mode = "wb"
    if args.resume:
        try:
            skip = os.path.getsize(args.out)
            open_mode = "ab"
        except FileNotFoundError:
            pass

    sh = TelnetShell(args.host, args.port, prompt, args.timeout)
    try:
        # Drain any banner / negotiation up to first prompt.
        sh.read_until_prompt()

        # Send the command.
        cmd = f"xget-bin {args.remote}"
        if skip > 0:
            cmd += f" {skip}"
        sh.send_line(cmd)

        # Wait for either an error line or the ready header. Errors
        # before ready start with "xget-bin:" or "XGET-BIN err".
        # We read until newline first to see what we got.
        deadline = time.monotonic() + args.timeout
        header_line = None
        while header_line is None:
            if time.monotonic() >= deadline:
                raise RuntimeError("timed out waiting for xget-bin header")
            sh._recv_some()
            for marker in (b"\nXGET-BIN ready size=", b"\nXGET-BIN err",
                           b"\nxget-bin:"):
                idx = sh.buf.find(marker)
                if idx >= 0:
                    # Find end-of-line for this marker.
                    eol = sh.buf.find(b"\n", idx + 1)
                    if eol < 0:
                        continue  # need more bytes
                    line = bytes(sh.buf[idx + 1:eol]).rstrip(b"\r")
                    del sh.buf[:eol + 1]
                    header_line = line
                    break

        if not header_line.startswith(b"XGET-BIN ready size="):
            print(f"error: {header_line.decode('ascii', 'replace')}",
                  file=sys.stderr)
            return 1

        total_str = header_line[len(b"XGET-BIN ready size="):]
        try:
            total = int(total_str)
        except ValueError:
            print(f"bad header: {header_line!r}", file=sys.stderr)
            return 1

        if skip > total:
            print(f"local size {skip} > remote size {total}; "
                  f"refusing to resume", file=sys.stderr)
            return 1

        to_read = total - skip
        if not args.quiet:
            print(f"xget-bin {args.remote}: total={format_bytes(total)} "
                  f"skip={format_bytes(skip)} to_read={format_bytes(to_read)}",
                  file=sys.stderr)

        # Stream binary bytes into the output file.
        t0 = time.monotonic()
        last_report = t0
        chunk = 65536
        received = 0
        with open(args.out, open_mode) as fh:
            while received < to_read:
                want = min(chunk, to_read - received)
                data = sh.read_n_bytes(want, timeout=args.total_timeout)
                fh.write(data)
                received += len(data)
                now = time.monotonic()
                if not args.quiet and (now - last_report) > 0.5:
                    elapsed = now - t0 or 1e-6
                    rate = received / elapsed
                    pct = 100.0 * received / max(to_read, 1)
                    print(f"  {format_bytes(received)}/{format_bytes(to_read)} "
                          f"({pct:5.1f}%) {format_rate(rate)}",
                          file=sys.stderr)
                    last_report = now

        # Trailer: "XGET-BIN done size=<total>\r\n"
        trailer = sh.read_until_substring(b"XGET-BIN done size=",
                                          timeout=args.timeout)
        # Drain to end-of-line so the prompt comes cleanly next.
        sh.read_until_substring(b"\n", timeout=args.timeout)
        if not trailer.endswith(b"XGET-BIN done size="):
            print(f"warning: unexpected trailer: {trailer!r}",
                  file=sys.stderr)

        elapsed = time.monotonic() - t0 or 1e-6
        rate = received / elapsed
        if not args.quiet:
            print(f"done: {format_bytes(received)} in {elapsed:.2f}s "
                  f"({format_rate(rate)})", file=sys.stderr)
        return 0

    finally:
        sh.close()


if __name__ == "__main__":
    sys.exit(main())
