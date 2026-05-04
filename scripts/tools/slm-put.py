#!/usr/bin/env python3
"""
Push a local file onto a running SLM-OS instance over the shell transport.

Prefers the framed `xput` shell protocol when available, with fallback to the
older `put` / `put -a` chunk commands for compatibility.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import select
import socket
import subprocess
import sys
import time
from typing import Callable


IAC = 255
DO = 253
DONT = 254
WILL = 251
WONT = 252
SB = 250
SE = 240


class TelnetShell:
    def __init__(self, host: str, port: int, prompt: bytes, timeout: float):
        self.host = host
        self.port = port
        self.prompt = prompt
        self.timeout = timeout
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(0.25)
        # Disable Nagle. Without this, the OS coalesces our last partial
        # block on `xput-bin` uploads — the kernel ends up waiting forever
        # for the final 72-128 KB that never crosses the wire because
        # Nagle holds it for an ACK that the kernel doesn't send because
        # it's still waiting for those bytes.
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = bytearray()
        self.iac_pending = bytearray()

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

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
            if cmd in (DO, DONT):
                reply = bytes([IAC, WONT, opt])
            else:
                reply = bytes([IAC, DONT, opt])
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
            data = self.sock.recv(4096)
        except socket.timeout:
            return
        if not data:
            raise RuntimeError("connection closed by remote host")
        self._consume_telnet(data)

    def read_until_prompt(self) -> bytes:
        deadline = time.monotonic() + self.timeout
        while True:
            if self.prompt in self.buf:
                prompt_idx = self.buf.rfind(self.prompt)
                out = bytes(self.buf[: prompt_idx + len(self.prompt)])
                del self.buf[: prompt_idx + len(self.prompt)]
                return out
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    f"timed out waiting for shell prompt {self.prompt!r}"
                )
            self._recv_some()

    def run_command(self, command: str) -> bytes:
        self.sock.sendall(command.encode("ascii") + b"\n")
        return self.read_until_prompt()


class SerialShell:
    def __init__(self, target: str, prompt: bytes, timeout: float):
        self.target = target
        self.prompt = prompt
        self.timeout = timeout
        self.proc = subprocess.Popen(
            ["labctl", "connect", target],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if self.proc.stdin is None or self.proc.stdout is None:
            raise RuntimeError("failed to open labctl serial console pipes")
        self.buf = bytearray()

    def close(self) -> None:
        try:
            if self.proc.stdin:
                self.proc.stdin.close()
        except OSError:
            pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2.0)

    def _recv_some(self) -> None:
        if self.proc.stdout is None:
            raise RuntimeError("serial stdout is not available")
        deadline = time.monotonic() + 0.25
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            if not ready:
                return
            chunk = os.read(self.proc.stdout.fileno(), 4096)
            if not chunk:
                if self.proc.poll() is not None:
                    raise RuntimeError("labctl serial console exited unexpectedly")
                return
            self.buf.extend(chunk)
            return

    def read_until_prompt(self) -> bytes:
        deadline = time.monotonic() + self.timeout
        nudged = False
        while True:
            if self.prompt in self.buf:
                prompt_idx = self.buf.rfind(self.prompt)
                out = bytes(self.buf[: prompt_idx + len(self.prompt)])
                del self.buf[: prompt_idx + len(self.prompt)]
                return out
            if time.monotonic() >= deadline:
                if not nudged and self.proc.stdin is not None:
                    self.proc.stdin.write(b"\n")
                    self.proc.stdin.flush()
                    nudged = True
                    deadline = time.monotonic() + self.timeout
                    continue
                raise RuntimeError(
                    f"timed out waiting for shell prompt {self.prompt!r}"
                )
            self._recv_some()

    def run_command(self, command: str) -> bytes:
        assert self.proc.stdin is not None
        self.proc.stdin.write(command.encode("ascii") + b"\n")
        self.proc.stdin.flush()
        return self.read_until_prompt()


Shell = TelnetShell | SerialShell
# Must mirror SHELL_MAX_LINE in kernel/include/config.h. A mismatch
# truncates commands (kernel < client) or wastes headroom (kernel >
# client). 32768 chosen post-#581 to raise the framed-chunk binary
# ceiling to ~16 KB, cutting 1 GB round-trips to ~65K (vs. ~2.16M
# at the original 1024-char limit). See the same constant's comment
# in kernel/include/config.h for the full history.
SHELL_MAX_LINE = 32768
SHELL_LINE_HEADROOM = 16


def format_bytes(n: int) -> str:
    """Human-readable byte count. KB/MB/GB powers of 1024 because that's
    what filesystem sizes match locally; the wire isn't involved in
    this formatting."""
    if n < 1024:
        return f"{n} B"
    if n < 1024 * 1024:
        return f"{n / 1024:.1f} KB"
    if n < 1024 * 1024 * 1024:
        return f"{n / (1024 * 1024):.1f} MB"
    return f"{n / (1024 * 1024 * 1024):.2f} GB"


def format_rate(bytes_per_sec: float) -> str:
    if bytes_per_sec < 1024:
        return f"{bytes_per_sec:.0f} B/s"
    if bytes_per_sec < 1024 * 1024:
        return f"{bytes_per_sec / 1024:.1f} KB/s"
    return f"{bytes_per_sec / (1024 * 1024):.2f} MB/s"


def format_duration(seconds: float) -> str:
    """ETA / elapsed formatting. Drops the leading unit when zero so
    `5m 12s` reads cleaner than `0h 5m 12s`."""
    if seconds < 0 or not (seconds == seconds):  # NaN check
        return "?"
    total = int(seconds)
    h, rem = divmod(total, 3600)
    m, s = divmod(rem, 60)
    if h:
        return f"{h}h {m:02d}m {s:02d}s"
    if m:
        return f"{m}m {s:02d}s"
    return f"{s}s"


# Throttle for emit_progress: minimum wall-clock seconds between two
# stderr progress lines. 0.5s = up to 2 lines/sec, fast enough that
# the ETA visibly evolves but slow enough that a high-throughput
# transfer (e.g. post-#595 ~16 KB chunks at tens of MB/s) doesn't
# drown the terminal. Single-process script, so module-level state
# is fine.
_PROGRESS_THROTTLE_SECONDS = 0.5
_progress_last_emit_time = 0.0


def reset_progress_throttle() -> None:
    """Clear the throttle so the very next emit_progress fires
    immediately. Called at the top of upload_framed / upload_legacy
    so the first chunk's progress always lands without delay."""
    global _progress_last_emit_time
    _progress_last_emit_time = 0.0


def emit_progress(start_time: float, offset: int, total: int,
                  session_start_offset: int = 0) -> None:
    """One-line progress to stderr: percent, bytes/total, instantaneous
    rate (averaged over the whole upload so far for stability), and
    ETA. Called once per successful chunk in upload_framed and
    upload_legacy.

    `offset` is the absolute on-disk byte position; `session_start_offset`
    is what was already on disk when this script invocation started
    (non-zero on a resumed upload). Rate is computed over this
    session's bytes only — `(offset - session_start_offset) / elapsed` —
    so a resume that picks up the last 5 MB of a 100 MB file reports
    the real 5-MB throughput, not a wildly inflated 100-MB / short-
    elapsed number.

    Throttled to one line per _PROGRESS_THROTTLE_SECONDS of wall clock
    so very fast transfers don't spam stderr. The final byte (offset
    == total) always emits — completion is not skipped even if the
    final chunk landed within the throttle window — so the run's
    final progress line shows the closing rate before the summary.

    Average rate (not EWMA) keeps the math obvious; if a single chunk
    stalls, the rate dips visibly — which is the behavior you want for
    diagnosing slowdowns."""
    global _progress_last_emit_time
    now = time.monotonic()
    is_final = (offset >= total) and (total > 0)
    if not is_final and (now - _progress_last_emit_time) < _PROGRESS_THROTTLE_SECONDS:
        return
    _progress_last_emit_time = now

    elapsed = max(now - start_time, 0.001)
    session_bytes = max(offset - session_start_offset, 0)
    rate = session_bytes / elapsed
    pct = (offset / total * 100.0) if total > 0 else 100.0
    if rate > 0 and offset < total:
        eta = (total - offset) / rate
        eta_str = format_duration(eta)
    else:
        eta_str = "?"
    print(
        f"  {format_bytes(offset)}/{format_bytes(total)} "
        f"({pct:.1f}%) | {format_rate(rate)} avg | ETA {eta_str}",
        file=sys.stderr,
    )


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=(
            "Push a file to a running SLM-OS shell, preferring the framed "
            "xput protocol with fallback to put/put -a."
        )
    )
    p.add_argument(
        "target",
        help=(
            "Target hostname/IP for telnet, or an SBC / serial target when "
            "--transport serial is used"
        ),
    )
    p.add_argument("local_path", help="Local file to upload")
    p.add_argument("remote_path", help="Destination path on the SLM-OS VFS")
    p.add_argument(
        "--protocol",
        choices=("auto", "binary", "framed", "legacy"),
        default="auto",
        help=(
            "Upload protocol (default: %(default)s). 'binary' uses "
            "the post-#597-OptionB `xput-bin` direct binary stream "
            "for ~10x throughput vs 'framed'; falls back to 'framed' "
            "automatically under 'auto' if the kernel doesn't have "
            "xput-bin compiled in."
        ),
    )
    p.add_argument(
        "--transport",
        choices=("telnet", "serial"),
        default="telnet",
        help="Transport to use (default: %(default)s)",
    )
    p.add_argument("--port", type=int, default=2323, help="Telnet port (default: 2323)")
    p.add_argument(
        "--labctl",
        action="store_true",
        help="Resolve telnet target as an SBC name through `labctl info`",
    )
    p.add_argument(
        "--chunk-bytes",
        type=int,
        default=16384,
        help=(
            "Bytes per put chunk before hex encoding (default: 16384). "
            "Capped at runtime by max_framed_chunk_bytes() / "
            "max_legacy_chunk_bytes(); with SHELL_MAX_LINE = 32768 the "
            "effective ceiling is ~16363 B."
        ),
    )
    p.add_argument(
        "--prompt",
        default="slmos> ",
        help="Shell prompt to wait for (default: %(default)s)",
    )
    p.add_argument(
        "--timeout",
        type=float,
        default=60.0,
        help=(
            "Seconds to wait for each prompt (default: %(default)s). "
            "Sized for #597's worst-case LFS write latency near the "
            "end of a 1 GB transfer. A timeout shorter than the "
            "slowest single chunk forces a reconnect, which re-opens "
            "the LFS handle and makes throughput worse — not better."
        ),
    )
    p.add_argument(
        "--no-verify-size",
        action="store_true",
        help="Skip final `stat`-based size verification",
    )
    p.add_argument(
        "--connect-retries",
        type=int,
        default=1,
        help="Connection attempts before giving up (default: %(default)s)",
    )
    p.add_argument(
        "--retry-delay",
        type=float,
        default=1.0,
        help="Delay between connect retries in seconds (default: %(default)s)",
    )
    p.add_argument(
        "--debug",
        action="store_true",
        help="Print raw shell responses to stderr",
    )
    p.add_argument(
        "--chunk-retries",
        type=int,
        default=3,
        help="Recovery attempts after a chunk transport failure (default: %(default)s)",
    )
    p.add_argument(
        "--no-resume",
        action="store_true",
        help="Restart from byte 0 instead of resuming an existing remote file",
    )
    return p.parse_args()


def parse_size_from_stat(output: bytes) -> int | None:
    text = output.decode("utf-8", errors="replace")
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("Size: ") and line.endswith(" bytes"):
            size_text = line[len("Size: ") : -len(" bytes")]
            try:
                return int(size_text)
            except ValueError:
                return None
    return None


def stat_missing(output: bytes) -> bool:
    text = output.decode("utf-8", errors="replace")
    return "No such file or directory" in text or "file not found" in text


def shell_command_failed(output: bytes) -> bool:
    text = output.decode("utf-8", errors="replace")
    return "Command returned error:" in text


def unknown_command(output: bytes) -> bool:
    text = output.decode("utf-8", errors="replace")
    return "Unknown command:" in text


def log_response(debug: bool, label: str, output: bytes) -> None:
    if not debug:
        return
    text = output.decode("utf-8", errors="replace")
    print(f"--- {label} ---", file=sys.stderr)
    print(text, file=sys.stderr, end="" if text.endswith("\n") else "\n")


def resolve_labctl_target(name: str) -> str:
    proc = subprocess.run(
        ["labctl", "info", name],
        check=True,
        capture_output=True,
        text=True,
    )
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line.startswith("ethernet:") or line.startswith("wifi:"):
            _, _, value = line.partition(":")
            target = value.strip().split()[0]
            if target:
                return target
    raise RuntimeError(f"no network address found in `labctl info {name}`")


def connect_telnet(host: str, port: int, prompt: bytes, timeout: float,
                   retries: int, retry_delay: float) -> TelnetShell:
    last_error: Exception | None = None
    for attempt in range(1, retries + 1):
        try:
            return TelnetShell(host=host, port=port, prompt=prompt, timeout=timeout)
        except OSError as exc:
            last_error = exc
            if attempt == retries:
                break
            time.sleep(retry_delay)
    assert last_error is not None
    raise RuntimeError(
        f"failed to connect to {host}:{port} after {retries} attempt(s): {last_error}"
    )


def connect_serial(target: str, prompt: bytes, timeout: float,
                   retries: int, retry_delay: float) -> SerialShell:
    last_error: Exception | None = None
    for attempt in range(1, retries + 1):
        shell: SerialShell | None = None
        try:
            shell = SerialShell(target=target, prompt=prompt, timeout=timeout)
            shell.initial_output = shell.read_until_prompt()
            return shell
        except Exception as exc:
            last_error = exc
            if shell is not None:
                try:
                    shell.close()
                except Exception:
                    pass
            if attempt == retries:
                break
            time.sleep(retry_delay)
    assert last_error is not None
    raise RuntimeError(
        f"failed to connect to serial target {target} after {retries} attempt(s): {last_error}"
    )


def remote_stat(shell: Shell, path: str, debug: bool) -> int | None:
    out = shell.run_command(f"stat {path}")
    log_response(debug, f"stat:{path}", out)
    if shell_command_failed(out):
        if stat_missing(out):
            return None
        raise RuntimeError(f"remote stat failed for {path}")
    size = parse_size_from_stat(out)
    if size is None:
        raise RuntimeError(f"failed to parse remote size for {path}")
    return size


def reconnect(factory: Callable[[], Shell], debug: bool) -> Shell:
    shell = factory()
    initial = getattr(shell, "initial_output", None)
    if initial is None:
        initial = shell.read_until_prompt()
    log_response(debug, "reconnect", initial)
    return shell


def normalize_remote_path(remote_path: str, cwd: str = "/") -> str:
    if not remote_path:
        return cwd
    path = pathlib.PurePosixPath(remote_path)
    if path.is_absolute():
        parts = list(path.parts)
    else:
        parts = list(pathlib.PurePosixPath(cwd, remote_path).parts)

    normalized: list[str] = []
    for part in parts:
        if part in ("", "."):
            continue
        if part == "/":
            normalized = ["/"]
            continue
        if part == "..":
            if len(normalized) > 1:
                normalized.pop()
            continue
        if not normalized:
            normalized = ["/", part]
        else:
            normalized.append(part)

    if not normalized or normalized == ["/"]:
        return "/"
    return "/" + "/".join(part for part in normalized if part != "/")


def max_legacy_chunk_bytes(remote_path: str, offset: int) -> int:
    verb = "put" if offset == 0 else "put -a"
    prefix_len = len(verb) + 1 + len(remote_path) + 1
    return max(1, (SHELL_MAX_LINE - 1 - SHELL_LINE_HEADROOM - prefix_len) // 2)


def max_framed_chunk_bytes(offset: int) -> int:
    prefix_len = len("xput chunk ") + len(str(offset)) + 1
    return max(1, (SHELL_MAX_LINE - 1 - SHELL_LINE_HEADROOM - prefix_len) // 2)


def upload_legacy(shell: Shell, args: argparse.Namespace, data: bytes,
                  total: int, target_desc: str,
                  shell_factory: Callable[[], Shell]) -> int:
    start_time = time.monotonic()
    reset_progress_throttle()
    if total == 0:
        out = shell.run_command(f"put {args.remote_path} 00")
        log_response(args.debug, "put-empty", out)
        if shell_command_failed(out):
            print("remote put command failed", file=sys.stderr)
            return 1
        out = shell.run_command(f"truncate {args.remote_path} 0")
        log_response(args.debug, "truncate-empty", out)
        if shell_command_failed(out):
            print("remote truncate command failed", file=sys.stderr)
            return 1
        print(f"uploaded 0 bytes to {args.remote_path}")
        return 0

    offset = 0
    if args.no_resume:
        existing = remote_stat(shell, args.remote_path, args.debug)
        if existing is not None:
            out = shell.run_command(f"truncate {args.remote_path} 0")
            log_response(args.debug, "truncate-reset", out)
            if shell_command_failed(out):
                print("remote truncate command failed", file=sys.stderr)
                return 1
    else:
        existing = remote_stat(shell, args.remote_path, args.debug)
        if existing is not None:
            if existing > total:
                print(
                    f"remote file is larger than local file: remote={existing} local={total}",
                    file=sys.stderr,
                )
                return 1
            if existing > 0:
                print(
                    "legacy protocol cannot verify an existing prefix; restarting upload from 0",
                    file=sys.stderr,
                )
                out = shell.run_command(f"truncate {args.remote_path} 0")
                log_response(args.debug, "truncate-reset", out)
                if shell_command_failed(out):
                    print("remote truncate command failed", file=sys.stderr)
                    return 1

    while offset < total:
        base_offset = offset
        attempt = 0
        while True:
            chunk_limit = min(args.chunk_bytes, max_legacy_chunk_bytes(args.remote_path, offset))
            chunk = data[offset : offset + chunk_limit]
            hex_chunk = chunk.hex()
            verb = "put" if offset == 0 else "put -a"
            try:
                out = shell.run_command(f"{verb} {args.remote_path} {hex_chunk}")
                log_response(args.debug, f"{verb}@{offset}", out)
                if shell_command_failed(out):
                    raise RuntimeError(f"remote {verb} command failed")
                offset += len(chunk)
                emit_progress(start_time, offset, total)
                break
            except Exception as exc:
                attempt += 1
                if attempt > args.chunk_retries:
                    print(
                        f"upload failed near offset {base_offset}: {exc}",
                        file=sys.stderr,
                    )
                    return 1
                try:
                    shell.close()
                except Exception:
                    pass
                time.sleep(args.retry_delay)
                shell = reconnect(shell_factory, args.debug)
                current = remote_stat(shell, args.remote_path, args.debug)
                offset = 0 if current is None else current
                if offset > total:
                    print(
                        f"remote file grew unexpectedly after reconnect: {offset}>{total}",
                        file=sys.stderr,
                    )
                    return 1
                print(
                    f"recovered after chunk error; remote has {offset}/{total} bytes",
                    file=sys.stderr,
                )

    if not args.no_verify_size:
        remote_size = remote_stat(shell, args.remote_path, args.debug)
        if remote_size != total:
            print(
                f"remote size mismatch: expected {total}, got {remote_size}",
                file=sys.stderr,
            )
            return 1

    elapsed = max(time.monotonic() - start_time, 0.001)
    avg_rate = total / elapsed
    print(
        f"uploaded {format_bytes(total)} to {args.remote_path} via "
        f"{args.transport}:{target_desc} protocol=legacy "
        f"in {format_duration(elapsed)} ({format_rate(avg_rate)} avg)"
    )
    return 0


def xput_begin(shell: Shell, args: argparse.Namespace, total: int) -> bytes:
    out = shell.run_command(f"xput begin {args.remote_path} {total}")
    log_response(args.debug, "xput-begin", out)
    return out


def parse_xput_received(output: bytes) -> int | None:
    status = parse_xput_status(output)
    if status is None:
        return None
    return status[2]


def parse_xput_status(output: bytes) -> tuple[str, int, int, int] | None:
    text = output.decode("utf-8", errors="replace")
    path = None
    size = None
    received = None
    checksum = None
    if "XPUT active" not in text:
        return None
    for token in text.replace("\r", " ").replace("\n", " ").split():
        if token.startswith("path="):
            path = token.split("=", 1)[1]
        elif token.startswith("size="):
            try:
                size = int(token.split("=", 1)[1])
            except ValueError:
                return None
        elif token.startswith("received="):
            try:
                received = int(token.split("=", 1)[1])
            except ValueError:
                return None
        elif token.startswith("checksum="):
            raw = token.split("=", 1)[1]
            try:
                checksum = int(raw, 0)
            except ValueError:
                return None
    if path is None or size is None or received is None or checksum is None:
        return None
    return (path, size, received, checksum)


def fnv1a32(data: bytes) -> int:
    checksum = 0x811C9DC5
    for b in data:
        checksum ^= b
        checksum = (checksum * 0x01000193) & 0xFFFFFFFF
    return checksum


def can_resume_framed(status: tuple[str, int, int, int] | None,
                      remote_path: str,
                      total: int,
                      data: bytes) -> tuple[bool, int]:
    if status is None:
        return (False, 0)
    path, size, received, checksum = status
    if path != normalize_remote_path(remote_path) or size != total:
        return (False, 0)
    if received < 0 or received > total:
        return (False, 0)
    if checksum != fnv1a32(data[:received]):
        return (False, 0)
    return (True, received)


def parse_xput_resume(output: bytes) -> tuple[str, int, int, int] | None:
    """Parse `XPUT ok resume path=... size=... received=... checksum=...`."""
    text = output.decode("utf-8", errors="replace")
    if "XPUT ok resume" not in text:
        return None
    path = None
    size = None
    received = None
    checksum = None
    for token in text.replace("\r", " ").replace("\n", " ").split():
        if token.startswith("path="):
            path = token.split("=", 1)[1]
        elif token.startswith("size="):
            try:
                size = int(token.split("=", 1)[1])
            except ValueError:
                return None
        elif token.startswith("received="):
            try:
                received = int(token.split("=", 1)[1])
            except ValueError:
                return None
        elif token.startswith("checksum="):
            try:
                checksum = int(token.split("=", 1)[1], 0)
            except ValueError:
                return None
    if path is None or size is None or received is None or checksum is None:
        return None
    return (path, size, received, checksum)


def begin_or_resume_framed(shell: "Shell",
                           args: argparse.Namespace,
                           data: bytes,
                           total: int) -> int:
    """Try `xput resume` (continues an existing partial file on disk
    without truncating); fall back to `xput begin` (truncates and
    starts fresh) if the file doesn't exist or its checksum disagrees
    with the local source. Returns the offset to start uploading from
    (the resume point on success, 0 on begin).

    The resume path is the fix for the case where a TCP disconnect
    erases the kernel-side xput session state but leaves the partial
    file on disk. Without it, every reconnect runs `xput begin` which
    LFS_O_TRUNCs the file and the upload restarts at byte 0 — the
    longer the upload, the more wasted work."""
    out = shell.run_command(f"xput resume {args.remote_path} {total}")
    log_response(args.debug, "xput-resume", out)
    if not shell_command_failed(out):
        info = parse_xput_resume(out)
        if info is not None:
            path, size, received, checksum = info
            if (path == normalize_remote_path(args.remote_path)
                    and size == total
                    and 0 <= received <= total
                    and checksum == fnv1a32(data[:received])):
                return received
            # File exists but doesn't match — fall through to truncating begin.
            print(
                f"xput resume: existing file disagrees with source "
                f"(received={received}, restarting from 0)",
                file=sys.stderr,
            )
    out = xput_begin(shell, args, total)
    if shell_command_failed(out):
        raise RuntimeError("xput begin failed")
    return 0


def parse_xput_next(output: bytes) -> int | None:
    text = output.decode("utf-8", errors="replace")
    for token in text.replace("\r", " ").replace("\n", " ").split():
        if token.startswith("next="):
            try:
                return int(token.split("=", 1)[1])
            except ValueError:
                return None
    return None


def upload_framed(shell: Shell, args: argparse.Namespace, data: bytes,
                  total: int, target_desc: str,
                  shell_factory: Callable[[], Shell]) -> int:
    start_time = time.monotonic()
    reset_progress_throttle()
    if total == 0:
        out = xput_begin(shell, args, total)
        if shell_command_failed(out):
            print("xput begin failed", file=sys.stderr)
            return 1
        out = shell.run_command("xput finish")
        log_response(args.debug, "xput-finish-empty", out)
        if shell_command_failed(out):
            print("xput finish failed", file=sys.stderr)
            return 1
        print(
            f"uploaded 0 bytes to {args.remote_path} via "
            f"{args.transport}:{target_desc} protocol=framed"
        )
        return 0

    offset = 0
    if args.no_resume:
        out = xput_begin(shell, args, total)
        if shell_command_failed(out):
            print("xput begin failed", file=sys.stderr)
            return 1
    else:
        status = shell.run_command("xput status")
        log_response(args.debug, "xput-status", status)
        status_info = parse_xput_status(status)
        resume_ok, offset = can_resume_framed(status_info, args.remote_path, total, data)
        if resume_ok:
            if offset > 0:
                print(
                    f"resuming framed upload at {offset}/{total} bytes "
                    f"(in-memory session)",
                    file=sys.stderr,
                )
        else:
            try:
                offset = begin_or_resume_framed(shell, args, data, total)
            except RuntimeError as exc:
                print(f"{exc}", file=sys.stderr)
                return 1
            if offset > 0:
                print(
                    f"resuming framed upload at {offset}/{total} bytes "
                    f"(from disk)",
                    file=sys.stderr,
                )

    # Anchor for session-only rate math: any bytes already on disk
    # at this point were transferred in a prior script run, not this
    # one, so they don't count toward this session's throughput.
    session_start_offset = offset

    while offset < total:
        base_offset = offset
        attempt = 0
        while True:
            chunk_limit = min(args.chunk_bytes, max_framed_chunk_bytes(offset))
            chunk = data[offset : offset + chunk_limit]
            hex_chunk = chunk.hex()
            try:
                out = shell.run_command(f"xput chunk {offset} {hex_chunk}")
                log_response(args.debug, f"xput-chunk@{offset}", out)
                if shell_command_failed(out):
                    raise RuntimeError("remote xput chunk failed")
                remote_next = parse_xput_next(out)
                if remote_next is None or remote_next <= offset:
                    raise RuntimeError("failed to parse xput next offset")
                offset = remote_next
                emit_progress(start_time, offset, total, session_start_offset)
                break
            except Exception as exc:
                attempt += 1
                if attempt > args.chunk_retries:
                    print(
                        f"framed upload failed near offset {base_offset}: {exc}",
                        file=sys.stderr,
                    )
                    return 1
                try:
                    shell.close()
                except Exception:
                    pass
                time.sleep(args.retry_delay)
                shell = reconnect(shell_factory, args.debug)
                status = shell.run_command("xput status")
                log_response(args.debug, "xput-status-recover", status)
                status_info = parse_xput_status(status)
                resume_ok, remote_offset = can_resume_framed(
                    status_info, args.remote_path, total, data
                )
                if resume_ok:
                    offset = remote_offset
                else:
                    # In-memory session is gone (TCP close discards it).
                    # Fall back to disk-resume before truncating begin —
                    # this is the load-bearing change vs. pre-fix behavior
                    # where every reconnect blew away the entire partial
                    # file. begin_or_resume_framed tries `xput resume`
                    # first; only if the file doesn't exist or its
                    # checksum disagrees does it call `xput begin`.
                    try:
                        offset = begin_or_resume_framed(
                            shell, args, data, total
                        )
                    except RuntimeError as exc:
                        print(
                            f"{exc} (during recovery)", file=sys.stderr
                        )
                        return 1
                print(
                    f"recovered after chunk error; remote has {offset}/{total} bytes",
                    file=sys.stderr,
                )

    out = shell.run_command("xput finish")
    log_response(args.debug, "xput-finish", out)
    if shell_command_failed(out):
        print("xput finish failed", file=sys.stderr)
        return 1

    if not args.no_verify_size:
        remote_size = remote_stat(shell, args.remote_path, args.debug)
        if remote_size != total:
            print(
                f"remote size mismatch: expected {total}, got {remote_size}",
                file=sys.stderr,
            )
            return 1

    elapsed = max(time.monotonic() - start_time, 0.001)
    session_bytes = max(total - session_start_offset, 0)
    avg_rate = session_bytes / elapsed
    print(
        f"uploaded {format_bytes(total)} to {args.remote_path} via "
        f"{args.transport}:{target_desc} protocol=framed "
        f"in {format_duration(elapsed)} ({format_rate(avg_rate)} avg)"
    )
    return 0


def upload_xput_bin(shell: "Shell",
                    args: argparse.Namespace,
                    data: bytes,
                    total: int,
                    target_desc: str,
                    shell_factory: Callable[[], "Shell"]) -> int:
    """Direct binary upload via the post-#597-OptionB `xput-bin` shell
    command. Streams the raw file payload over the existing telnet
    session AS-IS (with mandatory 0xFF doubling per RFC 854). No hex
    expansion, no per-line shell parse — kernel reads raw bytes
    straight into LFS.

    Per-block staging on the kernel side is 128 KB (vs framed protocol's
    16 KB per chunk), which amortizes the LFS COW metadata cost over
    larger writes.

    Resume: the kernel reports the existing on-disk file size in its
    `XPUT-BIN ready offset=N` response; the script streams from N
    onwards, so reconnects after a partial upload pick up where they
    left off without re-sending the bytes already on disk.

    The transport is telnet only — serial would need additional flow
    control. SerialShell callers fall through to upload_framed.
    """
    if args.transport != "telnet":
        print("xput-bin requires telnet transport; falling back to framed",
              file=sys.stderr)
        return upload_framed(shell, args, data, total, target_desc, shell_factory)

    if not isinstance(shell, TelnetShell):
        print("xput-bin requires TelnetShell; falling back to framed",
              file=sys.stderr)
        return upload_framed(shell, args, data, total, target_desc, shell_factory)

    start_time = time.monotonic()
    reset_progress_throttle()

    # Send the command. read_until_prompt would expect a trailing
    # `slmos> ` which we DON'T want yet — the kernel emits
    # `XPUT-BIN ready offset=N\r\n` first, then we stream, then it
    # emits `XPUT-BIN done size=N\r\n` and the prompt. So bypass
    # run_command and parse manually.
    cmd = f"xput-bin {args.remote_path} {total}\n".encode("ascii")
    shell.sock.sendall(cmd)

    # Wait for the ready line. shell.buf is filled by _recv_some()
    # which strips telnet IAC. Search for the marker.
    deadline = time.monotonic() + args.timeout
    resume_offset: int | None = None
    while time.monotonic() < deadline:
        marker = b"XPUT-BIN ready offset="
        idx = shell.buf.find(marker)
        if idx >= 0:
            end = shell.buf.find(b"\n", idx)
            if end >= 0:
                line = bytes(shell.buf[idx:end]).decode("ascii", errors="replace")
                try:
                    resume_offset = int(line.split("offset=", 1)[1].strip())
                except (IndexError, ValueError):
                    resume_offset = None
                # Consume up through the newline so the post-stream
                # `XPUT-BIN done` parser doesn't re-find this line.
                del shell.buf[: end + 1]
                break
        # Pre-stream error from the kernel (e.g. reason=busy when
        # another session is already uploading, or open/seek failures
        # against the target path). Surface it instead of waiting out
        # the full --timeout for a "ready" line that will never come.
        err_idx = shell.buf.find(b"XPUT-BIN err")
        if err_idx >= 0:
            err_end = shell.buf.find(b"\n", err_idx)
            err_line = bytes(
                shell.buf[err_idx : err_end if err_end >= 0 else len(shell.buf)]
            ).decode("ascii", errors="replace").strip()
            print(f"xput-bin: kernel rejected upload: {err_line}",
                  file=sys.stderr)
            # Drain to the prompt so subsequent commands work.
            try:
                shell.read_until_prompt()
            except Exception:
                pass
            return 1
        # Check for unknown-command response (kernel without xput-bin).
        if b"Unknown command" in shell.buf or b"command not found" in shell.buf:
            print("xput-bin: kernel doesn't support it; falling back to framed",
                  file=sys.stderr)
            # Drain the rest of the line (and any prompt) so the
            # follow-up framed upload sees a clean buffer.
            try:
                shell.read_until_prompt()
            except Exception:
                pass
            return upload_framed(shell, args, data, total, target_desc, shell_factory)
        shell._recv_some()

    if resume_offset is None:
        print(
            f"xput-bin: timeout waiting for ready response (>{args.timeout}s); "
            "is the kernel running #597-OptionB?",
            file=sys.stderr,
        )
        return 1

    if resume_offset > total:
        print(
            f"xput-bin: kernel reports offset={resume_offset} > total={total}; "
            "abort (file on disk is larger than source)",
            file=sys.stderr,
        )
        return 1
    if resume_offset > 0:
        print(
            f"resuming binary upload at {resume_offset}/{total} bytes (from disk)",
            file=sys.stderr,
        )

    # Stream the remaining bytes with mandatory IAC byte-stuffing.
    # 0xFF in payload becomes 0xFF 0xFF on wire per RFC 854. The
    # kernel's existing telnet RX parser unstuffs transparently
    # before bytes reach the shell ring.
    #
    # Disable the recv-side 0.25s socket timeout for the duration
    # of the stream — sendall on a stalled TCP window otherwise
    # raises socket.timeout in 250 ms, far short of LFS-write
    # times (~150 ms per 64 KB block on RAM-backed LFS, easily
    # blocking longer when the kernel ring fills). Restored to
    # the pre-stream value before the post-stream prompt read.
    saved_timeout = shell.sock.gettimeout()
    shell.sock.settimeout(None)
    try:
        # 128 KB matches the kernel's bin_buf commit size in
        # cmd_xput_bin. Aligning script BLOCK with kernel commit
        # boundary makes the partial-commit-on-close case predictable:
        # if the FIN-after-data race trims the tail, the loss rounds
        # to a kernel commit boundary rather than mid-block.
        BLOCK = 128 * 1024
        sent = resume_offset
        payload = data[resume_offset:]
        cursor = 0
        while cursor < len(payload):
            block = payload[cursor : cursor + BLOCK]
            # Stuff: replace each 0xFF with 0xFF 0xFF.
            if b"\xff" in block:
                stuffed = block.replace(b"\xff", b"\xff\xff")
            else:
                stuffed = block
            shell.sock.sendall(stuffed)
            cursor += len(block)
            sent += len(block)
            emit_progress(start_time, sent, total, resume_offset)

        # No SHUT_WR. With the kernel's lwIP-level RX flow control
        # (PR follow-up to #621), `cmd_xput_bin` reads exactly
        # `total - resume_offset` bytes off the wire, then emits
        # "XPUT-BIN done" and returns. We don't need to half-close
        # to signal end-of-stream — the byte count is the signal.
        # Pre-flow-control we used SHUT_WR as a Nagle/buffer-flush
        # workaround, but TCP_NODELAY (set in TelnetShell.__init__)
        # already disables Nagle so the last sendall hits the wire
        # immediately. SHUT_WR also caused a FIN-after-data race
        # where the kernel saw "closed" before reading the final
        # block's bytes, dropping ~6-14 KB at the tail of large
        # uploads — the bug this whole change set fixes.
    finally:
        shell.sock.settimeout(saved_timeout)

    # Wait for the done line. Generous timeout — the kernel may have
    # ~ring_size bytes still in flight from sendall + a few LFS write
    # cycles to drain. For a multi-MB upload at typical LFS-write-bound
    # rates (~1-2 MB/s), 30 s is enough; for the 1+ GB case, scale
    # with file size.
    drain_timeout = max(args.timeout, 30.0, total / (256 * 1024))
    deadline = time.monotonic() + drain_timeout
    done_seen = False
    err_seen = False
    err_line: bytes = b""
    closed_seen = False
    while time.monotonic() < deadline:
        if b"XPUT-BIN done" in shell.buf:
            done_seen = True
            break
        if b"XPUT-BIN err" in shell.buf:
            err_idx = shell.buf.find(b"XPUT-BIN err")
            err_end = shell.buf.find(b"\n", err_idx)
            err_line = bytes(shell.buf[err_idx : err_end if err_end >= 0 else len(shell.buf)])
            err_seen = True
            break
        try:
            shell._recv_some()
        except RuntimeError:
            # Kernel closed the connection unexpectedly. Without our
            # SHUT_WR, this should only happen on a real abort/RST
            # (the kernel's normal `done` path leaves the connection
            # open until we close). Treat as "uncertain final state"
            # and stat the remote file to find out what actually
            # made it.
            closed_seen = True
            break
    if err_seen:
        print(f"xput-bin: kernel reported error: {err_line!r}",
              file=sys.stderr)
        return 1
    if not done_seen and not closed_seen:
        print(
            f"xput-bin: timeout ({drain_timeout:.0f}s) waiting for done response",
            file=sys.stderr,
        )
        return 1

    # Resolve actual on-disk size. On the done-seen path the existing
    # shell is still open and we can stat directly. On the closed_seen
    # path the kernel closed our session so we have to reconnect to
    # run stat — required for an honest "uploaded N bytes" report,
    # since the partial-commit-on-close path may have trimmed the
    # tail (FIN-after-data race, documented in PR #621). The user
    # can opt out of stat with --no-verify-size; in that mode the
    # closed_seen path returns 0 with a warning so existing scripted
    # callers don't break.
    actual_size: int | None
    if closed_seen:
        if args.no_verify_size:
            print("xput-bin: peer closed before done response; "
                  "skipping stat (--no-verify-size); assumed-complete",
                  file=sys.stderr)
            actual_size = None
        else:
            try:
                shell = reconnect(shell_factory, args.debug)
            except Exception as e:
                print(
                    f"xput-bin: peer closed before done response and "
                    f"reconnect failed ({e}); cannot verify size",
                    file=sys.stderr,
                )
                return 1
            try:
                actual_size = remote_stat(shell, args.remote_path, args.debug)
            except RuntimeError as e:
                print(f"xput-bin: stat after close failed: {e}",
                      file=sys.stderr)
                return 1
    else:
        # Drain through to the next prompt so the post-stream stat works.
        try:
            shell.read_until_prompt()
        except Exception:
            pass
        if args.no_verify_size:
            actual_size = None
        else:
            try:
                actual_size = remote_stat(shell, args.remote_path, args.debug)
            except RuntimeError as e:
                print(f"xput-bin: stat failed: {e}", file=sys.stderr)
                return 1

    elapsed = max(time.monotonic() - start_time, 0.001)

    # Rate reflects only bytes sent in THIS session — not the full file
    # size when the upload resumed. Without this, a resume that picks
    # up the last 6 KB of a 100 MB file reports the rate as
    # (100 MB / session-elapsed), which is wildly wrong.
    if actual_size is None:
        # --no-verify-size path. Best-effort: assume we sent all the
        # bytes we attempted to send (total - resume_offset).
        session_bytes = max(total - resume_offset, 0)
        avg_rate = session_bytes / elapsed
        print(
            f"uploaded {format_bytes(total)} to {args.remote_path} via "
            f"{args.transport}:{target_desc} protocol=binary "
            f"in {format_duration(elapsed)} ({format_rate(avg_rate)} avg)"
        )
        return 0

    if actual_size != total:
        # Partial — report honestly and exit non-zero so callers know.
        missing = total - actual_size
        session_bytes = max(actual_size - resume_offset, 0)
        avg_rate = session_bytes / elapsed
        print(
            f"xput-bin: partial upload — wrote {format_bytes(actual_size)} of "
            f"{format_bytes(total)} ({missing} bytes missing) to "
            f"{args.remote_path} via {args.transport}:{target_desc} "
            f"in {format_duration(elapsed)} ({format_rate(avg_rate)} avg). "
            f"Re-run xput-bin to resume from {actual_size}.",
            file=sys.stderr,
        )
        return 1

    session_bytes = max(actual_size - resume_offset, 0)
    avg_rate = session_bytes / elapsed
    print(
        f"uploaded {format_bytes(actual_size)} to {args.remote_path} via "
        f"{args.transport}:{target_desc} protocol=binary "
        f"in {format_duration(elapsed)} ({format_rate(avg_rate)} avg)"
    )
    return 0


def main() -> int:
    args = parse_args()
    if args.chunk_bytes <= 0:
        print("--chunk-bytes must be > 0", file=sys.stderr)
        return 2
    if args.connect_retries <= 0:
        print("--connect-retries must be > 0", file=sys.stderr)
        return 2
    if args.chunk_retries <= 0:
        print("--chunk-retries must be > 0", file=sys.stderr)
        return 2

    local_path = pathlib.Path(args.local_path)
    data = local_path.read_bytes()
    total = len(data)
    prompt = args.prompt.encode("ascii")

    # If the remote path looks like a directory (trailing "/" or "/."),
    # auto-append the local file's basename. The kernel's `xput begin`
    # opens its path with O_WRONLY|O_CREAT|O_TRUNC, which fails on a
    # directory — without this rewrite a `slm-put.py ... /mnt/files/.`
    # invocation always returns "xput begin failed".
    if args.remote_path.endswith("/."):
        args.remote_path = args.remote_path[:-1] + local_path.name
    elif args.remote_path.endswith("/"):
        args.remote_path = args.remote_path + local_path.name

    if args.transport == "telnet":
        host = resolve_labctl_target(args.target) if args.labctl else args.target
        target_desc = host

        def shell_factory() -> Shell:
            return connect_telnet(
                host=host,
                port=args.port,
                prompt=prompt,
                timeout=args.timeout,
                retries=args.connect_retries,
                retry_delay=args.retry_delay,
            )
    else:
        target_desc = args.target

        def shell_factory() -> Shell:
            return connect_serial(
                target=target_desc,
                prompt=prompt,
                timeout=args.timeout,
                retries=args.connect_retries,
                retry_delay=args.retry_delay,
            )

    shell = shell_factory()
    try:
        initial = getattr(shell, "initial_output", None)
        if initial is None:
            initial = shell.read_until_prompt()
        log_response(args.debug, "initial", initial)
        if args.protocol == "legacy":
            return upload_legacy(shell, args, data, total, target_desc, shell_factory)
        if args.protocol == "framed":
            return upload_framed(shell, args, data, total, target_desc, shell_factory)
        if args.protocol == "binary":
            return upload_xput_bin(shell, args, data, total, target_desc, shell_factory)

        # auto: prefer binary > framed > legacy. Probe for xput-bin
        # support by issuing a status-style command. Note: xput-bin
        # without args returns -1 with usage, so we use that as a
        # cheap "does the kernel know this command" check.
        probe_bin = shell.run_command("xput-bin")
        log_response(args.debug, "xput-bin-probe", probe_bin)
        if not unknown_command(probe_bin):
            return upload_xput_bin(shell, args, data, total, target_desc, shell_factory)

        probe = shell.run_command("xput status")
        log_response(args.debug, "xput-probe", probe)
        if not shell_command_failed(probe) and not unknown_command(probe):
            return upload_framed(shell, args, data, total, target_desc, shell_factory)
        return upload_legacy(shell, args, data, total, target_desc, shell_factory)
    finally:
        shell.close()


if __name__ == "__main__":
    sys.exit(main())
