# Critic C2: extract labctl serial calls from the critic's own transcript.
#   calls.py <transcript.jsonl>                      list calls
#   calls.py <transcript.jsonl> <out> <id> [<id>..]  write replies verbatim
import json, sys

T = sys.argv[1]
uses, res = {}, {}
order = []
for line in open(T):
    try:
        e = json.loads(line)
    except Exception:
        continue
    m = e.get('message') or {}
    c = m.get('content')
    if not isinstance(c, list):
        continue
    for b in c:
        if b.get('type') == 'tool_use' and 'labctl__serial' in b.get('name', ''):
            uses[b['id']] = (e.get('timestamp'), b['name'].split('__')[-1], b['input'])
            order.append(b['id'])
        if b.get('type') == 'tool_result' and b.get('tool_use_id') in uses:
            content = b.get('content')
            if isinstance(content, list):
                content = ''.join(x.get('text', '') for x in content)
            res[b['tool_use_id']] = (e.get('timestamp'), content)


def text_of(content):
    try:
        j = json.loads(content)
        return j.get('result', content)
    except Exception:
        return content


if len(sys.argv) == 2:
    for k in order:
        t, n, a = uses[k]
        a = {x: y for x, y in a.items() if x != 'confirm_token'}
        r = res.get(k, (None, ''))
        print(k, t, r[0], n, json.dumps(a, sort_keys=True)[:140], len(text_of(r[1] or '')))
    sys.exit(0)

out, ids = sys.argv[2], sys.argv[3:]
with open(out, 'w') as f:
    for i, k in enumerate(ids, 1):
        t, n, a = uses[k]
        a = {x: y for x, y in a.items() if x != 'confirm_token'}
        rt, content = res[k]
        f.write('# labctl %s reply %d of %d\n' % (n, i, len(ids)))
        f.write('# call %s  reply %s  tool_use_id %s\n' % (t, rt, k))
        f.write('# args %s\n' % json.dumps(a, sort_keys=True))
        body = text_of(content)
        f.write(body)
        if not body.endswith('\n'):
            f.write('\n')
