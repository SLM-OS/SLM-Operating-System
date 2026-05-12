#!/usr/bin/env python3
"""End-to-end smoke tests for the hailo8 QEMU stub device.

Drives a built QEMU through its qtest socket — no guest OS needed. The
qtest accelerator exposes a tiny text protocol that lets us program PCI
config space, then directly read/write BAR memory addresses to exercise
the device's MemoryRegionOps callbacks.

Four scenarios mapped to the Phase 0.2 Definition of Done:

  1. empty corpus      → first BAR0 read produces HAILO_RE_CORPUS_EXTEND
                         and QEMU exits non-zero.
  2. hand-crafted hit  → BAR0 read at the corpus's recorded offset
                         returns the recorded value.
  3. shape divergence  → a write where the corpus expects a read produces
                         HAILO_RE_CORPUS_DIVERGENCE with reason=
                         op_shape_mismatch and the expected_* extension
                         fields.
  4. write divergence  → a write whose value differs from the corpus
                         produces HAILO_RE_CORPUS_DIVERGENCE and exit.

Run:
    QEMU=~/projects/qemu/build/qemu-system-x86_64 python3 tests/smoke.py
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import time

QEMU = os.environ.get("QEMU") or os.path.expanduser(
    "~/projects/qemu/build/qemu-system-x86_64"
)
DEFAULT_BAR0_BASE = 0xFEB00000   # Top of the q35 PCI hole, just below LAPIC (0xFEC00000+)

# -------------------------------------------------------------------------- #
# qtest protocol — tiny client                                                #
# -------------------------------------------------------------------------- #


class QtestSession:
    """A blocking text-protocol client for QEMU's qtest accelerator socket."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def _readline(self, timeout=5.0):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buf:
            self.sock.settimeout(max(0.05, deadline - time.monotonic()))
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("qtest socket closed")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode("utf-8", errors="replace").rstrip("\r")

    def send(self, line):
        self.sock.sendall((line + "\n").encode("utf-8"))

    def cmd(self, line, expect="OK"):
        self.send(line)
        reply = self._readline()
        if not reply.startswith(expect):
            raise RuntimeError(f"qtest cmd {line!r}: reply {reply!r}")
        return reply

    # -- PCI config space via the q35 host bridge (ports 0xCF8 / 0xCFC).
    @staticmethod
    def _cfg_addr(bus, dev, fn, offset):
        return (1 << 31) | (bus << 16) | (dev << 11) | (fn << 8) | (offset & 0xFC)

    def pci_cfg_write32(self, bus, dev, fn, offset, value):
        addr = self._cfg_addr(bus, dev, fn, offset)
        self.cmd(f"outl 0xcf8 0x{addr:08x}")
        self.cmd(f"outl 0xcfc 0x{value:08x}")

    def pci_cfg_read32(self, bus, dev, fn, offset):
        addr = self._cfg_addr(bus, dev, fn, offset)
        self.cmd(f"outl 0xcf8 0x{addr:08x}")
        self.send("inl 0xcfc")
        reply = self._readline()
        if not reply.startswith("OK"):
            raise RuntimeError(f"pci_cfg_read32: {reply!r}")
        return int(reply.split()[1], 16)

    def writel(self, phys, value):
        self.cmd(f"writel 0x{phys:x} 0x{value:08x}")

    def readl(self, phys):
        self.send(f"readl 0x{phys:x}")
        reply = self._readline()
        if not reply.startswith("OK"):
            raise RuntimeError(f"readl: {reply!r}")
        return int(reply.split()[1], 16)

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def find_hailo8_dev(qt):
    """Walk bus 0 looking for the Hailo8 device. Returns (dev, fn)."""
    for dev in range(32):
        for fn in range(8):
            vid_did = qt.pci_cfg_read32(0, dev, fn, 0x00)
            if vid_did == 0xFFFFFFFF:
                continue
            vid = vid_did & 0xFFFF
            did = (vid_did >> 16) & 0xFFFF
            if vid == 0x1E60 and did == 0x2864:
                return (dev, fn)
    return None


def program_bar0(qt, dev, fn, base):
    """Write `base` into BAR0 + BAR1 (64-bit pair) and enable memory decode."""
    # BAR0 is a 64-bit BAR; bits 0-3 hold the BAR type flags. Preserve those
    # for the low half (memory + 64-bit, bits 0=0, 1-2=10, 3=prefetch don't care)
    # by reading first and OR-ing the address in.
    qt.pci_cfg_write32(0, dev, fn, 0x10, (base & 0xFFFFFFFF) | 0x04)  # 64-bit memory
    qt.pci_cfg_write32(0, dev, fn, 0x14, (base >> 32) & 0xFFFFFFFF)
    # Enable memory-space decode (bit 1 of PCI COMMAND).
    cmd = qt.pci_cfg_read32(0, dev, fn, 0x04) & 0xFFFF
    qt.pci_cfg_write32(0, dev, fn, 0x04, cmd | 0x02)


# -------------------------------------------------------------------------- #
# QEMU launcher                                                               #
# -------------------------------------------------------------------------- #


def launch_qemu(corpus_path, qtest_path):
    """Spawn QEMU in qtest mode, attached to a unix qtest socket."""
    args = [
        QEMU,
        "-nodefaults", "-display", "none", "-no-reboot",
        "-machine", "q35,accel=qtest",
        "-m", "256",
        "-qtest", f"unix:{qtest_path},server=on,wait=off",
        "-device", f"hailo8,corpus={corpus_path}",
    ]
    proc = subprocess.Popen(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    # Wait for the qtest socket to appear.
    deadline = time.monotonic() + 5.0
    while not os.path.exists(qtest_path):
        if proc.poll() is not None:
            out, err = proc.communicate()
            raise RuntimeError(
                f"QEMU exited before qtest socket appeared (rc={proc.returncode})\n"
                f"stdout: {out}\nstderr: {err}"
            )
        if time.monotonic() > deadline:
            proc.kill()
            raise RuntimeError("timed out waiting for qtest socket")
        time.sleep(0.05)
    return proc


def connect_qtest(qtest_path):
    sock = socket.socket(socket.AF_UNIX)
    sock.connect(qtest_path)
    qt = QtestSession(sock)
    # qtest has no connection banner — server stays silent until the
    # first command. A clock_step 0 is a no-op probe that confirms
    # the protocol is alive.
    qt.cmd("clock_step 0")
    return qt


# -------------------------------------------------------------------------- #
# Scenarios                                                                   #
# -------------------------------------------------------------------------- #


def write_corpus(path, entries):
    with open(path, "w") as fp:
        fp.write(json.dumps({
            "type": "header", "format_version": 1,
            "hailort_version": "4.23.0", "fw_version": "4.23.0",
            "capture_host": "qemu-x86_64-smoke",
            "slmos_base_sha": "0" * 40,
            "capture_started_at": "2026-05-12T00:00:00Z",
            "notes": "smoke-test fixture",
        }) + "\n")
        for e in entries:
            fp.write(json.dumps(e) + "\n")


def scenario(name, corpus_entries, qtest_ops, expect_exit_nonzero,
             expect_stdout_contains=None, read_result_check=None):
    """Run one scenario. Returns (ok, message)."""
    with tempfile.TemporaryDirectory() as td:
        corpus = os.path.join(td, "corpus.jsonl")
        qsock = os.path.join(td, "qtest.sock")
        write_corpus(corpus, corpus_entries)
        proc = launch_qemu(corpus, qsock)
        qt = connect_qtest(qsock)
        last_read = None
        try:
            located = find_hailo8_dev(qt)
            if located is None:
                proc.kill()
                return False, "hailo8 device not found on bus 0"
            dev, fn = located
            program_bar0(qt, dev, fn, DEFAULT_BAR0_BASE)
            for op in qtest_ops:
                kind = op[0]
                if kind == "readl":
                    last_read = qt.readl(DEFAULT_BAR0_BASE + op[1])
                elif kind == "writel":
                    qt.writel(DEFAULT_BAR0_BASE + op[1], op[2])
                else:
                    raise RuntimeError(f"unknown qtest op {kind!r}")
        except (RuntimeError, EOFError, ConnectionResetError) as e:
            # The device may abruptly exit(1) — expected for EXTEND / DIVERGENCE.
            if not expect_exit_nonzero:
                proc.kill()
                stdout, stderr = proc.communicate()
                return False, f"unexpected qtest error: {e}\nstdout: {stdout}\nstderr: {stderr}"
        finally:
            qt.close()

        # If we expect a clean exit, QEMU won't have terminated on its own —
        # the qtest commands are passive. Send SIGTERM to shut it down.
        # If we expect non-zero exit, the device's exit(1) should already
        # have fired; SIGTERM as a fallback is harmless.
        if not expect_exit_nonzero and proc.poll() is None:
            proc.terminate()

        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()

        rc = proc.returncode
        if expect_exit_nonzero and rc == 0:
            return False, f"expected non-zero exit, got 0\nstdout: {stdout}\nstderr: {stderr}"
        if not expect_exit_nonzero and rc != 0:
            return False, f"expected zero exit, got {rc}\nstdout: {stdout}\nstderr: {stderr}"

        if expect_stdout_contains:
            for s in expect_stdout_contains:
                if s not in stdout:
                    return False, (f"missing {s!r} in stdout\nstdout: {stdout}\nstderr: {stderr}")

        if read_result_check:
            ok, msg = read_result_check(last_read)
            if not ok:
                return False, f"{msg}\nstdout: {stdout}\nstderr: {stderr}"

    return True, "OK"


def scenario_empty_corpus():
    return scenario(
        name="empty_corpus_extend",
        corpus_entries=[],
        qtest_ops=[("readl", 0x0)],
        expect_exit_nonzero=True,
        expect_stdout_contains=[
            "HAILO_RE_CORPUS_EXTEND",
            "seq=1",
            "bar=0",
            "offset=0",
            "reason=unknown_read",
        ],
    )


def scenario_lookup_hit():
    entry = {
        "type": "op", "seq": 1, "bar": 0, "offset": 0, "size": 4,
        "dir": "read", "value": "deadbeef",
        "source": "slmos_observed",
        "validated_at_commit": "a" * 40,
        "validated_at": "2026-05-12T00:00:00Z",
    }
    expected = 0xefbeadde

    def check(read):
        if read != expected:
            return False, f"expected read=0x{expected:08x}, got 0x{read:08x}"
        return True, ""

    return scenario(
        name="lookup_hit",
        corpus_entries=[entry],
        qtest_ops=[("readl", 0x0)],
        expect_exit_nonzero=False,
        read_result_check=check,
    )


def scenario_shape_divergence():
    """Corpus expects a read at seq=1 but we issue a write — exercises the
    op_shape_mismatch path and the expected_* extension fields."""
    entry = {
        "type": "op", "seq": 1, "bar": 0, "offset": 0, "size": 4,
        "dir": "read", "value": "deadbeef",
        "source": "slmos_observed",
        "validated_at_commit": "a" * 40,
        "validated_at": "2026-05-12T00:00:00Z",
    }
    return scenario(
        name="shape_divergence",
        corpus_entries=[entry],
        qtest_ops=[("writel", 0x0, 0x00000001)],
        expect_exit_nonzero=True,
        expect_stdout_contains=[
            "HAILO_RE_CORPUS_DIVERGENCE",
            "seq=1",
            "bar=0",
            "dir=write",
            "expected=00000000",
            "observed=00000000",
            "reason=op_shape_mismatch",
            "expected_bar=0",
            "expected_offset=0",
            "expected_size=4",
            "expected_dir=read",
        ],
    )


def scenario_write_divergence():
    # value="01000000" in the corpus is LE-encoded uint32 0x00000001 (matches
    # the corpus spec's worked example for hex encoding: byte 0 is the LSB).
    # qtest's `writel 0x03000000` writes integer 0x03000000 — its LE-encoding
    # is "00000003".
    entry = {
        "type": "op", "seq": 1, "bar": 0, "offset": 0, "size": 4,
        "dir": "write", "value": "01000000",
        "source": "qemu_capture",
        "validated_at_commit": None, "validated_at": None,
    }
    return scenario(
        name="write_divergence",
        corpus_entries=[entry],
        qtest_ops=[("writel", 0x0, 0x03000000)],
        expect_exit_nonzero=True,
        expect_stdout_contains=[
            "HAILO_RE_CORPUS_DIVERGENCE",
            "seq=1",
            "dir=write",
            "reason=write_value_mismatch",
            "expected=01000000",
            "observed=00000003",
        ],
    )


# -------------------------------------------------------------------------- #
# Main                                                                        #
# -------------------------------------------------------------------------- #


def main():
    if not os.path.exists(QEMU):
        print(f"QEMU binary not found at {QEMU!r} — set QEMU=... to override",
              file=sys.stderr)
        return 2

    scenarios = [
        ("empty_corpus_extend", scenario_empty_corpus),
        ("lookup_hit",          scenario_lookup_hit),
        ("shape_divergence",    scenario_shape_divergence),
        ("write_divergence",    scenario_write_divergence),
    ]

    failures = 0
    for name, fn in scenarios:
        print(f"[smoke] {name:30s} ", end="", flush=True)
        ok, msg = fn()
        if ok:
            print("OK")
        else:
            print("FAIL")
            print(f"    {msg}")
            failures += 1

    print(f"\n{len(scenarios) - failures} / {len(scenarios)} scenarios passed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
