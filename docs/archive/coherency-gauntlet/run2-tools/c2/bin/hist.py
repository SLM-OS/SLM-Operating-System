#!/usr/bin/env python3
"""Per-CPU failure-reason histograms from c2t lines.

hist.py <evidence-dir> <cell-path> <burst|sustained>
Sums each c2t field per CPU across that cell's runs and prints:
runs, runs where the CPU had ok=0, runs with exec=0, the attempt-level
reason totals (ok/iso/f_stale/f_pd/f_empty/f_other), the victim-visit
totals, total exec share, and first_ok_us median (sustained).
Also checks every run file has exactly one c2t line per CPU and chk=0
(or 1, an attempt in flight at the snapshot).
"""
import glob
import os
import re
import statistics
import sys

d, cell, mode = sys.argv[1:4]
files = sorted(glob.glob(os.path.join(d, f'{cell}-{mode}-run*.txt')))
LINE = re.compile(r'c2t cpu=(\d+) (.*)')
per = {}
problems = []
for fn in files:
    seen = {}
    for line in open(fn, errors='replace'):
        m = LINE.search(line)
        if not m:
            continue
        cpu = int(m.group(1))
        kv = dict(x.split('=', 1) for x in m.group(2).split() if '=' in x)
        seen[cpu] = kv
    if sorted(seen) != list(range(6)):
        problems.append(f'{os.path.basename(fn)}: cpus {sorted(seen)}')
    for cpu, kv in seen.items():
        if kv.get('chk') not in ('0', '1'):
            problems.append(f'{os.path.basename(fn)} cpu{cpu} chk={kv.get("chk")}')
        if kv.get('att') != kv.get('diag_att'):
            problems.append(f'{os.path.basename(fn)} cpu{cpu} att!=diag_att')
        p = per.setdefault(cpu, {'runs': 0, 'ok0': 0, 'exec0': 0, 'sum': {},
                                 'first_ok': []})
        p['runs'] += 1
        if kv.get('ok') == '0':
            p['ok0'] += 1
        if kv.get('exec') == '0':
            p['exec0'] += 1
        for k, v in kv.items():
            if v.lstrip('-').isdigit() and not k.endswith('_us'):
                p['sum'][k] = p['sum'].get(k, 0) + int(v)
        fo = kv.get('first_ok_us', '-')
        p['first_ok'].append(int(fo) if fo.lstrip('-').isdigit() else None)

print(f'== {cell} {mode}: {len(files)} run files')
for pr in problems:
    print('  PROBLEM', pr)
tot_exec = sum(p['sum'].get('exec', 0) for p in per.values()) or 1
keys_a = ['att', 'ok', 'iso', 'f_stale', 'f_pd', 'f_empty', 'f_other']
keys_v = ['v_pd', 'v_pd_work', 'v_busy', 'v_empty', 'v_gen', 'v_state',
          'v_cpu', 'v_aff', 'v_rm', 'pushfull']
for cpu in sorted(per):
    p = per[cpu]
    s = p['sum']
    fos = [x for x in p['first_ok'] if x is not None]
    med = int(statistics.median(fos)) if fos else '-'
    mx = max(fos) if fos else '-'
    share = 100.0 * s.get('exec', 0) / tot_exec
    print(f'  cpu{cpu} runs={p["runs"]} ok0_runs={p["ok0"]} exec0_runs={p["exec0"]} '
          f'exec={s.get("exec", 0)} share={share:.1f}% '
          + ' '.join(f'{k}={s.get(k, 0)}' for k in keys_a) + ' | '
          + ' '.join(f'{k}={s.get(k, 0)}' for k in keys_v)
          + f' | first_ok_us median={med} max={mx} none={p["first_ok"].count(None)}')
