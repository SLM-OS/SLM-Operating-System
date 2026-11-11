#!/usr/bin/env python3
"""Write one cell's run-3 evidence files from this agent's transcript.

assign.py cell  <cell> <since-ISO> <until-ISO> <outdir> [--dry-run]
    In [since, until): each serial_capture whose until_pattern starts with
    'slmos>' is a boot log: <cell>-boot.txt, then <cell>-boot2.txt, ...
    (a reboot inside a cell keeps its own boot log). Each confirmed
    `cat /sys/version` send opens a run; the next confirmed
    `bench stealing --sustain ...` send closes it, and both replies go into
    <cell>-runNN.txt in that order. A bench with no version before it, or
    any other confirmed send, gets its own <cell>-other-<id>.txt and is
    reported. Unconfirmed (token-request) sends send nothing and are
    skipped.

assign.py boots <prefix> <since-ISO> <until-ISO> <outdir> [--dry-run]
    Step 0: each slmos> serial_capture in the window -> <prefix>-bootNN.txt.

Refuses to overwrite an existing file. Prints the plan (tool_use ids and
filenames) so every file traces to the calls it came from.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract  # noqa: E402


def plan_cell(cell, since, until):
    uses, results, order = extract.load()
    plan, notes = [], []
    boots = runs = 0
    pending = None
    for tid in order:
        ts, name, inp = uses[tid]
        if not (since <= ts < until):
            continue
        if name.endswith('serial_capture'):
            if (inp.get('until_pattern') or '').startswith('slmos>'):
                boots += 1
                plan.append(([tid], f'{cell}-boot.txt' if boots == 1
                             else f'{cell}-boot{boots}.txt'))
            continue
        if not inp.get('confirm_token'):
            continue
        data = inp.get('data', '')
        if data == 'cat /sys/version':
            if pending:
                notes.append(f'version {pending} not followed by a bench')
                plan.append(([pending], f'{cell}-other-{pending[-8:]}.txt'))
            pending = tid
        elif data.startswith('bench stealing --sustain'):
            if pending is None:
                notes.append(f'bench {tid} without a version before it')
                plan.append(([tid], f'{cell}-other-{tid[-8:]}.txt'))
                continue
            runs += 1
            plan.append(([pending, tid], f'{cell}-run{runs:02d}.txt'))
            pending = None
        else:
            plan.append(([tid], f'{cell}-other-{tid[-8:]}.txt'))
    if pending:
        notes.append(f'version {pending} not followed by a bench')
        plan.append(([pending], f'{cell}-other-{pending[-8:]}.txt'))
    return plan, notes, f'{cell}: boots={boots} runs={runs}'


def plan_boots(prefix, since, until):
    uses, results, order = extract.load()
    plan = []
    for tid in order:
        ts, name, inp = uses[tid]
        if since <= ts < until and name.endswith('serial_capture') and \
                (inp.get('until_pattern') or '').startswith('slmos>'):
            plan.append(([tid], f'{prefix}-boot{len(plan) + 1:02d}.txt'))
    return plan, [], f'{prefix}: boots={len(plan)}'


def main():
    mode, name, since, until, outdir = sys.argv[1:6]
    dry = '--dry-run' in sys.argv[6:]
    plan, notes, summary = (plan_cell if mode == 'cell' else plan_boots)(
        name, since, until)
    for ids, fn in plan:
        print(f'{fn} <- {" + ".join(ids)}')
    for n in notes:
        print('NOTE', n)
    print(summary, f'files={len(plan)}')
    if dry:
        return
    os.makedirs(outdir, exist_ok=True)
    for ids, fn in plan:
        if os.path.exists(os.path.join(outdir, fn)):
            sys.exit(f'refusing to overwrite {fn}')
    for ids, fn in plan:
        extract.dump(ids, os.path.join(outdir, fn))


if __name__ == '__main__':
    main()
