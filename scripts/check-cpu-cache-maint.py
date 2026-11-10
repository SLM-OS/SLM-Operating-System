#!/usr/bin/env python3
"""Fail if CPU-to-CPU shared-state code calls the cache.h maintenance helpers.

Cortex-A76 (Pi 5) and Cortex-A78AE (Jetson) keep their caches coherent in
hardware for Normal WB Inner Shareable memory, so DC CVAC / DC CIVAC on
scheduler, task, SMP and IPC state is pure cost (#1193,
docs/adr/0007-cross-cpu-coherency.md). Ordering comes from locks,
__atomic_* acquire/release or smp_*mb(). The cache.h helpers stay for
device DMA, page tables and data read with caches off.

Every C file under CHECKED_DIRS is checked, plus CHECKED_FILES. The only
allowed calls are listed in ALLOWED: the secondary_mmu_* words, which a
secondary reads with its MMU and caches off.

Usage: check-cpu-cache-maint.py [--root DIR] [--partial]
"""
import argparse
import os
import re
import sys

CHECKED_DIRS = ['kernel/sched', 'kernel/ipc']
CHECKED_FILES = [
    'kernel/src/kprintf.c',
    'kernel/include/spinlock.h',
    'kernel/include/steal_deque.h',
    'kernel/include/sched.h',
    'kernel/include/task.h',
]

# (file, argument) pairs that are caches-off readers, not CPU sharing.
ALLOWED = {
    ('kernel/sched/smp.c', '&secondary_mmu_ttbr'),
    ('kernel/sched/smp.c', '&secondary_mmu_mair'),
    ('kernel/sched/smp.c', '&secondary_mmu_tcr'),
}

CALL = re.compile(r'\b(cache_(?:clean|invalidate|discard)(?:_range)?)\s*\(([^,);]*)')
# "dc" at a word start or right after a \t / \n escape inside an asm string.
RAW = re.compile(r'(?:\b|(?<=\\[tn]))dc\s+(?:civac|cvac|cvap|cvadp|ivac|zva)\b')


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.join(os.path.dirname(__file__), '..'))
    ap.add_argument('--partial', action='store_true',
                    help='tolerate missing CHECKED_FILES (self-test trees)')
    args = ap.parse_args()
    bad = []
    files = set()
    for d in CHECKED_DIRS:
        for top, _, names in os.walk(os.path.join(args.root, d)):
            for n in names:
                if n.endswith(('.c', '.h')):
                    files.add(os.path.relpath(os.path.join(top, n), args.root))
    for rel in CHECKED_FILES:
        if os.path.exists(os.path.join(args.root, rel)):
            files.add(rel)
        elif not args.partial:
            bad.append('%s: listed file is missing (renamed?)' % rel)
    for rel in sorted(files):
        code = strip_comments(open(os.path.join(args.root, rel)).read())
        for n, line in enumerate(code.split('\n'), 1):
            for m in CALL.finditer(line):
                if (rel, m.group(2).strip()) not in ALLOWED:
                    bad.append('%s:%d: %s(%s)' % (rel, n, m.group(1), m.group(2).strip()))
            if RAW.search(line):
                bad.append('%s:%d: raw DC by VA' % (rel, n))
    if bad:
        print('check-cpu-cache-maint: cache maintenance on CPU-shared state (#1193):')
        for b in bad:
            print('  ' + b)
        print('Use a lock, __atomic_* acquire/release or smp_*mb() instead.')
        return 1
    print('check-cpu-cache-maint: OK (%d files)' % len(files))
    return 0


if __name__ == '__main__':
    sys.exit(main())
