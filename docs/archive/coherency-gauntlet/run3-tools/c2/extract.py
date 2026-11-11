#!/usr/bin/env python3
"""Extract labctl serial replies verbatim from this agent's transcript.

Run 2's c2/bin/extract.py with the transcript path taken from
$C2_TRANSCRIPT (default: coherency run 3 C2's transcript).

list:  extract.py list [since-ISO]           -> index of serial_send/serial_capture calls
dump:  extract.py dump <tool_use_id> <out> [<tool_use_id> ...]
       writes a '#' metadata header per reply, then the reply text exactly as
       labctl returned it (outer {"result": ...} JSON decoded, nothing else).
       Several ids go into one file in order (used for the version + bench
       pair of a run, and for split replies).
"""
import json
import os
import sys

TRANSCRIPT = os.environ.get('C2_TRANSCRIPT', (
    '/home/john/.claude/projects/'
    '-home-john-projects-CS-496-Capstone-SLM-Operating-System--claude-'
    'worktrees-bridge-cse-014Q9aryGYwccZqUzMHyre5U/'
    'a3f89c3b-b428-422e-83c3-16d5b1f3e49e/subagents/'
    'agent-a79529bf39e71d97a.jsonl'))
NAMES = ('mcp__labctl__serial_send', 'mcp__labctl__serial_capture')


def load():
    uses, results = {}, {}
    order = []
    with open(TRANSCRIPT) as f:
        for line in f:
            o = json.loads(line)
            ts = o.get('timestamp', '')
            c = o.get('message', {}).get('content')
            if not isinstance(c, list):
                continue
            for b in c:
                if b.get('type') == 'tool_use' and b.get('name') in NAMES:
                    if b['id'] not in uses:
                        order.append(b['id'])
                    uses[b['id']] = (ts, b['name'], b['input'])
                elif b.get('type') == 'tool_result' and b.get('tool_use_id'):
                    cc = b.get('content')
                    if isinstance(cc, list):
                        cc = ''.join(x.get('text', '') for x in cc
                                     if x.get('type') == 'text')
                    results[b['tool_use_id']] = (ts, cc)
    return uses, results, order


def decode(raw):
    try:
        return json.loads(raw)['result']
    except Exception:
        return raw


def dump(ids, out):
    uses, results, _ = load()
    with open(out, 'w') as f:
        for k, tid in enumerate(ids):
            ts, name, inp = uses[tid]
            rts, raw = results[tid]
            inp2 = {k2: v for k2, v in inp.items() if k2 != 'confirm_token'}
            f.write(f"# labctl {name.split('__')[-1]} reply {k + 1} of {len(ids)}\n")
            f.write(f"# call {ts}  reply {rts}  tool_use_id {tid}\n")
            f.write(f"# args {json.dumps(inp2, sort_keys=True)}\n")
            f.write(decode(raw))
            f.write('\n')
    print(out)


def main():
    if sys.argv[1] == 'list':
        uses, results, order = load()
        since = sys.argv[2] if len(sys.argv) > 2 else ''
        for i, tid in enumerate(order):
            ts, name, inp = uses[tid]
            if ts < since:
                continue
            res = decode(results.get(tid, (None, ''))[1] or '')
            first = res.split('\n', 1)[0][:70]
            kind = 'send' if name.endswith('send') else 'cap'
            confirmed = 'Y' if inp.get('confirm_token') else '-'
            print(f"{i:4d} {ts} {tid} {kind} conf={confirmed} "
                  f"data={inp.get('data', '')!r} "
                  f"until={inp.get('until_pattern', inp.get('capture_until', ''))!r} "
                  f"len={len(res)} | {first}")
    elif sys.argv[1] == 'dump':
        dump([sys.argv[2]] + sys.argv[4:], sys.argv[3])


if __name__ == '__main__':
    main()
