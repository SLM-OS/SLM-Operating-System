#!/usr/bin/env python3
"""Write one cell's evidence files from this agent's transcript.

assign.py <cell-path> <since-ISO> <until-ISO> <outdir>

In [since, until): the boot capture is the serial_capture whose
until_pattern starts with 'slmos>'; confirmed serial_sends are numbered in
order per command ('bench stealing' -> burst, '... --sustain ...' ->
sustained, 'coherency litmus' -> litmus). Unconfirmed (token-request)
calls are skipped: they send nothing. Prints a manifest; refuses to
overwrite an existing file.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract  # noqa: E402

cell, since, until, outdir = sys.argv[1:5]
uses, results, order = extract.load()
plan = []
burst = sustained = litmus = 0
for tid in order:
    ts, name, inp = uses[tid]
    if not (since <= ts < until):
        continue
    if name.endswith('serial_capture'):
        if (inp.get('until_pattern') or '').startswith('slmos>'):
            plan.append((tid, f'{cell}-boot.txt'))
        continue
    if not inp.get('confirm_token'):
        continue
    data = inp.get('data', '')
    if data == 'bench stealing':
        burst += 1
        plan.append((tid, f'{cell}-burst-run{burst:02d}.txt'))
    elif data.startswith('bench stealing --sustain'):
        sustained += 1
        plan.append((tid, f'{cell}-sustained-run{sustained:02d}.txt'))
    elif data == 'coherency litmus':
        litmus += 1
        plan.append((tid, f'{cell}-litmus.txt' if litmus == 1
                     else f'{cell}-litmus-{litmus}.txt'))
    else:
        plan.append((tid, f'{cell}-other-{tid[-8:]}.txt'))

os.makedirs(outdir, exist_ok=True)
for tid, fn in plan:
    out = os.path.join(outdir, fn)
    if os.path.exists(out):
        sys.exit(f'refusing to overwrite {out}')
for tid, fn in plan:
    sys.argv = ['extract.py', 'dump', tid, os.path.join(outdir, fn)]
    extract.main()
print(f'{cell}: boot={sum(1 for _, f in plan if f.endswith("boot.txt"))} '
      f'burst={burst} sustained={sustained} litmus={litmus} files={len(plan)}')
