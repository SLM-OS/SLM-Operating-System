#!/usr/bin/env python3
"""Sensitivity view (labelled, not a bar): per-run values from c2t lines,
with NO chk filter. For each file prints, per CPU: share, att, f_pd, ok,
first_ok_us, chk. Then medians two ways for first success:
  (a) over runs with a success (the analyzer's T1 rule, as pre-registered)
  (b) runs with ok=0 counted as later than the whole run (censored high).
sens.py <file>...
"""
import re
import statistics
import sys

LINE = re.compile(r'c2t cpu=(\d+) (.*)')
KV = re.compile(r'(\w+)=(\S+)')
rows = {}
for fn in sys.argv[1:]:
    tot = 0
    per = {}
    for ln in open(fn, errors='replace'):
        m = LINE.search(ln)
        if not m:
            continue
        kv = dict(KV.findall(m.group(2)))
        per[int(m.group(1))] = kv
    tot = sum(int(v['exec']) for v in per.values())
    for c, kv in per.items():
        r = rows.setdefault(c, [])
        fo = kv['first_ok_us']
        r.append(dict(file=fn.split('/')[-1], share=100.0 * int(kv['exec']) / tot,
                      att=int(kv['att']), fpd=int(kv['f_pd']), ok=int(kv['ok']),
                      fo=None if fo == '-' else int(fo), chk=int(kv['chk'])))
n = len(sys.argv) - 1
print(f'files: {n}, no chk filter')
for c in sorted(rows):
    r = rows[c]
    sh = statistics.median(x['share'] for x in r)
    att = sum(x['att'] for x in r)
    fpd = sum(x['fpd'] for x in r)
    fos = [x['fo'] for x in r if x['fo'] is not None]
    cens = sorted([x['fo'] if x['fo'] is not None else 10**9 for x in r])
    a = statistics.median(fos) / 1000 if fos else None
    b = statistics.median(cens)
    b = 'none' if b >= 10**9 else f'{b / 1000:.1f}'
    chks = sorted(set(x['chk'] for x in r))
    print(f'CPU {c}: median share {sh:.2f}%; f_pd {fpd}/{att} = '
          f'{100.0 * fpd / att if att else 0:.1f}%; first success median '
          f'(a) {a if a is None else round(a, 1)} ms over {len(fos)} runs, '
          f'(b) {b} ms over {len(r)} runs (ok=0 runs censored high); '
          f'runs with ok=0: {sum(1 for x in r if x["ok"] == 0)}; chk values {chks}')
