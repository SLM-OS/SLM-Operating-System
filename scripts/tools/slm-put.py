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
        self.buf = bytearray()

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    def _handle_iac(self, data: bytes, idx: int) -> int:
        if idx + 1 >= len(data):
            return len(data)

        cmd = data[idx + 1]
        if cmd == IAC:
            self.buf.append(IAC)
            return idx + 2

        if cmd in (DO, DONT, WILL, WONT):
            if idx + 2 >= len(data):
                return len(data)
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
            return len(data)

        return idx + 2

    def _recv_some(self) -> None:
        try:
            data = self.sock.recv(4096)
        except socket.timeout:
            return
        if not data:
            raise RuntimeError("connection closed by remote host")
        i = 0
        while i < len(data):
            if data[i] == IAC:
                i = self._handle_iac(data, i)
            else:
                self.buf.append(data[i])
                i += 1

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
SHELL_MAX_LINE = 1024
SHELL_LINE_HEADROOM = 16


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
        choices=("auto", "framed", "legacy"),
        default="auto",
        help="Upload protocol (default: %(default)s)",
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
        default=512,
        help="Bytes per put chunk before hex encoding (default: 512)",
    )
    p.add_argument(
        "--prompt",
        default="slmos> ",
        help="Shell prompt to wait for (default: %(default)s)",
    )
    p.add_argument(
        "--timeout",
        type=float,
        default=10.0,
        help="Seconds to wait for each prompt (default: %(default)s)",
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
            offset = existing
            if offset > 0:
                print(f"resuming at {offset}/{total} bytes", file=sys.stderr)

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
                print(f"{offset}/{total} bytes uploaded", file=sys.stderr)
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

    print(
        f"uploaded {total} bytes to {args.remote_path} via "
        f"{args.transport}:{target_desc} protocol=legacy"
    )
    return 0


def xput_begin(shell: Shell, args: argparse.Namespace, total: int) -> bytes:
    out = shell.run_command(f"xput begin {args.remote_path} {total}")
    log_response(args.debug, "xput-begin", out)
    return out


def parse_xput_received(output: bytes) -> int | None:
    text = output.decode("utf-8", errors="replace")
    if "XPUT active" not in text:
        return None
    for token in text.replace("\r", " ").replace("\n", " ").split():
        if token.startswith("received="):
            try:
                return int(token.split("=", 1)[1])
            except ValueError:
                return None
    return None


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
        received = parse_xput_received(status)
        status_text = status.decode("utf-8", errors="replace")
        if received is not None and f"path={args.remote_path}" in status_text and f"size={total}" in status_text:
            offset = received
            if offset > 0:
                print(f"resuming framed upload at {offset}/{total} bytes", file=sys.stderr)
        else:
            out = xput_begin(shell, args, total)
            if shell_command_failed(out):
                print("xput begin failed", file=sys.stderr)
                return 1

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
                print(f"{offset}/{total} bytes uploaded", file=sys.stderr)
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
                received = parse_xput_received(status)
                status_text = status.decode("utf-8", errors="replace")
                if received is None or f"path={args.remote_path}" not in status_text:
                    out = xput_begin(shell, args, total)
                    if shell_command_failed(out):
                        print("xput begin failed during recovery", file=sys.stderr)
                        return 1
                    offset = 0
                else:
                    offset = received
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

    print(
        f"uploaded {total} bytes to {args.remote_path} via "
        f"{args.transport}:{target_desc} protocol=framed"
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
    host = resolve_labctl_target(args.target) if args.labctl else args.target

    if args.transport == "telnet":
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

        probe = shell.run_command("xput status")
        log_response(args.debug, "xput-probe", probe)
        if not shell_command_failed(probe) and not unknown_command(probe):
            return upload_framed(shell, args, data, total, target_desc, shell_factory)
        return upload_legacy(shell, args, data, total, target_desc, shell_factory)
    finally:
        shell.close()


if __name__ == "__main__":
    sys.exit(main())
