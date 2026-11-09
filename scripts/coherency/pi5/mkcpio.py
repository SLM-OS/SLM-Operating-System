#!/usr/bin/env python3
"""Write a gzip newc cpio for the #1193 Pi 5 initramfs without root.

Usage: mkcpio.py OUT.gz BUSYBOX LITMUS INIT [ECTLR_KO]
Creates /init, /bin/busybox, /bin/sh -> busybox, /coherency/litmus, empty
/proc /sys /dev /tmp, and the /dev/console character node (5,1) that the
kernel opens before running /init. Entries are owned by root, mtime 0.
"""
import gzip
import stat
import sys


def entry(out, ino, name, mode, data=b"", rdev=(0, 0)):
    name_b = name.encode() + b"\0"
    fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, rdev[0], rdev[1],
              len(name_b), 0)
    buf = ("070701" + "".join("%08X" % v for v in fields)).encode() + name_b
    buf += b"\0" * ((4 - len(buf) % 4) % 4)
    buf += data
    buf += b"\0" * ((4 - len(data) % 4) % 4)
    out.write(buf)


def read(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    out_path, busybox, litmus, init = sys.argv[1:5]
    ents = [
        ("bin", stat.S_IFDIR | 0o755, b"", (0, 0)),
        ("coherency", stat.S_IFDIR | 0o755, b"", (0, 0)),
        ("dev", stat.S_IFDIR | 0o755, b"", (0, 0)),
        ("proc", stat.S_IFDIR | 0o755, b"", (0, 0)),
        ("sys", stat.S_IFDIR | 0o755, b"", (0, 0)),
        ("tmp", stat.S_IFDIR | 0o1777, b"", (0, 0)),
        ("dev/console", stat.S_IFCHR | 0o600, b"", (5, 1)),
        ("bin/busybox", stat.S_IFREG | 0o755, read(busybox), (0, 0)),
        ("bin/sh", stat.S_IFLNK | 0o777, b"busybox", (0, 0)),
        ("coherency/litmus", stat.S_IFREG | 0o755, read(litmus), (0, 0)),
        ("init", stat.S_IFREG | 0o755, read(init), (0, 0)),
    ]
    if len(sys.argv) > 5:
        ents.append(("coherency/ectlr.ko", stat.S_IFREG | 0o644,
                     read(sys.argv[5]), (0, 0)))
    with gzip.GzipFile(out_path, "wb", compresslevel=9, mtime=0) as out:
        for i, (name, mode, data, rdev) in enumerate(ents, start=1):
            entry(out, i, name, mode, data, rdev)
        entry(out, 0, "TRAILER!!!", 0)


if __name__ == "__main__":
    main()
