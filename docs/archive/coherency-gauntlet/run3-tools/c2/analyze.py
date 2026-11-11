#!/usr/bin/env python3
"""Coherency run 3, C2: per-cell tables from `bench stealing --sustain` files.

analyze.py [--head SHA] [--owner 1] [--per-run] <cell-label> <file>...

Reads only the evidence files named on the command line (one run per
file: `cat /sys/version` reply, then the complete `bench stealing
--sustain` reply). Nothing else is consulted. Every table names the
files it was computed from (first..last and count).

Checks before counting (a file that fails is listed and left out):
  - the banner hash in the file (`(<sha>)` after "build") matches --head
    (prefix match; skipped when --head is not given, e.g. run 2 files,
    which carry no `cat /sys/version`);
  - exactly one `c2t-format` header, one c2t line per CPU 0..cpus-1;
  - chk in {0,1} and att == diag_att on every line;
  - format v2 lines carry every v2 field; the watched CPU's wlog holds
    min(128, sum(win_n)) entries (else the line was truncated).

Tables (all per cell, pooled over the counted runs unless stated):
  T1 per CPU: median and pooled share of tasks executed, exec, att, ok,
     fraction of attempts per outcome, first-success median and max (ms,
     over runs with a success), runs with ok=0.
  T2 per thief: attempts that started inside the watched CPU's
     preempt_disabled window (fraction of att, by window reason, unknown),
     offset into the window (w_off), and the phase of attempts against
     the watched CPU's tick (ph_w, ph_w_pd) and against the thief's own
     tick (ph_own), each as % per bin, with the share within 200 us of
     the tick (uniform phase over a 10 ms tick gives 2.0%).
  T3 tick phase of each CPU against the watched CPU, per run, from
     tick_first_us: (tf_c - tf_w) mod period, period from the watched
     CPU's tick_n / tick span.
  T4 watched CPU's windows: count, total and mean duration, longest, by
     reason; window-entry phase against its own tick (wph); share of the
     run spent inside windows (from win_sum_us over wall-clock); and the
     wlog entries summarized (count by reason).
"""
import argparse
import os
import re
import statistics
import sys

PH_LABELS = ['<10us', '<20us', '<50us', '<100us', '<200us', '<500us',
             '<1ms', '<2ms', '<5ms', '<10ms', '>=10ms']
PH_EDGES_US = [0, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, None]
WOFF_LABELS = ['<1us', '<2us', '<4us', '<8us', '<16us', '<32us', '<64us',
               '>=64us']
REASONS = ['T', 'C', 'P', 'O', 'B']
OUTCOMES = ['ok', 'iso', 'f_stale', 'f_pd', 'f_empty', 'f_other']
V2_KEYS = ['w_in', 'w_in_unk', 'w_off', 'ph_w', 'ph_w_pd', 'ph_own',
           'tick_n', 'tick_first_us', 'tick_last_us', 'win_n', 'win_sum_us',
           'win_max_us', 'wph', 'wlog']

HDR = re.compile(r'c2t-format (v\d+) (.*)')
C2T = re.compile(r'\bc2t cpu=(\d+) (.*)')
# `cat /sys/version` prints "commit:  <sha>"; the boot banner prints
# "build <stamp> (<sha>)". Either names the head.
BANNER = re.compile(r'(?:commit:\s+|build \d+ \()([0-9a-f]{7,40})\b')
WALL = re.compile(r'Wall-clock:\s+(\d+) us')
WATCH = re.compile(r'#1216 watch=cpu(\d+)')


def uniform_ph(period_us=10000.0):
    """Expected % per ph bin for a uniform phase over one tick period."""
    out = []
    for i in range(len(PH_LABELS)):
        lo = PH_EDGES_US[i]
        hi = PH_EDGES_US[i + 1]
        hi = period_us if hi is None else min(hi, period_us)
        out.append(max(0.0, hi - lo) / period_us * 100.0 if lo < period_us
                   else 0.0)
    return out


def ints(s):
    if s is None or s == '-':
        return None
    return [int(x) for x in s.split(',')]


def num(s):
    if s is None or s == '-':
        return None
    try:
        return int(s)
    except ValueError:
        return None


def parse_file(fn):
    text = open(fn, errors='replace').read().replace('\r', '')
    rec = {'file': os.path.basename(fn), 'problems': []}
    m = BANNER.search(text)
    rec['banner'] = m.group(1) if m else None
    hdrs = HDR.findall(text)
    if len(hdrs) != 1:
        rec['problems'].append(f'{len(hdrs)} c2t-format headers')
        if not hdrs:
            return rec
    fmt, rest = hdrs[0]
    rec['fmt'] = fmt
    rec['hdr'] = dict(x.split('=', 1) for x in rest.split() if '=' in x)
    m = WALL.search(text)
    rec['wall_us'] = int(m.group(1)) if m else None
    m = WATCH.search(text)
    rec['watch'] = int(m.group(1)) if m else None
    cpus = int(rec['hdr'].get('cpus', '0'))
    lines = {}
    for line in text.split('\n'):
        m = C2T.search(line)
        if not m:
            continue
        cpu = int(m.group(1))
        if cpu in lines:
            rec['problems'].append(f'cpu{cpu} has two c2t lines')
        kv = {}
        for tok in m.group(2).split():
            if '=' in tok:
                k, v = tok.split('=', 1)
                kv[k] = v
        lines[cpu] = kv
    rec['cpu'] = lines
    if sorted(lines) != list(range(cpus)):
        rec['problems'].append(f'c2t cpus {sorted(lines)} != 0..{cpus - 1}')
    for cpu, kv in lines.items():
        if kv.get('chk') not in ('0', '1'):
            rec['problems'].append(f'cpu{cpu} chk={kv.get("chk")}')
        if kv.get('att') != kv.get('diag_att'):
            rec['problems'].append(f'cpu{cpu} att {kv.get("att")} != diag_att '
                                   f'{kv.get("diag_att")}')
        if fmt != 'v1':
            missing = [k for k in V2_KEYS if k not in kv]
            if missing:
                rec['problems'].append(f'cpu{cpu} missing v2 fields {missing}'
                                       ' (truncated line?)')
    w = rec['watch']
    if fmt != 'v1' and w is not None and w in lines and 'wlog' in lines[w]:
        kv = lines[w]
        wn = ints(kv.get('win_n'))
        wl = kv.get('wlog', '-')
        n_log = 0 if wl == '-' else len(wl.split(','))
        if wn is not None and n_log != min(128, sum(wn)):
            rec['problems'].append(f'watched cpu{w} wlog has {n_log} entries, '
                                   f'expected {min(128, sum(wn))} (truncated?)')
    return rec


def frange(files):
    if not files:
        return '(no files)'
    if len(files) == 1:
        return f'`{files[0]}` (1 file)'
    return f'`{files[0]}` .. `{files[-1]}` ({len(files)} files)'


def pct(a, b, d=1):
    return f'{100.0 * a / b:.{d}f}%' if b else '-'


def watched_period(recs, w):
    """Median tick period of the watched CPU (us), from its tick span."""
    ps = []
    for r in recs:
        kw = r['cpu'].get(w, {})
        tn, tf, tl = (num(kw.get('tick_n')), num(kw.get('tick_first_us')),
                      num(kw.get('tick_last_us')))
        if tn and tn >= 2 and tf is not None and tl is not None:
            ps.append((tl - tf) / (tn - 1))
    return med(ps), len(ps)


def fmt_ms(us):
    return '-' if us is None else f'{us / 1000.0:.1f}'


def med(xs):
    return statistics.median(xs) if xs else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--head', default=None)
    ap.add_argument('--owner', type=int, default=1)
    ap.add_argument('--per-run', action='store_true')
    ap.add_argument('label')
    ap.add_argument('files', nargs='+')
    a = ap.parse_args()

    recs, bad = [], []
    for fn in sorted(a.files):
        r = parse_file(fn)
        if a.head and (r['banner'] is None or
                       not (r['banner'].startswith(a.head) or
                            a.head.startswith(r['banner']))):
            r['problems'].append(f'banner {r["banner"]} != head {a.head}')
        (bad if r['problems'] else recs).append(r)
    names = [r['file'] for r in recs]
    print(f'## Cell {a.label}\n')
    print(f'Counted: {frange(names)}; head {a.head or "(not checked)"}.')
    if bad:
        print(f'Left out ({len(bad)} files):')
        for r in bad:
            print(f'- `{r["file"]}`: ' + '; '.join(r['problems']))
    if not recs:
        return
    hdr0 = recs[0]['hdr']
    print(f'Format {recs[0]["fmt"]}; mode {hdr0.get("mode")}, depth '
          f'{hdr0.get("depth")}, work {hdr0.get("work")}, sustain_ms '
          f'{hdr0.get("sustain_ms")}, cpus {hdr0.get("cpus")}.')
    for r in recs:
        if r['hdr'].get('mode') != hdr0.get('mode') or \
           r['hdr'].get('sustain_ms') != hdr0.get('sustain_ms'):
            print(f'- NOTE `{r["file"]}` has mode {r["hdr"].get("mode")} '
                  f'sustain_ms {r["hdr"].get("sustain_ms")}')
    cpus = int(hdr0.get('cpus', '0'))
    owner = a.owner
    thieves = [c for c in range(cpus) if c != owner]
    v2 = all(r['fmt'] != 'v1' for r in recs)

    # ---- T1
    agg = {c: {k: 0 for k in ['exec', 'att'] + OUTCOMES} for c in range(cpus)}
    shares = {c: [] for c in range(cpus)}
    first_ok = {c: [] for c in range(cpus)}
    ok0 = {c: [] for c in range(cpus)}
    for r in recs:
        tot = sum(num(r['cpu'][c].get('exec')) or 0 for c in range(cpus))
        for c in range(cpus):
            kv = r['cpu'][c]
            for k in agg[c]:
                agg[c][k] += num(kv.get(k)) or 0
            shares[c].append(100.0 * (num(kv.get('exec')) or 0) / tot
                             if tot else 0.0)
            fo = num(kv.get('first_ok_us'))
            if fo is not None:
                first_ok[c].append(fo)
            if kv.get('ok') == '0':
                ok0[c].append(r['file'])
    tot_exec = sum(agg[c]['exec'] for c in range(cpus))
    print(f'\n### T1 per CPU ({frange(names)})\n')
    print('| CPU | median share | pooled share | exec | att | ok | '
          + ' | '.join(f'{k} % att' for k in OUTCOMES)
          + ' | first success median / max (ms) | runs with ok=0 |')
    print('|' + '---|' * (8 + len(OUTCOMES)))
    for c in range(cpus):
        g = agg[c]
        fo = first_ok[c]
        fo_s = (f'{fmt_ms(med(fo))} / {fmt_ms(max(fo))} ({len(fo)} runs)'
                if fo else 'none')
        o0 = (f'{len(ok0[c])} of {len(recs)}'
              + (f' ({", ".join(x for x in ok0[c][:6])}'
                 f'{", ..." if len(ok0[c]) > 6 else ""})' if ok0[c] and
                 c != owner else ''))
        print(f'| {c}{" (owner)" if c == owner else ""} | '
              f'{med(shares[c]):.1f}% | {pct(g["exec"], tot_exec)} | '
              f'{g["exec"]} | {g["att"]} | {g["ok"]} | '
              + ' | '.join(pct(g[k], g['att']) for k in OUTCOMES)
              + f' | {fo_s} | {o0} |')
    best = max(thieves, key=lambda c: med(shares[c]))
    print(f'\nBest thief by median share: CPU {best} '
          f'({med(shares[best]):.1f}%). Ratio of each thief\'s median share '
          'to it: ' + ', '.join(f'CPU {c} {med(shares[c]) / med(shares[best]):.3f}'
                                for c in thieves if c != best) + '.')
    zero_runs = sorted({f for c in thieves for f in ok0[c]})
    print(f'Runs with a non-owner CPU at ok=0: {len(zero_runs)} of {len(recs)}'
          + (f' ({", ".join(zero_runs)})' if zero_runs else '') + '.')

    if a.per_run:
        print(f'\n#### Per run: share % and ok per CPU, first_ok (ms)\n')
        print('| file | ' + ' | '.join(f'CPU{c} share / ok / first ms'
                                       for c in range(cpus)) + ' |')
        print('|' + '---|' * (cpus + 1))
        for r in recs:
            tot = sum(num(r['cpu'][c].get('exec')) or 0 for c in range(cpus))
            cells = []
            for c in range(cpus):
                kv = r['cpu'][c]
                e = num(kv.get('exec')) or 0
                cells.append(f'{100.0 * e / tot if tot else 0:.1f} / '
                             f'{kv.get("ok")} / '
                             f'{fmt_ms(num(kv.get("first_ok_us")))}')
            print(f'| `{r["file"]}` | ' + ' | '.join(cells) + ' |')

    if not v2:
        print('\n(format v1: no window or phase fields; T2 to T4 skipped)')
        return
    w = recs[0]['watch'] if recs[0]['watch'] is not None else owner

    # ---- T2
    print(f'\n### T2 thieves against CPU {w}\'s preempt_disabled window and '
          f'tick ({frange(names)})\n')
    print('Inside window = attempts that started with preempt_disabled[w] set.'
          ' Phase columns are % of the attempts that had a watched tick.\n')
    tsum = {}
    for c in thieves:
        s = {'att': 0, 'f_pd': 0, 'w_in': [0] * 5, 'w_in_unk': 0,
             'w_off': [0] * 8, 'ph_w': [0] * 11, 'ph_w_pd': [0] * 11,
             'ph_own': [0] * 11}
        for r in recs:
            kv = r['cpu'][c]
            s['att'] += num(kv.get('att')) or 0
            s['f_pd'] += num(kv.get('f_pd')) or 0
            s['w_in_unk'] += num(kv.get('w_in_unk')) or 0
            for k in ('w_in', 'w_off', 'ph_w', 'ph_w_pd', 'ph_own'):
                v = ints(kv.get(k))
                if v:
                    s[k] = [x + y for x, y in zip(s[k], v)]
        tsum[c] = s
    print('| thief | att | inside window | ' + ' | '.join(
        f'in {r}' for r in REASONS) + ' | in, no window | f_pd % att |')
    print('|' + '---|' * (5 + len(REASONS)))
    for c in thieves:
        s = tsum[c]
        inside = sum(s['w_in']) + s['w_in_unk']
        print(f'| {c} | {s["att"]} | {pct(inside, s["att"])} ({inside}) | '
              + ' | '.join(f'{x}' for x in s['w_in'])
              + f' | {s["w_in_unk"]} | {pct(s["f_pd"], s["att"])} |')
    print('\nOffset into the open window (w_off), % of in-window attempts:\n')
    print('| thief | ' + ' | '.join(WOFF_LABELS) + ' |')
    print('|' + '---|' * (1 + len(WOFF_LABELS)))
    for c in thieves:
        s = tsum[c]
        t = sum(s['w_off'])
        print(f'| {c} | ' + ' | '.join(pct(x, t) for x in s['w_off']) + ' |')
    per_us, per_n = watched_period(recs, w)
    uni = uniform_ph(per_us or 10000.0)
    print(f'\nUniform reference uses CPU {w}\'s median tick period '
          f'{per_us or 10000.0:.1f} us ({per_n} runs).')
    for key, title in (('ph_w', f'attempt time after CPU {w}\'s last tick'),
                       ('ph_w_pd', f'attempts ending on the early-out, time '
                                   f'after CPU {w}\'s last tick'),
                       ('ph_own', 'attempt time after the thief\'s own tick')):
        print(f'\n{key}: {title}, % per bin; last column = within 200 us\n')
        print('| thief | n | ' + ' | '.join(PH_LABELS) + ' | <200us |')
        print('|' + '---|' * (3 + len(PH_LABELS)))
        for c in thieves:
            v = tsum[c][key]
            t = sum(v)
            print(f'| {c} | {t} | ' + ' | '.join(pct(x, t) for x in v)
                  + f' | {pct(sum(v[:5]), t)} |')
        print('| uniform | - | ' + ' | '.join(f'{x:.1f}%' for x in uni)
              + f' | {sum(uni[:5]):.1f}% |')

    # ---- T3
    print(f'\n### T3 tick phase against CPU {w} (from tick_first_us) '
          f'({frange(names)})\n')
    print('Offset = (tick_first_us[c] - tick_first_us[w]) mod period, us.\n')
    offs = {c: [] for c in range(cpus)}
    periods = []
    for r in recs:
        kw = r['cpu'][w]
        tn, tf, tl = (num(kw.get('tick_n')), num(kw.get('tick_first_us')),
                      num(kw.get('tick_last_us')))
        if not tn or tn < 2 or tf is None or tl is None:
            continue
        period = (tl - tf) / (tn - 1)
        periods.append(period)
        for c in range(cpus):
            tfc = num(r['cpu'][c].get('tick_first_us'))
            if tfc is not None:
                offs[c].append((tfc - tf) % period)
    if periods:
        print(f'Watched tick period median {med(periods):.1f} us over '
              f'{len(periods)} runs.\n')
        print('| CPU | runs | offset median | min | max |')
        print('|---|---|---|---|---|')
        for c in range(cpus):
            o = offs[c]
            if o:
                print(f'| {c} | {len(o)} | {med(o):.0f} | {min(o):.0f} | '
                      f'{max(o):.0f} |')

    # ---- T4
    print(f'\n### T4 CPU {w}\'s windows ({frange(names)})\n')
    n = [0] * 5
    ssum = [0] * 5
    mx = [0] * 5
    wph = [0] * 11
    wall = 0
    log_reason = {}
    log_n = 0
    for r in recs:
        kv = r['cpu'][w]
        for i, x in enumerate(ints(kv.get('win_n')) or [0] * 5):
            n[i] += x
        for i, x in enumerate(ints(kv.get('win_sum_us')) or [0] * 5):
            ssum[i] += x
        for i, x in enumerate(ints(kv.get('win_max_us')) or [0] * 5):
            mx[i] = max(mx[i], x)
        for i, x in enumerate(ints(kv.get('wph')) or [0] * 11):
            wph[i] += x
        wall += r['wall_us'] or 0
        wl = kv.get('wlog', '-')
        if wl != '-':
            for ent in wl.split(','):
                parts = ent.split(':')
                if len(parts) == 3:
                    log_reason[parts[2]] = log_reason.get(parts[2], 0) + 1
                    log_n += 1
    print('| reason | windows | total us | mean us | longest us | '
          '% of wall-clock |')
    print('|---|---|---|---|---|---|')
    for i, rn in enumerate(REASONS):
        print(f'| {rn} | {n[i]} | {ssum[i]} | '
              f'{ssum[i] / n[i] if n[i] else 0:.1f} | {mx[i]} | '
              f'{pct(ssum[i], wall, 3)} |')
    print(f'| all | {sum(n)} | {sum(ssum)} | '
          f'{sum(ssum) / sum(n) if sum(n) else 0:.1f} | {max(mx)} | '
          f'{pct(sum(ssum), wall, 3)} |')
    t = sum(wph)
    print(f'\nWindow entry after CPU {w}\'s own tick (wph), % of {t} windows:\n')
    print('| ' + ' | '.join(PH_LABELS) + ' | <200us |')
    print('|' + '---|' * (1 + len(PH_LABELS)))
    print('| ' + ' | '.join(pct(x, t) for x in wph)
          + f' | {pct(sum(wph[:5]), t)} |')
    print(f'\nwlog entries: {log_n} over {len(recs)} runs; by reason: '
          + ', '.join(f'{k} {v}' for k, v in sorted(log_reason.items())) + '.')


if __name__ == '__main__':
    main()
