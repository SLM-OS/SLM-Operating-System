#!/usr/bin/env python3
"""Write an evidence file from a labctl reply that Claude Code saved to a
tool-results file because it was too large to return inline.

dump-saved.py <tool_use_id> <saved-json> <out>
Header as extract.py writes it, plus the saved file's path; then the
reply text exactly as labctl returned it ({"result": ...} decoded).
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract  # noqa: E402

tid, saved, out = sys.argv[1:4]
uses, results, _ = extract.load()
ts, name, inp = uses[tid]
rts = results.get(tid, ('?', ''))[0]
text = json.load(open(saved))['result']
if os.path.exists(out):
    sys.exit(f'refusing to overwrite {out}')
inp2 = {k: v for k, v in inp.items() if k != 'confirm_token'}
with open(out, 'w') as f:
    f.write(f"# labctl {name.split('__')[-1]} reply 1 of 1\n")
    f.write(f"# call {ts}  reply {rts}  tool_use_id {tid}\n")
    f.write(f"# args {json.dumps(inp2, sort_keys=True)}\n")
    f.write(f"# reply too large to return inline; saved by Claude Code to "
            f"{saved}\n")
    f.write(text)
    f.write('\n')
print(out)
